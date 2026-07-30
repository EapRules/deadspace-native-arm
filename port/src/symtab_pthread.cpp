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
     * Off by default now, and the measurement that changed it is worth keeping.
     *
     * When this was added the run faulted in two different places on identical
     * runs, and delaying every engine thread by 1500 ms made it land in one
     * place reliably - the race was real. It is no longer: with the ABI fixes
     * that followed (va_arg promotion, the ldrd alignment trap, and the
     * dispatch-arity mismatch, all in the JNI machinery), 0 ms and 5000 ms per
     * thread now produce a byte-identical fault at +0x30a744.
     *
     * So the race was almost certainly never the engine's. It was ours: garbage
     * arguments reaching JNI methods, which is exactly the kind of corruption
     * whose timing depends on which thread got there first. Keeping a nine
     * second startup delay to hide a bug that no longer exists would be
     * shipping a workaround for someone else's fixed problem.
     *
     * Left reachable rather than deleted, because the Vita port needed it and
     * this build is the same: if a non-deterministic fault ever returns, set
     * DEADSPACE_LATE_THREAD_MS and find out in one run whether it is a race.
     */
    int ms = env ? atoi(env) : 0;
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

    if (!attr)
        return pthread_create(thread, NULL, entry, arg);

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
 * Only the four entry points libdeadspace.so actually imports are overridden;
 * pthread_mutex_trylock and friends are left to the bridge because the game
 * never calls them and an unused override is an untested one.
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
static pthread_mutex_t g_mutex_bootstrap = PTHREAD_MUTEX_INITIALIZER;

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

    { NULL, 0 },
};
