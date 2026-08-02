/*
 * POSIX semaphores, in bionic's layout.
 *
 * ---------------------------------------------------------------------------
 * The failure
 *
 * Once com/eamobile/Query let the engine past its content gate it died every
 * run, at the same instruction, inside its own allocator:
 *
 *     FATAL: SIGSEGV at 0x00000008
 *            pc = ...+0x002539f8   str r1, [r3, #8]
 *            lr = ...+0x00256278
 *            r0 = <the GeneralAllocator>  r2 = 0x10c  r3 = 0x00000000
 *
 * +0x2539f8 is PPMalloc's insert-into-bin. It computes the bin from the chunk
 * size (`lsr r4, r2, #3` at +0x2539c4, so bin = size >> 3), loads the bin's
 * link pointer, and stores through it. Reading the allocator out of the core
 * dump showed the object is fully initialised - mbInitialized is 1 and all 127
 * bins are self-linked - except bin 0, which is the one bin PPMalloc's
 * InitBins loop deliberately skips (+0x254d68 starts at index 1). Landing in
 * bin 0 means size >> 3 == 0, i.e. a chunk whose size field is zero, and the
 * chunk at r1 confirmed it: its whole header, and forty bytes around it, read
 * back as zeros.
 *
 * So this was never an allocator bug. Something zeroed a live chunk header.
 *
 * ---------------------------------------------------------------------------
 * What zeroed it
 *
 * The port already carries four fixes of one shape - a bionic type that is
 * smaller than the host's, bound straight through, so the host writes past the
 * end of what the game reserved (pthread_attr_t 24 vs 36, pthread_mutex_t 4 vs
 * 24, struct timeval 8 vs 16, struct stat 104 vs the time64 one). sem_t is the
 * fifth and it is the worst of them:
 *
 *     bionic/libc/include/semaphore.h, ILP32:
 *         typedef struct { volatile unsigned int count; } sem_t;   // 4 bytes
 *     glibc/arm-linux-gnueabihf:
 *         union { char __size[16]; long int __align; } sem_t;      // 16 bytes
 *
 * thunks/libc/generated/impl_tab.h binds all nine sem_* entries with
 * THUNK_DIRECT, which is correct on any libc whose sem_t is one word and wrong
 * here. sem_init() then writes sixteen bytes - a small count followed by
 * twelve zeros - into a four-byte object. The engine allocates its semaphores
 * out of its own heap, so those twelve zero bytes land on whatever PPMalloc
 * put next, which is the following chunk's prev_size and size fields. The
 * allocator survives until the next time it walks that bin, and then binds a
 * zero-size chunk into bin 0 and stores through a null.
 *
 * That is why the fault looked like allocator corruption from inside the
 * allocator and had nothing to do with it, and why it moved to a different
 * milestone rather than a different address: the overwrite is deterministic,
 * only its discovery is deferred.
 *
 * ---------------------------------------------------------------------------
 * The bridge
 *
 * Same shape as the pthread_mutex_t bridge in src/symtab_pthread.cpp: the four
 * bytes the game owns hold a pointer to a real host sem_t allocated on the
 * side. It is simpler than the mutex case in one respect - POSIX has no static
 * initialiser for a semaphore, sem_init is always called explicitly - so there
 * is no "is this word a pointer or a bionic init constant?" ambiguity to
 * resolve, and no lazy creation path.
 *
 * A semaphore copied by value would copy the pointer and end up shared, and
 * destroying either copy would leave the other dangling. Not defended against,
 * deliberately: copying a sem_t is undefined behaviour in POSIX too, the
 * mutex bridge next to this one made the same call, and a guard would cost a
 * lookup on every sem_wait in the engine's audio path.
 */
#include <errno.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "so_util.h"
#include "thunk_gen.h"
#include "trace.h"

