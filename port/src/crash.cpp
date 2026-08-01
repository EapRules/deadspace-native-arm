/*
 * Fault reporting for the loaded module. See crash.h.
 *
 * The module is mapped at a base the loader chooses at runtime and it is not a
 * file the debugger can open anyway - it lives inside the user's APK. So the
 * one thing worth printing is the faulting PC *relative to the module's text*,
 * which is directly usable:
 *
 *     unzip -p deadspace.apk lib/armeabi-v7a/libdeadspace.so > /tmp/g.so
 *     arm-linux-gnueabihf-objdump -d --start-address=0x<off-0x20> \
 *         --stop-address=0x<off+0x20> /tmp/g.so
 *
 * Everything below runs inside a signal handler on whichever thread faulted,
 * so it uses write(2) and hand-rolled hex only: no printf, no malloc, no
 * locks. After reporting it restores the default action and re-raises, so the
 * process still dies exactly as it would have.
 */
#include "crash.h"

#include <fcntl.h>
#include <sys/syscall.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

static uintptr_t   g_text_base = 0;
static size_t      g_text_size = 0;
static const char *g_soname    = "module";

/*
 * Only the first thread to fault gets to report.
 *
 * This engine runs several threads and they do not fault politely one at a
 * time. When two go down together, both handlers run concurrently and both
 * write to fd 2 a few bytes at a time, and the two reports interleave into
 * something like
 *
 *     r0libEAMGameDeadSpace.so = +0x57bc27640x003e7198
 *
 * which is not a corrupted register, it is two lines braided together. That
 * cost an evening: it reads as though the crash handler itself had faulted, so
 * the real report - the only evidence of where the engine died - looked
 * untrustworthy when it was simply mixed with another one.
 *
 * A later thread is not silenced entirely, because "a second thread also
 * faulted" is itself worth knowing; it gets one atomic line and then dies.
 */
static volatile int g_reporting = 0;

static void put(const char *s)
{
    ssize_t n = write(2, s, strlen(s));
    (void)n;
}

static void put_hex(uintptr_t v)
{
    char buf[10] = { '0', 'x' };
    for (int i = 0; i < 8; i++)
        buf[9 - i] = "0123456789abcdef"[(v >> (4 * i)) & 0xf];
    ssize_t n = write(2, buf, sizeof(buf));
    (void)n;
}

/* One register, plus where it lands inside the module if it lands there. */
static void put_reg(const char *name, uintptr_t v)
{
    put("       ");
    put(name);
    put(" = ");
    put_hex(v);
    if (g_text_size && v >= g_text_base && v < g_text_base + g_text_size) {
        put("  (");
        put(g_soname);
        put("+");
        put_hex(v - g_text_base);
        put(")");
    }
    put("\n");
}

static const char *signal_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGILL:  return "SIGILL";
    case SIGFPE:  return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default:      return "signal";
    }
}

/*
 * A return-address scan of the faulting stack.
 *
 * There is no unwinder here: the module was mapped by our own loader, so it is
 * in no dl_iterate_phdr list and .ARM.exidx never gets registered. Frame
 * pointers are out too - the game is built -fomit-frame-pointer like every
 * NDK release.
 *
 * What is left is the crude method, and for this job it is enough: walk the
 * stack word by word and print everything that points into the module's text.
 * It reports call sites that already returned as well as live frames, so it is
 * a list of suspects rather than a backtrace - but every real frame is in it,
 * and that is what turns "SIGSEGV somewhere in the C++ runtime" into a
 * specific engine function to disassemble.
 *
 * Thumb return addresses have bit 0 set; the offsets are printed with it
 * cleared so they can be pasted straight into objdump --start-address.
 */
