/*
 * Milestone tracing.
 *
 * The harness cannot ask the port "did it work?"; it can only read what the
 * port prints. Every observable milestone (module loaded, onCreate returned,
 * gl ready, frame, non-black framebuffer) is announced through trace() so the
 * verification script asserts on facts instead of on the absence of a crash.
 *
 * Enabled by LOADER_TRACE=1 so a released port stays quiet.
 */
#ifndef DEADSPACE_TRACE_H
#define DEADSPACE_TRACE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Prints "TRACE: <msg>" to stderr, unbuffered, when LOADER_TRACE is set. */
void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Always printed, tracing on or off: the user needs to see why it refused. */
void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif /* DEADSPACE_TRACE_H */
