/*
 * 32-bit time_t, because the game has one and the host does not.
 *
 * Debian trixie moved armhf to 64-bit time_t. bionic's armeabi-v7a time_t
 * stayed 32-bit, and Dead Space was compiled against it. Binding the game's
 * clock_gettime straight to the host's - which is what the generated bionic
 * table does, and what works on distros that have not made the switch - hands
 * the kernel a 16-byte struct timespec to fill in where the game reserved 8:
 *
 *     xt::Time::getSeconds():
 *         sub  sp, #16
 *         str  <canary>, [sp, #12]
 *         add  r1, sp, #4        ; struct timespec
 *         blx  clock_gettime
 *
 * The extra eight bytes land on the stack canary and the process dies in
 * __stack_chk_fail before the game has drawn anything. It is not a subtle
 * failure, but it is a silent one: nothing in the log points at time_t.
 *
 * Every function here therefore takes the game's layout, calls the host with
 * the host's, and narrows across the boundary. These entries come before the
 * generated libc table in so_dynamic_libraries so they win the lookup.
 *
 * Narrowing is lossless until 2038; past that the game sees a wrapped clock,
 * exactly as it would on a real armeabi-v7a device.
 */
#include <stdint.h>
#include <time.h>

#include "so_util.h"
#include "thunk_gen.h"

extern "C" {

typedef int32_t bionic_time_t;

struct bionic_timespec {
    int32_t tv_sec;
    int32_t tv_nsec;
};

/* struct tm is identical on both sides (nine ints, then long + char*), so it
 * crosses unchanged; only the time_t on either end of it needs narrowing. */

int bionic_clock_gettime(int clk_id, struct bionic_timespec *ts)
{
    struct timespec host;
    int rc = clock_gettime(clk_id, &host);
    if (rc == 0 && ts) {
        ts->tv_sec  = (int32_t)host.tv_sec;
        ts->tv_nsec = (int32_t)host.tv_nsec;
    }
    return rc;
}

int bionic_nanosleep(const struct bionic_timespec *req, struct bionic_timespec *rem)
{
    struct timespec host_req, host_rem;
    if (!req)
        return clock_gettime(CLOCK_MONOTONIC, &host_rem); /* propagate EFAULT */

    host_req.tv_sec  = req->tv_sec;
    host_req.tv_nsec = req->tv_nsec;

    int rc = nanosleep(&host_req, &host_rem);
    if (rem) {
        rem->tv_sec  = (int32_t)host_rem.tv_sec;
        rem->tv_nsec = (int32_t)host_rem.tv_nsec;
    }
    return rc;
}

bionic_time_t bionic_time(bionic_time_t *out)
{
    bionic_time_t now = (bionic_time_t)time(NULL);
    if (out)
        *out = now;
    return now;
}

struct tm *bionic_localtime(const bionic_time_t *t)
{
    time_t host = t ? (time_t)*t : 0;
    return localtime(&host);
}

bionic_time_t bionic_mktime(struct tm *tm)
{
    return (bionic_time_t)mktime(tm);
}

double bionic_difftime(bionic_time_t end, bionic_time_t start)
{
    return difftime((time_t)end, (time_t)start);
}

} /* extern "C" */

DynLibFunction symtable_time[] = {
    THUNK_SPECIFIC("clock_gettime", bionic_clock_gettime),
    THUNK_SPECIFIC("nanosleep",     bionic_nanosleep),
    THUNK_SPECIFIC("time",          bionic_time),
    THUNK_SPECIFIC("localtime",     bionic_localtime),
    THUNK_SPECIFIC("mktime",        bionic_mktime),
    /* returns a double: the only one here that needs the softfp bridge. */
    THUNK_SPECIFIC("difftime",      bionic_difftime),
    { NULL, 0 },
};