/*
 * Is this word plausibly a return address, or just data that happens to fall
 * inside the module?
 *
 * The range check alone is not enough, and the reason is a detail of the
 * loader: text_size is the whole PT_LOAD segment, not just .text, so every
 * pointer-shaped constant in .rodata and .data passes it. A report full of
 * those is worse than no report - one run appeared to show an infinite
 * recursion between two addresses that objdump says are not code at all, and
 * that cost time to disbelieve.
 *
 * A real return address always has a call immediately before it. This game is
 * ARM throughout - no Thumb in the disassembly - so the four forms that can
 * precede one are cheap to recognise:
 *
 *     BL / BLcond      cond 101 1 <imm24>
 *     BLX <imm>        1111 101 <H> <imm24>
 *     BLX <reg>        cond 0001 0010 ... 0011 <Rm>
 *     ldr pc, [...]    the `mov lr, pc ; ldr pc, [r3]` virtual-call pair this
 *                      engine uses everywhere, where lr points just past the
 *                      load
 *
 * False positives are still possible - data can encode a valid BL - but they
 * drop from "most lines" to "rare", which is the difference between a list of
 * suspects and noise.
 */
static bool looks_like_return_address(uintptr_t v)
{
    uint32_t prev = *(const uint32_t *)((v & ~(uintptr_t)1) - 4);

    if ((prev & 0x0F000000) == 0x0B000000) return true;  /* BL / BLcond   */
    if ((prev & 0xFE000000) == 0xFA000000) return true;  /* BLX immediate */
    if ((prev & 0x0FFFFFF0) == 0x012FFF30) return true;  /* BLX register  */
    if ((prev & 0x0E10F000) == 0x0410F000) return true;  /* ldr pc, [..]  */

    return false;
}

static void put_stack(uintptr_t sp)
{
    if (!sp || !g_text_size)
        return;

    put("       stack (module return addresses, innermost first):\n");

    /* Bounded so a corrupted sp cannot walk off the mapping and fault us a
     * second time inside the handler, where the report would be lost. */
    const uintptr_t *w   = (const uintptr_t *)(sp & ~(uintptr_t)3);
    const int        max = 1024;   /* 4 KiB of stack */
    int              hits = 0;

    for (int i = 0; i < max && hits < 24; i++) {
        uintptr_t v = w[i];
        if (v < g_text_base + 4 || v >= g_text_base + g_text_size)
            continue;
        if (!looks_like_return_address(v))
            continue;
        put("         ");
        put(g_soname);
        put("+");
        put_hex((v & ~(uintptr_t)1) - g_text_base);
        put("\n");
        hits++;
    }

    if (!hits)
        put("         (none - the fault is not below any module frame)\n");
}

/*
 * A file descriptor on /dev/null, opened once at init, used to ask the kernel
 * whether an address is readable.
 *
 * write() validates its buffer and returns EFAULT rather than delivering a
 * signal, which is the one way to probe memory from inside a signal handler
 * without risking a second fault. Everything else - reading it directly,
 * installing a temporary handler, msync - either faults or is not
 * async-signal-safe.
 */
static int g_null_fd = -1;

static bool readable(const void *p, size_t len)
{
    if (g_null_fd < 0 || !p)
        return false;
    return write(g_null_fd, p, len) == (ssize_t)len;
}

/*
 * Four words at a pointer, when the pointer is readable.
 *
 * The registers alone say where a fault happened; they do not say what the
 * program thought it had. This port spent hours on a fault whose whole
 * explanation was "the std::vector at r12 has begin == 0" - three words that
 * were sitting in memory the entire time and that no report ever printed.
 */
static void put_words(const char *name, uintptr_t v)
{
    if (!readable((const void *)v, 16))
        return;

    const uintptr_t *w = (const uintptr_t *)v;
    put("       [");
    put(name);
    put("] = ");
    for (int i = 0; i < 4; i++) {
        put_hex(w[i]);
        put(i == 3 ? "\n" : " ");
    }
}

