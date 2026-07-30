#include "trace.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static int trace_enabled(void)
{
    /* Resolved once: this sits on the frame path later on. */
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("LOADER_TRACE");
        cached = (v && *v && *v != '0') ? 1 : 0;
    }
    return cached;
}

extern "C" void trace(const char *fmt, ...)
{
    if (!trace_enabled())
        return;

    va_list ap;
    va_start(ap, fmt);
    fputs("TRACE: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
}

extern "C" void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("FATAL: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
}
