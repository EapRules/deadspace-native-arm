/*
 * Development-only clock acceleration, for the qemu/Mesa emulator.
 *
 * Under llvmpipe the loader draws two or three frames a second, but the engine
 * paces its splashes, fades and cinematics off the wall clock, so an eight
 * second logo costs eight real seconds and a boot-to-title iteration costs
 * forty minutes. Scaling what the guest reads off the monotonic clock buys
 * those minutes back without the loader knowing anything about the game.
 *
 * The whole mechanism is one number: an offset, zero at process start, that
 * grows at (scale - 1) nanoseconds per real nanosecond. Everything the guest
 * observes as elapsed time gets that offset added, so a delta the engine
 * measures across two calls comes out `scale` times larger than the real one.
 * The offset only ever grows, which is what keeps the clock monotonic - the
 * guest never sees time move backwards, only faster.
 *
 * Deliberately NOT scaled: CLOCK_REALTIME, time(), ftime(), localtime() and
 * gmtime(). Those are what a save file's timestamp and any certificate-style
 * date comparison are built from, and a save written eleven minutes in the
 * future is a bug report from a player who never set the variable.
 *
 * gettimeofday() IS scaled, which looks like a contradiction and is a
 * deliberate one: it is a microsecond clock and every engine here uses it for
 * frame pacing, not for stamping anything. Dead Space's frame timer reads it
 * directly. The cost is that a save stamped from gettimeofday during a scaled
 * run carries the accumulated offset, which is minutes of skew in a
 * development run and never happens in a release build.
 *
 * Absolute deadlines the guest hands *back* to the loader - the timespec of
 * pthread_cond_timedwait and sem_timedwait - must go through the inverse, or a
 * deadline computed from a scaled clock lands in the host's future by the whole
 * accumulated offset and the wait turns into a hang that grows the longer the
 * process has been up.
 *
 * Off unless <PREFIX>_TIME_SCALE is set, and a no-op in that case: the offset
 * function returns a constant zero before it reads any clock.
 */
#ifndef PORTBASE_TIME_SCALE_H
#define PORTBASE_TIME_SCALE_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The configured multiplier, or 1.0 when the feature is off. Parsed once. */
double port_time_scale(void);

/* Nanoseconds to add to a real elapsed time to get the guest's. 0 when off. */
int64_t port_time_scale_offset_ns(void);

/* Host clock -> what the guest should see. No-op when off. */
void port_time_scale_forward(struct timespec *value);

/* A guest absolute deadline -> the host clock it means. No-op when off. */
void port_time_scale_reverse(struct timespec *value);

#ifdef __cplusplus
}
#endif

#endif /* PORTBASE_TIME_SCALE_H */
