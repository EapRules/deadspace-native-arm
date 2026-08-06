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
#include "time_scale.h"
#include "trace.h"

/*
 * Development-only clock acceleration. The contract, the reasoning and the list
 * of what is deliberately left unscaled are in time_scale.h; this is only where
 * it happens to live, because these are the functions it acts on.
 *
 * DEADSPACE_TEST_TIME_SCALE is this port's original, game-specific spelling of
 * the same idea, kept as an alias so the compatibility-matrix runs that use it
 * keep working. It is the one thing here that knows a game's name, and it goes
 * away once those runs move to DEADSPACE_TIME_SCALE.
 */

/*
 * The multiplier, parsed once. Capped at 64 because past that the offset grows
 * further between two frames than any engine's interpolation expects, and
 * physics stepped off the clock starts tunnelling through geometry - which
 * reads as a port bug and is not one.
 */
double port_time_scale(void)
{
    static const double scale = [] {
        const char *value = getenv("DEADSPACE_TIME_SCALE");
        const char *name  = "DEADSPACE_TIME_SCALE";
        if (!value || !*value) {
            value = getenv("DEADSPACE_TEST_TIME_SCALE");
            name  = "DEADSPACE_TEST_TIME_SCALE";
        }
        if (!value || !*value)
            return 1.0;

        char *end = NULL;
        const double parsed = strtod(value, &end);
        if (end == value || (end && *end != '\0') || !(parsed >= 1.0) ||
            parsed > 64.0) {
            warning("%s=%s is not a scale between 1 and 64; ignoring it.\n",
                    name, value);
            return 1.0;
        }
        if (parsed != 1.0)
            warning("time scale %.2fx - development emulator only. The guest's "
                    "monotonic clock and gettimeofday run fast; CLOCK_REALTIME, "
                    "time() and localtime() do not.\n", parsed);
        return parsed;
    }();
    return scale;
}

int64_t port_time_scale_offset_ns(void)
{
    const double scale = port_time_scale();
    if (scale == 1.0)
        return 0;   /* the whole feature, off, before any clock is read */

    /* Anchored on the first call, which is the earliest moment the offset is
     * observable by anyone. Function-local static init is thread-safe, and it
     * needs to be: the render thread and the audio thread do arrive together. */
    static const struct timespec origin = [] {
        struct timespec value = {0, 0};
        clock_gettime(CLOCK_MONOTONIC, &value);
        return value;
    }();

    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t elapsed = (int64_t)(now.tv_sec - origin.tv_sec) * 1000000000LL
                          + (int64_t)(now.tv_nsec - origin.tv_nsec);
    if (elapsed <= 0)
        return 0;
    return (int64_t)((double)elapsed * (scale - 1.0));
}

static void shift_timespec(struct timespec *value, int64_t delta_ns)
{
    if (!value || delta_ns == 0)
        return;

    int64_t total = (int64_t)value->tv_nsec + delta_ns;
    value->tv_sec += (time_t)(total / 1000000000LL);
    total %= 1000000000LL;
    if (total < 0) {
        total += 1000000000LL;
        value->tv_sec -= 1;
    }
    value->tv_nsec = (long)total;
}

void port_time_scale_forward(struct timespec *value)
{
    shift_timespec(value, port_time_scale_offset_ns());
}

/*
 * The offset read here is the one at the moment of the call, not the one in
 * force when the guest computed the deadline. It is very slightly smaller, so
 * the wait can end a hair early - which every caller of a timed wait already
 * handles, because spurious wakeups exist.
 */
void port_time_scale_reverse(struct timespec *value)
{
    shift_timespec(value, -port_time_scale_offset_ns());
}

/*
 * Which clock ids measure elapsed time. CLOCK_REALTIME is a date, and the two
 * CPU-time clocks are not wall time at all - scaling either would be a lie
 * about something nobody asked to go faster.
 */
static bool clock_measures_elapsed_time(int clk_id)
{
    switch (clk_id) {
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
        return true;
    default:
        return false;
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
        if (clock_measures_elapsed_time(clk_id))
            port_time_scale_forward(&host);
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
        /* Scaled, unlike the other realtime paths here: see time_scale.h. */
        struct timespec paced;
        paced.tv_sec  = host.tv_sec;
        paced.tv_nsec = (long)host.tv_usec * 1000;
        port_time_scale_forward(&paced);
        tv->tv_sec  = (int32_t)paced.tv_sec;
        tv->tv_usec = (int32_t)(paced.tv_nsec / 1000);
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

    /* A sleep is a duration, not an instant, so it divides rather than shifts:
     * the guest asked to be woken after N of *its* nanoseconds. */
    const double scale = port_time_scale();
    if (scale > 1.0) {
        const int64_t total = (int64_t)((double)((int64_t)host_req.tv_sec * 1000000000LL
                                                 + host_req.tv_nsec) / scale);
        host_req.tv_sec  = (time_t)(total / 1000000000LL);
        host_req.tv_nsec = (long)(total % 1000000000LL);
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
    /* Unscaled, unlike the old DEADSPACE_TEST_TIME_SCALE path that added the
     * offset here: this is the clock a save file's timestamp comes from. */
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
    THUNK_SPECIFIC("gettimeofday",  bionic_gettimeofday),
    THUNK_SPECIFIC("nanosleep",     bionic_nanosleep),
    THUNK_SPECIFIC("time",          bionic_time),
    THUNK_SPECIFIC("localtime",     bionic_localtime),
    THUNK_SPECIFIC("mktime",        bionic_mktime),
    /* returns a double: the only one here that needs the softfp bridge. */
    THUNK_SPECIFIC("difftime",      bionic_difftime),
    { NULL, 0 },
};
