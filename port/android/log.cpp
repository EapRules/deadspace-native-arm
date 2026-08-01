/*
 * liblog: the game's only logging import.
 *
 * Android routes this to logcat; here it goes to stderr, which is what the
 * harness captures. The priority letter is kept because the game logs its own
 * asset and GL failures through it, and those messages are the cheapest
 * diagnostic we have when something goes wrong inside the .so.
 */
#include <stdarg.h>
#include <stdio.h>

extern "C" {

enum {
    ANDROID_LOG_UNKNOWN = 0,
    ANDROID_LOG_DEFAULT,
    ANDROID_LOG_VERBOSE,
    ANDROID_LOG_DEBUG,
    ANDROID_LOG_INFO,
    ANDROID_LOG_WARN,
    ANDROID_LOG_ERROR,
    ANDROID_LOG_FATAL,
    ANDROID_LOG_SILENT,
};

static char prio_letter(int prio)
{
    switch (prio) {
    case ANDROID_LOG_VERBOSE: return 'V';
    case ANDROID_LOG_DEBUG:   return 'D';
    case ANDROID_LOG_INFO:    return 'I';
    case ANDROID_LOG_WARN:    return 'W';
    case ANDROID_LOG_ERROR:   return 'E';
    case ANDROID_LOG_FATAL:   return 'F';
    default:                  return '?';
    }
}

/* Variadic, so it is exported unthunked: on AAPCS the variable part of the
 * argument list is passed in core registers/stack under both the soft-float
 * and hard-float variants, so no ABI bridge is needed or possible here. */
int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = fprintf(stderr, "[%c/%s] ", prio_letter(prio), tag ? tag : "?");
    n += vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
    fflush(stderr);
    return n + 1;
}

int __android_log_write(int prio, const char *tag, const char *text)
{
    return __android_log_print(prio, tag, "%s", text ? text : "");
}

/*
 * Bionic's scheduling hints. They are genuinely empty in a release bionic too:
 * they exist for the ART profiler. Implementing them as no-ops is the real
 * behaviour, not a stub.
 */
void __google_potentially_blocking_region_begin(void) {}
void __google_potentially_blocking_region_end(void) {}

} /* extern "C" */

/*
 * The table the loader binds into libdeadspace.so for liblog.so, plus the two
 * bionic profiling hooks that live in libc but have no thunk of their own.
 */
#include "so_util.h"
#include "thunk_gen.h"

DynLibFunction symtable_log[] = {
    /* Variadic: exported as-is, no ABI bridge. */
    NO_THUNK("__android_log_print", (uintptr_t)&__android_log_print),
    NO_THUNK("__android_log_write", (uintptr_t)&__android_log_write),
    THUNK_DIRECT(__google_potentially_blocking_region_begin),
    THUNK_DIRECT(__google_potentially_blocking_region_end),
    { NULL, 0 },
};
