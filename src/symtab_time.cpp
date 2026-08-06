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
 * gettimeofday is the same bug with a nastier landing. The caller is a
 * microsecond-clock helper at module+0x237740, and it does not have a canary:
 *
 *     push {r4, lr}          ; [sp+8] = r4, [sp+12] = return address
 *     sub  sp, sp, #8        ; [sp+0..7] = struct timeval, all of it
 *     mov  r0, sp
 *     bl   gettimeofday      ; host writes 16 bytes, not 8
 *     ldm  sp, {r0, r2}      ; tv_sec, tv_usec
 *     ...
 *     pop  {r4, pc}          ; <- pc
 *
 * The host's 64-bit tv_usec is written at [sp+8..15]: its low word lands on
 * the saved r4 and its high word - zero, because microseconds never fill 32
 * bits - lands on the saved return address. So the function computes the right
 * timestamp, stores it correctly, and then returns to address zero.
 *
 * That is what the run log showed before this entry existed:
 *
 *     FATAL: SIGSEGV at 0x00000000
 *            pc = 0x00000000  lr = <libc>  r0 = 0x6a6bab71  r1 = 0x000f4240
 *
 * r0 is a live Unix timestamp and r1 is 1000000, which is the sec-to-usec
 * multiply about to happen two instructions later - the smoking gun, and the
 * reason this is not the missing-JNI-class crash it was mistaken for. The
 * fake-class warnings printed just above it are real but unrelated: the engine
 * survives every one of them, because a NULL jmethodID goes through the JNI
 * shim's null checks and comes back as a zero return value, not a jump.
 *
 * Every function here therefore takes the game's layout, calls the host with
 * the host's, and narrows across the boundary. These entries come before the
 * generated libc table in so_dynamic_libraries so they win the lookup.
 *
 * Narrowing is lossless until 2038; past that the game sees a wrapped clock,
 * exactly as it would on a real armeabi-v7a device.
 */
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "so_util.h"
#include "thunk_gen.h"
#include "trace.h"

/* Test-only wall-clock acceleration.  NativeOnDrawFrame still runs every
 * frame; only elapsed time observed by the game is scaled so unskippable EA
 * splashes and cinematics do not make the compatibility matrix take an hour.
 * Production never sets the variable and receives the host clock verbatim. */
static double test_time_scale(void)
{
    static double scale = [] {
        const char *value = getenv("DEADSPACE_TEST_TIME_SCALE");
        if (!value || !*value)
            return 1.0;
        char *end = NULL;
        double parsed = strtod(value, &end);
        if (end == value || parsed < 1.0 || parsed > 16.0)
            parsed = 1.0;
        if (parsed != 1.0)
            trace("test clock scale=%.2fx", parsed);
        return parsed;
    }();
    return scale;
}

static int64_t test_extra_nanoseconds(void)
{
    double scale = test_time_scale();
    if (scale == 1.0)
        return 0;
    static struct timespec origin = [] {
        struct timespec value = {};
        clock_gettime(CLOCK_MONOTONIC, &value);
        return value;
    }();
    struct timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t elapsed = (int64_t)(now.tv_sec - origin.tv_sec) * 1000000000LL
                    + (int64_t)now.tv_nsec - origin.tv_nsec;
    return (int64_t)((double)elapsed * (scale - 1.0));
}

static void add_nanoseconds(struct timespec *value, int64_t extra)
{
    if (!value || extra == 0)
        return;
    int64_t nanoseconds = (int64_t)value->tv_nsec + extra;
    value->tv_sec += nanoseconds / 1000000000LL;
    value->tv_nsec = nanoseconds % 1000000000LL;
    if (value->tv_nsec < 0) {
        value->tv_nsec += 1000000000LL;
        value->tv_sec--;
    }
}

extern "C" {

typedef int32_t bionic_time_t;

struct bionic_timespec {
    int32_t tv_sec;
    int32_t tv_nsec;
};

/* bionic's armeabi struct timeval: two longs, 8 bytes total. The host's is
 * 16 - __time64_t plus __suseconds64_t - and that difference is not visible
 * at any call site, only in how many bytes the callee writes. */
struct bionic_timeval {
    int32_t tv_sec;
    int32_t tv_usec;
};

/* struct tm is identical on both sides (nine ints, then long + char*), so it
 * crosses unchanged; only the time_t on either end of it needs narrowing. */

int bionic_clock_gettime(int clk_id, struct bionic_timespec *ts)
{
    struct timespec host;
    int rc = clock_gettime(clk_id, &host);
    if (rc == 0 && ts) {
        add_nanoseconds(&host, test_extra_nanoseconds());
        ts->tv_sec  = (int32_t)host.tv_sec;
        ts->tv_nsec = (int32_t)host.tv_nsec;
    }
    return rc;
}

/*
 * struct timezone is two ints on both sides, so it is forwarded untouched.
 * The game only ever passes NULL for it (the caller zeroes r1 before the
 * branch that leads here), but forwarding is free and a stub that ignored the
 * argument would be a landmine for whichever call site does use it.
 */
int bionic_gettimeofday(struct bionic_timeval *tv, struct timezone *tz)
{
    struct timeval host;
    int rc = gettimeofday(tv ? &host : NULL, tz);
    if (rc == 0 && tv) {
        int64_t adjusted = (int64_t)host.tv_usec * 1000
                         + test_extra_nanoseconds();
        host.tv_sec += adjusted / 1000000000LL;
        host.tv_usec = (adjusted % 1000000000LL) / 1000;
        tv->tv_sec  = (int32_t)host.tv_sec;
        tv->tv_usec = (int32_t)host.tv_usec;
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
    double scale = test_time_scale();
    if (scale > 1.0) {
        int64_t total = (int64_t)host_req.tv_sec * 1000000000LL
                      + host_req.tv_nsec;
        total = (int64_t)((double)total / scale);
        host_req.tv_sec = total / 1000000000LL;
        host_req.tv_nsec = total % 1000000000LL;
    }

    int rc = nanosleep(&host_req, &host_rem);
    if (rem) {
        rem->tv_sec  = (int32_t)host_rem.tv_sec;
        rem->tv_nsec = (int32_t)host_rem.tv_nsec;
    }
    return rc;
}

bionic_time_t bionic_time(bionic_time_t *out)
{
    bionic_time_t now = (bionic_time_t)(time(NULL)
        + test_extra_nanoseconds() / 1000000000LL);
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
    THUNK_SPECIFIC("gettimeofday",  bionic_gettimeofday),
    THUNK_SPECIFIC("nanosleep",     bionic_nanosleep),
    THUNK_SPECIFIC("time",          bionic_time),
    THUNK_SPECIFIC("localtime",     bionic_localtime),
    THUNK_SPECIFIC("mktime",        bionic_mktime),
    /* returns a double: the only one here that needs the softfp bridge. */
    THUNK_SPECIFIC("difftime",      bionic_difftime),
    { NULL, 0 },
};
