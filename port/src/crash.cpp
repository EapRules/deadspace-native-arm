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

#include <signal.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

static uintptr_t   g_text_base = 0;
static size_t      g_text_size = 0;
static const char *g_soname    = "module";

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
        if (v < g_text_base || v >= g_text_base + g_text_size)
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

static void on_fault(int sig, siginfo_t *si, void *ucontext)
{
    const ucontext_t *uc = (const ucontext_t *)ucontext;

    put("\nFATAL: ");
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
