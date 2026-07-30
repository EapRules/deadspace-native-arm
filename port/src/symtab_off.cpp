/*
 * 32-bit off_t, because the game has one and the host does not.
 *
 * Same class of failure as src/symtab_time.cpp, and for the same reason:
 * Debian trixie's armhf is built with 64-bit time_t *and* _FILE_OFFSET_BITS=64,
 * so on the build image sizeof(off_t) == 8. bionic's armeabi off_t is 4 bytes
 * and Dead Space was compiled against that. Binding the game's mmap/lseek/
 * ftruncate straight to the host's - THUNK_DIRECT(mmap) etc. in
 * thunks/libc/generated/impl_tab.h, which is right on any distro that has not
 * made the switch - silently disagrees about the *register layout* of the
 * call, not just about the width of one field.
 *
 * How it showed up. The engine's allocator (EA's PPMalloc, GeneralAllocator)
 * asks the system for a 16 MB core block. Its acquire routine at text+0x2533ec
 * tries sbrk first and falls back to mmap; the first allocation out of
 * operator new returned NULL and the game stored through it:
 *
 *     16ccb0: bl 16cc14              @ operator new -> GeneralAllocator::Malloc
 *     16ccb4: lsr r7, r7, #1
 *   > 16ccb8: str r0, [r6, r7, lsl #2]   @ r6 = 0, faults at 0x0000000c
 *
 * Instrumenting the two entry points the allocator can reach (temporary
 * logging thunks over sbrk/mmap, since qemu -strace shows no syscall at all
 * for a call glibc rejects in userspace) printed:
 *
 *     DBG sbrk(0)        = 0x1b55000
 *     DBG sbrk(16781312) = 0x1b55000
 *     DBG mmap(nil, 16781312, 3, 34, -1, 16781312) = 0xffffffff
 *
 * Two separate things are visible there and only the second one is fixed here.
 *
 *   1. sbrk returns the *old* break under glibc. This code wants the *new*
 *      one - it does base = sbrk(0), end = sbrk(n) and rejects the result when
 *      base >= end - which is what bionic's sbrk returned at the time this
 *      game was built (AOSP fixed that return value years later). So the sbrk
 *      path can never succeed here and the allocator correctly falls through
 *      to its mmap fallback. That is a designed fallback, not a bug: the cost
 *      is one wasted 16 MB break extension per core block. Left alone
 *      deliberately - it is a different defect (return-value semantics, not
 *      ABI) and the allocator works without it.
 *
 *   2. The mmap that fallback makes never reaches the kernel. Look at the
 *      offset in the trace: it equals the length, and the game passes zero.
 *      The game emits the bionic call - fd at [sp+0], a single 32-bit offset
 *      word at [sp+4] (text+0x253194: str ip,[sp] ; str r4,[sp,#4]) - while
 *      the host's mmap wants an 8-byte, 8-byte-aligned off_t and therefore
 *      reads [sp+8..15], which the caller reserved but never wrote. glibc sees
 *      a garbage offset, fails the page-alignment check in its own wrapper and
 *      returns MAP_FAILED without issuing a syscall. The allocator then has no
 *      core at all and every Malloc returns NULL.
 *
 * lseek and ftruncate are the other two off_t imports of this game and are
 * broken the same way, worse in fact: a 64-bit argument must land in an
 * even-aligned register pair, so the host reads r2:r3 for an offset the game
 * put in r1 and the whence argument is off by a slot. They are fixed together
 * with mmap rather than separately, because it is one disagreement - the width
 * of off_t - and splitting it would leave known-miscompiled calls in the table
 * waiting to fault during asset loading instead of during allocation.
 *
 * (fopen/fseek/ftell/open are also in the game's import list but take long or
 * are variadic, and long is 32 bits on both sides; they are not affected.)
 *
 * These entries come before the generated libc table in so_dynamic_libraries
 * so they win the lookup.
 */
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>

#include "trace.h"

#include "so_util.h"
#include "thunk_gen.h"

extern "C" {

typedef int32_t bionic_off_t;

/*
 * The offset is sign-extended rather than zero-extended: bionic's off_t is
 * signed, and a negative offset has to stay negative so the host rejects it
 * the way the kernel would have. Forwarded to mmap64 instead of mmap so the
 * conversion is explicit at the call site and does not depend on whatever
 * _FILE_OFFSET_BITS this file happens to be compiled with.
 *
 * prot and flags cross unchanged: PROT_* and the MAP_* bits the game uses
 * (MAP_PRIVATE 0x02, MAP_ANONYMOUS 0x20 - the 34 in the trace above) have the
 * same values in bionic's ARM headers as in the host's.
 */
void *bionic_mmap(void *addr, size_t length, int prot, int flags,
                  int fd, bionic_off_t offset)
{
    void *r = mmap64(addr, length, prot, flags, fd, (off64_t)offset);

    /*
     * Traced behind an env var rather than left to be re-derived.
     *
     * This is the call EA's GeneralAllocator makes to acquire a core block, and
     * when it fails every Malloc in the engine returns NULL - which surfaces
     * far away as an empty std::vector, or an object with a null vtable, and
     * looks like anything but an allocation failure. The comment at the top of
     * this file describes exactly that hunt. One env var makes the next one a
     * single run.
     */
    if (getenv("DEADSPACE_TRACE_MMAP"))
        trace("mmap(%p, %zu, prot=%d, flags=%d, fd=%d, off=%d) = %p%s",
              addr, length, prot, flags, fd, (int)offset, r,
              r == MAP_FAILED ? "  MAP_FAILED" : "");
    return r;
}

/*
 * Returning off_t as well as taking it, so both directions narrow.
 *
 * A result past 2 GiB is reported as EOVERFLOW instead of being truncated:
 * bionic's lseek is the raw 32-bit syscall and the kernel answers -EOVERFLOW
 * for exactly this case, so the game already has to cope with it. Truncating
 * would instead hand back a plausible-looking small offset and corrupt the
 * next read. No asset in this game is anywhere near that size, so this branch
 * is a guard rather than a code path.
 */
bionic_off_t bionic_lseek(int fd, bionic_off_t offset, int whence)
{
    off64_t r = lseek64(fd, (off64_t)offset, whence);
    if (r == (off64_t)-1)
        return -1;
    if (r > INT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (bionic_off_t)r;
}

int bionic_ftruncate(int fd, bionic_off_t length)
{
    return ftruncate64(fd, (off64_t)length);
}

} /* extern "C" */

DynLibFunction symtable_off[] = {
    /* No float crosses these boundaries, so no softfp bridge is generated;
     * what matters is that the *host* prototype is never seen by the game. */
    THUNK_SPECIFIC("mmap",      bionic_mmap),
    THUNK_SPECIFIC("lseek",     bionic_lseek),
    THUNK_SPECIFIC("ftruncate", bionic_ftruncate),
    { NULL, 0 },
};