static void on_fault(int sig, siginfo_t *si, void *ucontext)
{
    const ucontext_t *uc = (const ucontext_t *)ucontext;

    /*
     * __sync_lock_test_and_set rather than a mutex: this is a signal handler
     * and pthread_mutex_lock is not async-signal-safe. One atomic word is, and
     * it is all the exclusion needed - the loser never writes again.
     */
    if (__sync_lock_test_and_set(&g_reporting, 1)) {
        put("\nFATAL: a second thread faulted while the first was still "
            "reporting; its details are omitted so the report above stays "
            "readable.\n");
        /*
         * Wait, do not die.
         *
         * The obvious thing here is to restore SIG_DFL and re-raise, and it is
         * wrong: this thread dies, the process goes with it, and the winner is
         * killed mid-sentence. The first attempt at this fix produced
         *
         *     pc = 0x4f66f384  (libEAMGameDeadSpace.so
         *
         * - the report truncated exactly where the other thread pulled the
         * floor out. So the loser parks instead and lets the winner finish and
         * take the process down. Bounded, because a winner that never finishes
         * must not turn a crash into a hang: after two seconds this thread
         * gives up and dies itself, which at least preserves the exit status.
         */
        for (int i = 0; i < 200; i++) {
            struct timespec ts = { 0, 10 * 1000 * 1000 };  /* 10 ms */
            nanosleep(&ts, NULL);
        }
        struct sigaction dfl2;
        memset(&dfl2, 0, sizeof(dfl2));
        dfl2.sa_handler = SIG_DFL;
        sigaction(sig, &dfl2, NULL);
        raise(sig);
        return;
    }

    /*
     * Which thread. This engine runs six of them and the fault is on a worker,
     * not on the frame path - a fact that took three sessions to establish
     * because nothing in the report distinguished them. syscall(SYS_gettid) is
     * async-signal-safe; pthread_self is not, and gettid() the glibc wrapper
     * did not exist before 2.30.
     */
    put("\nFATAL: [tid ");
    put_hex((uintptr_t)syscall(SYS_gettid));
    put("] ");
    put(signal_name(sig));
    put(" at ");
    put_hex((uintptr_t)(si ? si->si_addr : 0));
    put("\n");

    if (uc) {
        const mcontext_t *m = &uc->uc_mcontext;
        put_reg("pc", m->arm_pc);
        put_reg("lr", m->arm_lr);
        put_reg("sp", m->arm_sp);
        put_reg("r0", m->arm_r0);
        put_reg("r1", m->arm_r1);
        put_reg("r2", m->arm_r2);
        put_reg("r3", m->arm_r3);
        put_reg("ip", m->arm_ip);

        /* What the registers point at, which is usually the actual answer. */
        put_words("r0", m->arm_r0);
        put_words("r1", m->arm_r1);
        put_words("r2", m->arm_r2);
        put_words("r3", m->arm_r3);
        put_words("ip", m->arm_ip);
        put_words("sp", m->arm_sp);
    }

    put("       module text at ");
    put_hex(g_text_base);
    put("\n");

    if (uc)
        put_stack(uc->uc_mcontext.arm_sp);

    /* Die the way we would have died without this handler. */
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigaction(sig, &dfl, NULL);
    raise(sig);
}

extern "C" void crash_report_init(so_module *mod, const char *soname)
{
    if (mod) {
        g_text_base = mod->text_base;
        g_text_size = mod->text_size;
    }
    if (soname)
        g_soname = soname;

    /* O_WRONLY so a probe never actually consumes anything. */
    g_null_fd = open("/dev/null", O_WRONLY);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags     = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);

    /*
     * SIGABRT too. glibc's heap checks ("free(): invalid pointer",
     * "malloc(): corrupted top size") abort() instead of faulting, and without
     * this the process just dies with "uncaught target signal 6" and no clue
     * about which module frame handed the bad pointer to free(). The stack
     * scan below is exactly as useful there as it is for a SIGSEGV.
     */
    sigaction(SIGABRT, &sa, NULL);
}
