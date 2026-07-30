/*
 * The pthread objects whose bionic layout the host does not share. Two of
 * them: pthread_attr_t, below, and pthread_mutex_t, further down.
 *
 * pthread_attr_t, because bionic's is 24 bytes and glibc's is 36.
 *
 * bionic (ILP32) declares the attribute object inline:
 *
 *     typedef struct {
 *       uint32_t flags;  void *stack_base;   size_t stack_size;
 *       size_t guard_size;  int32_t sched_policy;  int32_t sched_priority;
 *     } pthread_attr_t;                                  // 24 bytes
 *
 * glibc declares it as `union { char __size[36]; long __align; }`. The
 * generated bionic table binds pthread_attr_init straight to the host's, and
 * the host's zeroes all 36 bytes - into the 24 the game reserved.
 *
 * native_app_glue puts that object on the stack:
 *
 *     android_app_create():
 *         pthread_attr_t attr;                  // 24 bytes of frame
 *         pthread_attr_init(&attr);             // host writes 36
 *         pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
 *         pthread_create(&app->thread, &attr, android_app_entry, app);
 *
 * so the twelve extra bytes land on whatever the compiler put after it -
 * here, the stack canary - and the process dies in __stack_chk_fail during
 * ANativeActivity_onCreate, before the game thread has done anything. Exactly
 * the same failure shape as clock_gettime in symtab_time.cpp, and just as
 * silent: nothing in the log mentions pthread.
 *
 * So the attribute object is kept in the game's layout on the game's stack,
 * and translated into a real host pthread_attr_t only at pthread_create.
 *
 * These entries come before the generated libc table in so_dynamic_libraries
 * so they win the lookup.
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <time.h>
#include "trace.h"
#include "so_util.h"
#include "thunk_gen.h"
#include "thunk_pthread.h"

/* Defined in src/main.cpp, with C++ linkage - so declared outside the
 * extern "C" block below or the two would not be the same symbol. */
struct so_module;
extern so_module *deadspace_module(void);

