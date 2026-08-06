#include <atomic>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "khronos/glad.h"
#include "trace.h"

#include "gl_diag.h"

using GetError = GLenum (*)(void);

static std::atomic<int> g_reports(0);
static std::atomic<long> g_errors(0);
static GetError g_get_error = nullptr;
static constexpr int kReportLimit = 128;

bool gl_diag_enabled(void)
{
    static int enabled = [] {
        const char *value = getenv("DEADSPACE_GL_DIAG");
        return value && *value && strcmp(value, "0") != 0;
    }();
    return enabled != 0;
}

static GetError raw_get_error(void)
{
    if (!g_get_error)
        g_get_error = (GetError)SDL_GL_GetProcAddress("glGetError");
    return g_get_error;
}

static void report(const char *position, const char *name, GLenum error)
{
    g_errors.fetch_add(1);
    int line = g_reports.fetch_add(1);
    if (line < kReportLimit)
        trace("GLDIAG: %s %s: 0x%04x", position,
              name ? name : "(unknown)", error);
    else if (line == kReportLimit)
        trace("GLDIAG: report limit reached; suppressing further errors");
}

long gl_diag_error_count(void)
{
    return g_errors.load();
}

void gl_diag_before(const char *name)
{
    GetError get_error = raw_get_error();
    if (!get_error)
        return;

    GLenum error;
    while ((error = get_error()) != GL_NO_ERROR)
        report("pending before", name, error);
}

void gl_diag_after(const char *name)
{
    GetError get_error = raw_get_error();
    if (!get_error)
        return;

    GLenum error;
    while ((error = get_error()) != GL_NO_ERROR)
        report("produced by", name, error);
}