extern "C" {

/* bionic/libc/include/semaphore.h, 32-bit. One word, and the game reserves
 * exactly this much - on its stack, in its structs, and in its heap. */
struct BIONIC_sem_t {
    void *host;
};

static_assert(sizeof(struct BIONIC_sem_t) == 4,
              "bionic's sem_t is a single 32-bit word on armeabi; anything "
              "wider here would reintroduce the overwrite this file fixes");

/*
 * bionic's struct timespec: time_t is 32 bits. Duplicated from
 * src/symtab_time.cpp rather than shared, because that file's copy is part of
 * its own argument-conversion story and a header shared between them would
 * suggest the two are meant to stay in step with something. They are both just
 * transcriptions of the same ABI.
 */
struct bionic_timespec {
    int32_t tv_sec;
    int32_t tv_nsec;
};

static inline sem_t *host_of(struct BIONIC_sem_t *s)
{
    return s ? (sem_t *)s->host : NULL;
}

int bionic_sem_init(struct BIONIC_sem_t *s, int pshared, unsigned int value)
{
    if (!s)
        return (errno = EINVAL), -1;

    sem_t *host = (sem_t *)calloc(1, sizeof(*host));
    if (!host)
        return (errno = ENOMEM), -1;

    if (sem_init(host, pshared, value) != 0) {
        free(host);
        return -1;
    }

    /*
     * pshared is passed through rather than forced to 0. It is always 0 in
     * this game - the semaphores live in one process - but a bridge that
     * silently downgraded it would be a landmine for the next port to reuse
     * this file.
     */
    s->host = host;

    static int announced = 0;
    if (announced < 4) {
        announced++;
        trace("sem_init(%p, pshared=%d, value=%u) -> host sem at %p",
              (void *)s, pshared, value, (void *)host);
    }
    return 0;
}

int bionic_sem_destroy(struct BIONIC_sem_t *s)
{
    sem_t *host = host_of(s);
    if (!host)
        return (errno = EINVAL), -1;

    int rc = sem_destroy(host);
    free(host);
    s->host = NULL;
    return rc;
}

int bionic_sem_wait(struct BIONIC_sem_t *s)
{
    sem_t *host = host_of(s);
    if (!host)
        return (errno = EINVAL), -1;
    return sem_wait(host);
}

int bionic_sem_trywait(struct BIONIC_sem_t *s)
{
    sem_t *host = host_of(s);
    if (!host)
        return (errno = EINVAL), -1;
    return sem_trywait(host);
}

int bionic_sem_post(struct BIONIC_sem_t *s)
{
    sem_t *host = host_of(s);
    if (!host)
        return (errno = EINVAL), -1;
    return sem_post(host);
}

int bionic_sem_getvalue(struct BIONIC_sem_t *s, int *value)
{
    sem_t *host = host_of(s);
    if (!host || !value)
        return (errno = EINVAL), -1;
    return sem_getvalue(host, value);
}

/*
 * The second half of the same bug, and the reason this one is not just a
 * pointer indirection: the deadline is a struct timespec, which is 8 bytes on
 * the game's side and 16 on this host. Passing the game's through unconverted
 * makes the host read tv_nsec out of the caller's tv_sec and the top half of
 * the seconds out of whatever follows - a deadline in the far future or the
 * far past, either of which turns a timed wait into a hang or a busy loop.
 *
 * progress notes from the previous iteration listed sem_timedwait as one of
 * three time64 holes "not called yet". It is called now.
 */
int bionic_sem_timedwait(struct BIONIC_sem_t *s, const struct bionic_timespec *abs)
{
    sem_t *host = host_of(s);
    if (!host || !abs)
        return (errno = EINVAL), -1;

    struct timespec deadline;
    deadline.tv_sec  = (time_t)abs->tv_sec;
    deadline.tv_nsec = (long)abs->tv_nsec;
    return sem_timedwait(host, &deadline);
}

} /* extern "C" */

DynLibFunction symtable_sem[] = {
    THUNK_SPECIFIC("sem_init",      bionic_sem_init),
    THUNK_SPECIFIC("sem_destroy",   bionic_sem_destroy),
    THUNK_SPECIFIC("sem_wait",      bionic_sem_wait),
    THUNK_SPECIFIC("sem_trywait",   bionic_sem_trywait),
    THUNK_SPECIFIC("sem_post",      bionic_sem_post),
    THUNK_SPECIFIC("sem_getvalue",  bionic_sem_getvalue),
    THUNK_SPECIFIC("sem_timedwait", bionic_sem_timedwait),
    { NULL, 0 },
};