extern "C" {

/* bionic's armeabi timespec: two 32-bit words. The host's is two 64-bit ones.
 * Declared here rather than shared with symtab_sem.cpp so each file stays
 * readable on its own; the layout is fixed by the game's ABI, not by us. */
static pthread_mutex_t g_mutex_bootstrap = PTHREAD_MUTEX_INITIALIZER;

struct bionic_timespec {
    int32_t tv_sec;
    int32_t tv_nsec;
};

struct bionic_pthread_attr {
    uint32_t flags;
    void    *stack_base;
    uint32_t stack_size;
    uint32_t guard_size;
    int32_t  sched_policy;
    int32_t  sched_priority;
};

/* bionic/pthread_internal.h */
enum {
    BIONIC_PTHREAD_ATTR_FLAG_DETACHED = 0x00000001,
    BIONIC_PTHREAD_ATTR_FLAG_INHERIT  = 0x00000004,
};

/* bionic's own defaults for a 32-bit process. */
static const uint32_t kBionicDefaultStack = 1 * 1024 * 1024;
static const uint32_t kBionicDefaultGuard = 4096;

int bionic_pthread_attr_init(struct bionic_pthread_attr *attr)
{
    if (!attr)
        return EINVAL;

    memset(attr, 0, sizeof(*attr));
    attr->stack_size    = kBionicDefaultStack;
    attr->guard_size    = kBionicDefaultGuard;
    attr->sched_policy  = SCHED_OTHER;
    attr->sched_priority = 0;
    return 0;
}

int bionic_pthread_attr_destroy(struct bionic_pthread_attr *attr)
{
    if (!attr)
        return EINVAL;
    memset(attr, 0, sizeof(*attr));
    return 0;
}

int bionic_pthread_attr_setdetachstate(struct bionic_pthread_attr *attr, int state)
{
    if (!attr)
        return EINVAL;

    /* PTHREAD_CREATE_JOINABLE == 0 and PTHREAD_CREATE_DETACHED == 1 on both
     * sides, so the constant crosses unchanged; only the storage differs. */
    if (state == PTHREAD_CREATE_DETACHED)
        attr->flags |= BIONIC_PTHREAD_ATTR_FLAG_DETACHED;
    else if (state == PTHREAD_CREATE_JOINABLE)
        attr->flags &= ~(uint32_t)BIONIC_PTHREAD_ATTR_FLAG_DETACHED;
    else
        return EINVAL;

    return 0;
}

int bionic_pthread_attr_getdetachstate(const struct bionic_pthread_attr *attr, int *state)
{
    if (!attr || !state)
        return EINVAL;
    *state = (attr->flags & BIONIC_PTHREAD_ATTR_FLAG_DETACHED)
                 ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE;
    return 0;
}

int bionic_pthread_attr_setstacksize(struct bionic_pthread_attr *attr, uint32_t size)
{
    if (!attr || size < 16384)
        return EINVAL;
    attr->stack_size = size;
    return 0;
}

int bionic_pthread_attr_getstacksize(const struct bionic_pthread_attr *attr, uint32_t *size)
{
    if (!attr || !size)
        return EINVAL;
    *size = attr->stack_size;
    return 0;
}

int bionic_pthread_attr_setguardsize(struct bionic_pthread_attr *attr, uint32_t size)
{
    if (!attr)
        return EINVAL;
    attr->guard_size = size;
    return 0;
}

/*
 * The counterpart: read the game's attribute object and build a host one.
 *
 * The generated table's pthread_create thunk ignores the attribute argument
 * entirely - safe, since it cannot parse it, but it means a thread the game
 * detached stays joinable and is never reaped. native_app_glue detaches its
 * thread and never joins it, so honouring the flag is what keeps a long
 * session from accumulating dead thread descriptors.
 *
 * The stack size is deliberately *not* honoured downwards. bionic's 1 MB
 * default is sized for a thread that only ever runs the game; here the same
 * thread also runs host GL, SDL and libc code that Android's linker never had
 * to fit in that budget, so the host default is used unless the game asked
 * for more.
 */
/*
 * One thread in this engine has to start late, and this is where that is done.
 *
 * The symptom is the one that is hardest to act on: the run faults in a
 * *different place each time*. Two crash sites alternated across otherwise
 * identical runs - +0x30a744 on the main stack, +0x5f384 on another - and a fix
 * for one would appear to work and then stop working, because what actually
 * changed between runs was thread scheduling, not the fix.
 *
 * The Vita port hit the same thing and its patch.c hooks the entry trampoline
 * at +0x320624 to sleep before running, with the comment "a thread that causes
 * some kind of undefined behaviour and crashes in different places if not
 * delayed". That trampoline is four instructions -
 *
 *     push {r4, lr} ; ldr r3,[r0] ; mov lr,pc ; ldr pc,[r3] ; mov r0,#0
 *
 * - a C++ thread object's start routine: call virtual method zero on the
 * argument, return 0. Which means it does not need a binary hook at all. It
 * arrives here as the `entry` argument of pthread_create, so recognising it by
 * address and delaying the *creation* has the same effect with none of the
 * trampoline machinery, and stays visible in a stack trace.
 *
 * This is a workaround and it is worth being plain about that: the underlying
 * defect is a startup race inside the engine that nobody has diagnosed, on
 * either platform. Delaying makes it lose the race reliably instead of
 * sometimes. DEADSPACE_LATE_THREAD_MS exists so the next person can find out
 * how much margin there really is without rebuilding, and setting it to 0 turns
 * the workaround off to check whether it is still needed.
 */
#define LATE_THREAD_OFFSET 0x320624

static bool is_late_thread(void *(*entry)(void *))
{
    so_module *mod = deadspace_module();
    if (!mod || !entry)
        return false;

    /* Which entry point, by module-relative offset. Empty means "every thread
     * the engine starts", which is the setting to reach for when narrowing
     * down whether a race exists at all before finding out whose it is. */
    const char *want = getenv("DEADSPACE_LATE_THREAD_AT");
    if (!want || !*want)
        return (uintptr_t)entry >= mod->text_base &&
               (uintptr_t)entry <  mod->text_base + mod->text_size;

    unsigned long off = strtoul(want, NULL, 0);
    return (uintptr_t)entry == mod->text_base + off;
}

static void delay_late_thread(void)
{
    const char *env = getenv("DEADSPACE_LATE_THREAD_MS");
    /*
     * On by default, and the sample size is the whole story here.
     *
     * This was added when the run faulted in two different places on identical
     * runs - +0x30a744 and +0x5f384 - and delaying every engine thread made it
     * land in one place, which is what made the fault chaseable at all.
     *
     * It was then removed, on the evidence of two runs at 0 ms and two at
     * 5000 ms that all faulted identically. That was wrong: two runs cannot
     * distinguish "deterministic" from "usually lands here". Six runs each say:
     *
     *     no delay      5 x +0x30a744, 1 x +0x5f384
     *     1500 ms       6 x +0x30a744
     *
     * The race is still there. It is just rare enough that a small sample looks
     * clean, which is the trap this variable exists to survive - and the reason
     * the removal commit's claim of a byte-identical fault should be read as
     * what it was, a conclusion drawn from four data points.
     *
     * Still a workaround, and still not shippable at this cost: 1500 ms times
     * six threads is nine seconds of startup. What it buys is a reproducible
     * fault, which is worth more right now than a fast one. Narrowing it to the
     * single thread that matters is what DEADSPACE_LATE_THREAD_AT is for, and 0
     * turns it off to re-measure.
     */
    int ms = env ? atoi(env) : 1500;
    if (ms <= 0)
        return;

    /* Deliberately does not name an offset. It used to print
     * LATE_THREAD_OFFSET, which is the Vita port's hook target and not what is
     * being delayed here at all - that address never arrives as a pthread
     * entry. A log line that names the wrong thing is worse than one that names
     * nothing: the pthread_create trace above already prints the real entry
     * point, immediately before this. */
    trace("  ^ delayed %d ms before starting (engine startup race; "
          "DEADSPACE_LATE_THREAD_MS=0 disables)", ms);

    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/*
 * Name the thread that was just created, by the same id the crash handler
 * prints.
 *
 * The entry-point trace above says which function a thread runs; it does not
 * say which of the six running threads faulted. Matching a crash to its entry
 * point is the difference between "a worker thread dies" and "the thread that
 * runs +0x24c914 dies", and only one of those can be acted on.
 *
 * The tid is read from the child's own descriptor rather than passed in,
 * because there is no portable way to ask for it before the thread starts.
 * pthread_getattr_np would give the stack, not the id; this uses the pthread_t
 * as the key instead, which the crash handler cannot see - so both are printed
 * and the stack bounds tie them together.
 */
static void report_thread(int rc, pthread_t *thread, void *(*entry)(void *))
{
    if (rc != 0 || !thread)
        return;

    so_module *mod = deadspace_module();
    unsigned   off = 0;
    if (mod && (uintptr_t)entry >= mod->text_base &&
        (uintptr_t)entry <  mod->text_base + mod->text_size)
        off = (unsigned)((uintptr_t)entry - mod->text_base);

    void  *stack = NULL;
    size_t size  = 0;
    pthread_attr_t a;
    if (pthread_getattr_np(*thread, &a) == 0) {
        pthread_attr_getstack(&a, &stack, &size);
        pthread_attr_destroy(&a);
    }

    trace("  ^ pthread_t=%p stack=%p..%p",
          (void *)*thread, stack, (char *)stack + size);
    (void)off;
}


int bionic_pthread_create(pthread_t *thread, const struct bionic_pthread_attr *attr,
                          void *(*entry)(void *), void *arg)
{
    /* Name every thread the engine starts, module-relative. Guessing which
     * entry point matters is what wasted the first attempt at the delay below:
     * the trampoline the Vita port hooks never arrives here, and one line per
     * creation says so in a single run instead of a session of disassembly. */
    {
        so_module *mod = deadspace_module();
        if (mod && (uintptr_t)entry >= mod->text_base &&
            (uintptr_t)entry <  mod->text_base + mod->text_size)
            trace("pthread_create: entry at +0x%08x, arg=%p",
                  (unsigned)((uintptr_t)entry - mod->text_base), arg);
        else
            trace("pthread_create: entry at %p (outside the module)", (void *)entry);
    }

    if (is_late_thread(entry))
        delay_late_thread();

    if (!attr) {
        int rc0 = pthread_create(thread, NULL, entry, arg);
        report_thread(rc0, thread, entry);
        return rc0;
    }

    pthread_attr_t host;
    if (pthread_attr_init(&host) != 0)
        return pthread_create(thread, NULL, entry, arg);

    if (attr->flags & BIONIC_PTHREAD_ATTR_FLAG_DETACHED)
        pthread_attr_setdetachstate(&host, PTHREAD_CREATE_DETACHED);

    size_t host_default = 0;
    pthread_attr_getstacksize(&host, &host_default);
    if (attr->stack_size > host_default)
        pthread_attr_setstacksize(&host, attr->stack_size);

    int rc = pthread_create(thread, &host, entry, arg);
    pthread_attr_destroy(&host);
    report_thread(rc, thread, entry);
    return rc;
}

/*
 * Mutexes, because bionic's pthread_mutex_t is one 32-bit word and glibc's is
 * twenty-four bytes.
 *
 * The game cannot own a real host mutex in the space it reserved, so
 * gmloader-next's bridge (thunks/libc/pthread.cpp) keeps the host mutex on the
 * heap and stores the pointer in that word. Sound - except for how it decides
 * whether a mutex has been created yet: it tests the word against zero, and
 * zero is only one of bionic's three static initialisers.
 *
 *     PTHREAD_MUTEX_INITIALIZER             { 0      }
 *     PTHREAD_RECURSIVE_MUTEX_INITIALIZER   { 0x4000 }
 *     PTHREAD_ERRORCHECK_MUTEX_INITIALIZER  { 0x8000 }
 *
 * A statically initialised *recursive* mutex therefore reaches the bridge
 * holding 0x4000, the bridge reads that as an already-allocated pointer, and
 * glibc dereferences address 0x4000.
 *
 * That is where Dead Space died, before onCreate and before anything was logged:
 * the C++ runtime it links statically guards its function-local statics with
 *
 *     static pthread_mutex_t guard = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
 *
 * (libdeadspace.so ships it at .data+0x1210c0, word = 0x4000), and the engine's
 * xt::ReflectType registration hits the first such static from an .init_array
 * constructor - so the fault happened during so_load_module, with the log
 * still showing nothing but "Linking libdeadspace.so...".
 *
 * Minigore 2 never tripped this: its build of the same engine has no
 * statically initialised recursive mutex, so every word the bridge ever saw
 * was 0.
 *
 * The word is therefore decoded here instead, and this table comes before the
 * generated libc one in so_dynamic_libraries, so these win the lookup.
 *
 * The claim that used to close this comment - that only four entry points are
 * imported and "pthread_mutex_trylock and friends are left to the bridge
 * because the game never calls them" - was wrong, and wrong in the expensive
 * direction. The game imports trylock and calls it three times, twice inside
 * EAThread's Mutex on the same object it locks two instructions earlier.
 * Bound straight to glibc, it reads our heap pointer as __lock, __kind from a
 * live field of the game's object, and on success writes __owner and __nusers
 * over twenty bytes the game owns.
 *
 * The lesson is not about trylock. "The game never calls it" is a claim about
 * the binary and belongs in a readelf output, not in a comment - and the same
 * sentence was covering pthread_mutexattr_setpshared, pthread_attr_setstack
 * and pthread_attr_setschedparam, all three imported, all three called from
 * the thread-creation path that every worker in the engine goes through.
 */

/* bionic/pthread.h: the mutex type lives in bits 14-15 of the value word. */
enum {
    BIONIC_MUTEX_TYPE_MASK       = 0xc000,
    BIONIC_MUTEX_TYPE_RECURSIVE  = 0x4000,
    BIONIC_MUTEX_TYPE_ERRORCHECK = 0x8000,
};

/*
 * How a static initialiser is told apart from a pointer we stored.
 *
 * Every value bionic can put in that word statically fits in sixteen bits,
 * and nothing we hand back can be that low: Linux refuses to map below
 * vm.mmap_min_addr, which is 64 KiB on every kernel these handhelds run.
 */
static const uintptr_t kBionicInitWordMax = 0xffff;

/*
 * Serialises the lazy creation below. A statically initialised mutex has no
 * init call to hang the allocation off, so the first lock is where it has to
 * happen - and two threads can arrive there at once.
 */

static pthread_mutex_t *host_mutex(BIONIC_pthread_mutex_t *m)
{
    if (!m)
        return NULL;

    if ((uintptr_t)m->real_mtx > kBionicInitWordMax)
        return m->real_mtx;

    pthread_mutex_lock(&g_mutex_bootstrap);

    /* Re-read under the lock: another thread may have created it meanwhile. */
    uintptr_t word = (uintptr_t)m->real_mtx;
    if (word <= kBionicInitWordMax) {
        pthread_mutex_t *host = (pthread_mutex_t *)calloc(1, sizeof(*host));
        if (host) {
            /* The type is the whole point: a recursive guard mutex locked as
             * a normal one self-deadlocks the first time the engine re-enters
             * it, which is what __cxa_guard_acquire does by design. */
            pthread_mutexattr_t attr;
            pthread_mutexattr_init(&attr);
            switch (word & BIONIC_MUTEX_TYPE_MASK) {
            case BIONIC_MUTEX_TYPE_RECURSIVE:
                pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
                break;
            case BIONIC_MUTEX_TYPE_ERRORCHECK:
                pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
                break;
            default:
                pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
                break;
            }
            pthread_mutex_init(host, &attr);
            pthread_mutexattr_destroy(&attr);
            m->real_mtx = host;
        }
    }

    pthread_mutex_unlock(&g_mutex_bootstrap);
    return m->real_mtx;
}

int bionic_pthread_mutex_init(BIONIC_pthread_mutex_t *m, pthread_mutexattr_t **attr)
{
    if (!m)
        return EINVAL;

    /*
     * The incoming word is deliberately NOT looked at.
     *
     * An earlier version tried to be tidy here: if the word already held a
     * pointer above kBionicInitWordMax it destroyed and freed that host mutex
     * first, so re-initialising would not leak. That reasoning has a hole -
     * it assumes the word means something on entry, and on a freshly
     * malloc'd mutex it does not.
     *
     * Dead Space allocates its mutexes with the engine's own allocator, which
     * is plain memalign - no zeroing:
     *
     *     xt::FileWatcher::FileWatcher+0x7c   mov  r0, #4
     *                                         bl   xt::MemoryManager::allocMemory
     *                                         mov  r1, #0
     *                                         bl   pthread_mutex_init@plt
     *
     * Those four bytes are whatever the previous owner of the chunk left
     * there - in practice a stale heap pointer, which sails past the
     * kBionicInitWordMax test. The shim then called free() on a pointer that
     * was never allocated as a mutex, and glibc aborted the process with
     * "free(): invalid pointer" a few allocations later.
     *
     * Bionic itself never reads the word in pthread_mutex_init: the mutex is
     * inline there, so initialising simply overwrites it. Matching that is
     * both correct and the only safe option. Re-initialising an already
     * initialised mutex is undefined behaviour in POSIX and neither the game
     * nor bionic does it, so the leak this used to prevent cannot happen -
     * and even if it did, a leaked 24-byte mutex beats a dead process.
     */
    pthread_mutex_t *host = (pthread_mutex_t *)calloc(1, sizeof(*host));
    if (!host)
        return ENOMEM;

    /* The double indirection is gmloader-next's convention, not ours: its
     * pthread_mutexattr_init bridge stores a host attribute object in the
     * word the game reserved. libdeadspace.so imports no pthread_mutexattr_*
     * function at all and only ever passes NULL here. */
    int rc = pthread_mutex_init(host, attr ? *attr : NULL);
    if (rc != 0) {
        free(host);
        return rc;
    }

    m->real_mtx = host;
    return 0;
}

int bionic_pthread_mutex_destroy(BIONIC_pthread_mutex_t *m)
{
    if (!m)
        return EINVAL;

    /* A mutex that was only ever statically initialised and never locked has
     * nothing behind it, and destroying it is still legal. */
    if ((uintptr_t)m->real_mtx > kBionicInitWordMax) {
        pthread_mutex_destroy(m->real_mtx);
        free(m->real_mtx);
    }

    m->value = 0;
    return 0;
}

int bionic_pthread_mutex_lock(BIONIC_pthread_mutex_t *m)
{
    pthread_mutex_t *host = host_mutex(m);
    return host ? pthread_mutex_lock(host) : EINVAL;
}

int bionic_pthread_mutex_unlock(BIONIC_pthread_mutex_t *m)
{
    pthread_mutex_t *host = host_mutex(m);
    return host ? pthread_mutex_unlock(host) : EINVAL;
}

} /* extern "C" */

/* ------------------------------------------------------------------ *
 * The four calls the comment above wrongly assumed nobody made.
 *
 * Each one operates on an object this loader keeps in a layout the host does
 * not share, and each was bound straight to glibc. None of them crash where
 * they are called - they corrupt an object that faults later, somewhere else,
 * which is why they survived this long. The Vita port wraps all four
 * (vita-ref/loader/reimpl/pthr.c) against this same library.
 * ------------------------------------------------------------------ */

/*
 * trylock has to follow the same indirection lock/unlock do: the game's word
 * holds a pointer to the host mutex, and host_mutex() decodes bionic's static
 * initialisers on the way. Three call sites, two of them on the same object
 * that pthread_mutex_lock takes twelve instructions later.
 */
int bionic_pthread_mutex_trylock(BIONIC_pthread_mutex_t *m)
{
    pthread_mutex_t *host = host_mutex(m);
    if (!host)
        return EINVAL;
    return pthread_mutex_trylock(host);
}

/*
 * setpshared, likewise. glibc implements it as a read-modify-write that sets
 * bit 31 of the word it is given, and under this bridge that word is a heap
 * pointer - so a PROCESS_SHARED request turns 0x000b1020 into 0x800b1020 and
 * the next pthread_mutex_init dereferences it. The engine asks for
 * PROCESS_SHARED on one of its two branches.
 */
int bionic_pthread_mutexattr_setpshared(pthread_mutexattr_t **attr_ptr, int pshared)
{
    if (!attr_ptr || !*attr_ptr)
        return EINVAL;
    return pthread_mutexattr_setpshared(*attr_ptr, pshared);
}

/*
 * setstack and setschedparam write through glibc's pthread_attr layout into an
 * object this file deliberately keeps in bionic's. The offsets overlap without
 * overflowing, so nothing faults - the fields simply land on the wrong ones:
 *
 *   setschedparam writes the priority at offset 0, which is bionic's `flags`,
 *   erasing the DETACHED bit that pthread_attr_setdetachstate set two calls
 *   earlier. Every thread the engine starts then leaks its descriptor and its
 *   stack, because nothing ever joins them.
 *
 *   setstack writes stackaddr and stacksize at 16 and 20, which are bionic's
 *   sched_policy and sched_priority, while the size the engine asked for never
 *   reaches `stack_size` at offset 8 - so bionic_pthread_create below compares
 *   against the host default, loses, and the thread runs on a stack the engine
 *   did not choose while the buffer it allocated is simply abandoned.
 *
 * Both are in EA::Thread::Thread::Begin, between setdetachstate and
 * pthread_create. Every worker goes through them.
 */
int bionic_pthread_attr_setstack(struct bionic_pthread_attr *attr,
                                 void *stack_base, uint32_t stack_size)
{
    if (!attr || !stack_base || stack_size < 16384)
        return EINVAL;
    attr->stack_base = stack_base;
    attr->stack_size = stack_size;
    return 0;
}

int bionic_pthread_attr_setschedparam(struct bionic_pthread_attr *attr,
                                      const int *param)
{
    if (!attr || !param)
        return EINVAL;
    /* bionic's sched_param is a single int, and so is the host's on ARM; only
     * where it lands differs. */
    attr->sched_priority = *param;
    return 0;
}



/*
 * pthread_cond_timedwait, because the game's struct timespec is eight bytes and
 * this host's is sixteen.
 *
 * Debian trixie's armhf is built with _TIME_BITS=64, so the host's time_t is
 * 64-bit and its timespec is {int64 tv_sec; int64 tv_nsec}. bionic's armeabi is
 * {int32; int32}. This is the same disagreement src/symtab_time.cpp fixed for
 * clock_gettime and nanosleep and src/symtab_sem.cpp fixed for sem_timedwait -
 * and this one call was left behind, forwarding the pointer untouched.
 *
 * The disassembly shows the caller reading only two words:
 *
 *     24bf18  ldr r5, [r2, #4]    ; tv_nsec
 *     24bf1c  ldr lr, [ip, #3740] ; the "no timeout" sentinel
 *     24bf40  bl  pthread_cond_timedwait@plt
 *
 * so glibc composes tv_sec from the game's {tv_sec, tv_nsec} pair and reads
 * tv_nsec from eight bytes of the caller's frame that were never written. The
 * deadline lands either far in the future - the waiting thread never wakes - or
 * in the past, or glibc rejects it with EINVAL and the caller, which only
 * distinguishes ETIMEDOUT from "error", spins. All three are a hung or spinning
 * subsystem with nothing in the log, which is the failure mode this port has
 * been chasing on a worker thread.
 *
 * EA::Thread::Condition::Wait is the only call site.
 */
int bionic_pthread_cond_timedwait(pthread_cond_t **cnd, BIONIC_pthread_mutex_t *m,
                                  const struct bionic_timespec *abs)
{
    pthread_mutex_t *host = host_mutex(m);
    if (!cnd || !host || !abs)
        return EINVAL;

    /*
     * Lazily created, and serialised - which the bridge's version is not.
     *
     * The engine never calls pthread_cond_init: readelf lists it as UND with no
     * relocation at all, and EA::Thread::Condition's constructor simply zeroes
     * two words. So every condvar in this game is created on first use, and a
     * condvar is by definition touched from more than one thread - the waiter
     * and the signaller are never the same. Two of them arriving at a zero word
     * together each allocate one, and one of the two is then referenced by
     * nobody: a lost wakeup, a thread parked forever, no crash and nothing in
     * the log.
     *
     * Same reasoning and same lock as host_mutex() below, which took
     * g_mutex_bootstrap for exactly this case and whose comment does not
     * mention that condvars have it worse.
     */
    if (!*cnd) {
        pthread_mutex_lock(&g_mutex_bootstrap);
        if (!*cnd) {                       /* re-read under the lock */
            pthread_cond_t *c = (pthread_cond_t *)calloc(1, sizeof(pthread_cond_t));
            if (!c) {
                pthread_mutex_unlock(&g_mutex_bootstrap);
                return ENOMEM;
            }
            pthread_cond_init(c, NULL);
            __atomic_store_n(cnd, c, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&g_mutex_bootstrap);
    }

    struct timespec ts;
    ts.tv_sec  = (time_t)abs->tv_sec;
    ts.tv_nsec = (long)abs->tv_nsec;
    return pthread_cond_timedwait(*cnd, host, &ts);
}

DynLibFunction symtable_pthread[] = {
    THUNK_SPECIFIC("pthread_attr_init",           bionic_pthread_attr_init),
    THUNK_SPECIFIC("pthread_attr_destroy",        bionic_pthread_attr_destroy),
    THUNK_SPECIFIC("pthread_attr_setdetachstate", bionic_pthread_attr_setdetachstate),
    THUNK_SPECIFIC("pthread_attr_getdetachstate", bionic_pthread_attr_getdetachstate),
    THUNK_SPECIFIC("pthread_attr_setstacksize",   bionic_pthread_attr_setstacksize),
    THUNK_SPECIFIC("pthread_attr_getstacksize",   bionic_pthread_attr_getstacksize),
    THUNK_SPECIFIC("pthread_attr_setguardsize",   bionic_pthread_attr_setguardsize),
    THUNK_SPECIFIC("pthread_create",              bionic_pthread_create),

    THUNK_SPECIFIC("pthread_mutex_init",          bionic_pthread_mutex_init),
    THUNK_SPECIFIC("pthread_mutex_destroy",       bionic_pthread_mutex_destroy),
    THUNK_SPECIFIC("pthread_mutex_lock",          bionic_pthread_mutex_lock),
    THUNK_SPECIFIC("pthread_mutex_unlock",        bionic_pthread_mutex_unlock),
    THUNK_SPECIFIC("pthread_mutex_trylock",       bionic_pthread_mutex_trylock),
    THUNK_SPECIFIC("pthread_cond_timedwait",      bionic_pthread_cond_timedwait),
    THUNK_SPECIFIC("pthread_mutexattr_setpshared", bionic_pthread_mutexattr_setpshared),
    THUNK_SPECIFIC("pthread_attr_setstack",       bionic_pthread_attr_setstack),
    THUNK_SPECIFIC("pthread_attr_setschedparam",  bionic_pthread_attr_setschedparam),

    { NULL, 0 },
};
