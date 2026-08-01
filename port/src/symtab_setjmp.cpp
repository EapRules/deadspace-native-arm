/*
 * setjmp/longjmp, in bionic's jmp_buf layout.
 *
 * ---------------------------------------------------------------------------
 * The failure
 *
 * Every run died before the first frame, always at the same instruction,
 * inside the engine's own free():
 *
 *     FATAL: SIGSEGV at 0xfffffff1
 *            pc = ...+0x0039d26c   ldr ip, [r1, #-16]      r1 = 0x00000001
 *            lr = ...+0x0039d33c
 *
 * +0x39d25c is a member Free(this, ptr): it reads the 16-byte chunk header at
 * ptr-16 and compares its magic against 0xaa53c5aa. Two instructions earlier
 * it does `cmp r1, #0 / beq`, so the pointer was not null - it was exactly 1,
 * which is the shape of a boolean that ended up in a pointer slot.
 *
 * lr says the caller is +0x39d324, and that function is the teardown half of
 * the object built at +0x39d114: it frees this->[0x114], then this->[0x118],
 * then this->[0x11c] and so on. So the field at +0x114 of a live object held
 * the value 1.
 *
 * ---------------------------------------------------------------------------
 * What the object actually looks like
 *
 * Dumping it out of the qemu core dump (0x52e73f78, 0x170 bytes) shows the
 * constructor's own values everywhere except that one word:
 *
 *     +0x000  ab1500ff        the magic +0x39d160 writes
 *     +0x008  00000060        the element count
 *     +0x014  <r4-r11, sp, lr and stack addresses>
 *     +0x114  00000001        <- freed as a pointer, faults
 *     +0x118  00000000
 *     +0x14c  01 01 01 01 01 01 01   the seven flags the ctor sets to 1
 *     +0x168  aa005501        the error code +0x39d248 stores before longjmp
 *
 * +0x14 is a jmp_buf: the error path at +0x39d238 does `add r0, r3, #20` and
 * branches to longjmp@plt, so byte 20 of this object is what the engine hands
 * longjmp. And the next field starts at +0x114, which is 0x14 + 256 - the game
 * reserved exactly 256 bytes for it.
 *
 *     bionic/libc/include/setjmp.h, arm:
 *         #define _JBLEN 64
 *         typedef long jmp_buf[_JBLEN];              // 256 bytes
 *     glibc/arm-linux-gnueabihf:
 *         struct __jmp_buf_tag {
 *             __jmp_buf __jmpbuf;                    // 256 bytes
 *             int       __mask_was_saved;            // at +256
 *             __sigset_t __saved_mask;               // 128 more, at +260
 *         };                                         // 388 bytes
 *
 * thunks/libc/generated/impl_tab.h binds setjmp with THUNK_DIRECT, so the game
 * was calling glibc's, which is __sigsetjmp(env, 1): it saves the registers,
 * then sets __mask_was_saved = 1 at offset 256. That word is the engine's
 * next field. The 1 the port has been chasing is glibc's flag, landing on a
 * pointer the destructor later frees.
 *
 * The core dump corroborates the rest of it too. __saved_mask follows at +260,
 * and only its first eight bytes get written, because sigprocmask hands the
 * kernel a sigsetsize of 8 and glibc does not clear the remaining 120 - which
 * is exactly why +0x118 reads back as zero while the seven flags at +0x14c
 * still hold the constructor's 1s. A full 128-byte write would have flattened
 * them, and the fact that it did not is what pins this on __mask_was_saved
 * specifically rather than on a generic overrun.
 *
 * ---------------------------------------------------------------------------
 * Why this is the fifth instance of one bug, not a new one
 *
 * The port already carries four bridges of the same shape - pthread_attr_t
 * (24 vs 36), pthread_mutex_t (4 vs 24), struct timeval (8 vs 16), sem_t
 * (4 vs 16). Each is a bionic type smaller than the host's, bound straight
 * through, so the host writes past the end of what the game reserved. jmp_buf
 * is the same bug with the largest gap yet, and the hardest to see, because
 * the overwrite is 132 bytes past a 256-byte object and lands on a field that
 * is only read much later, in a destructor, on a different call stack.
 *
 * ---------------------------------------------------------------------------
 * Why assembly, and not the usual side-table bridge
 *
 * sem_t and pthread_mutex_t are fixed by keeping a host object on the side and
 * storing a pointer to it in the bytes the game owns. That cannot work here:
 * setjmp returns twice, so a C wrapper that called the host's setjmp would
 * have longjmp resume into the wrapper's frame, which is gone. setjmp has to
 * be the function the game calls, which means writing it.
 *
 * It saves the ten core words bionic saves (r4-r11, sp, lr) plus the eight
 * callee-saved VFP doubles - 26 words, comfortably inside the 64 the game
 * reserved, and it writes nothing else. d8-d15 are included because the game
 * is full of `vpush {d8-d15}` and bionic's own arm setjmp saves them; leaving
 * them out would only show up as arithmetic quietly resuming with the wrong
 * values after a longjmp, which is not a failure anyone would trace back here.
 *
 * Not saved, deliberately: the signal mask. That is the whole point - bionic's
 * setjmp does not save it either (its sigsetjmp does, in the spare word), and
 * the game imports setjmp and longjmp only, no sig* variant.
 */
#include <stdint.h>

#include "so_util.h"
#include "thunk_gen.h"

extern "C" {

int  bionic_setjmp(void *env);
void bionic_longjmp(void *env, int value);

}

/*
 * .arm on both: the game is ARM-only code and calls these through a PLT slot,
 * so the address in the table must not carry a Thumb bit. ip is used to stage
 * sp because putting sp in an stm register list is deprecated from ARMv7 on
 * and assembles to a warning on this toolchain.
 */
asm(
    ".text\n"
    ".align  2\n"
    ".arm\n"

    ".global bionic_setjmp\n"
    ".type   bionic_setjmp, %function\n"
"bionic_setjmp:\n"
    "   mov     ip, sp\n"
    "   stmia   r0!, {r4-r11, ip, lr}\n"
    "   vstmia  r0!, {d8-d15}\n"
    "   mov     r0, #0\n"
    "   bx      lr\n"
    ".size   bionic_setjmp, .-bionic_setjmp\n"

    ".global bionic_longjmp\n"
    ".type   bionic_longjmp, %function\n"
"bionic_longjmp:\n"
    /* longjmp(env, 0) must still make setjmp return non-zero. */
    "   mov     r2, r0\n"
    "   movs    r0, r1\n"
    "   moveq   r0, #1\n"
    "   ldmia   r2!, {r4-r11}\n"
    "   ldr     r3, [r2], #4\n"   /* the saved sp */
    "   ldr     lr, [r2], #4\n"
    "   vldmia  r2!, {d8-d15}\n"
    "   mov     sp, r3\n"
    "   bx      lr\n"
    ".size   bionic_longjmp, .-bionic_longjmp\n"
);

DynLibFunction symtable_setjmp[] = {
    NO_THUNK("setjmp",  (uintptr_t)&bionic_setjmp),
    NO_THUNK("longjmp", (uintptr_t)&bionic_longjmp),
    { NULL, 0 },
};
