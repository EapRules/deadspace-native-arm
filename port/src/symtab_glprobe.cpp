/*
 * Shader compilation probe.
 *
 * M4's criterion is "the EGL context is current and the game's shaders
 * compile". The loader cannot answer that by inspection: the engine compiles
 * its shaders from its own thread, checks the status itself, and says nothing
 * on stderr when one fails - it just draws nothing afterwards, which looks
 * exactly like a working port with a black frame.
 *
 * So glCompileShader and glLinkProgram are intercepted. Each call is forwarded
 * to the real driver entry point and the status is read back; a failure is
 * printed with the driver's info log, and the counts are what the loader waits
 * on before it is willing to claim GL is up.
 *
 * This is observation, not emulation: the driver does the work and its verdict
 * is reported verbatim. A shader that does not compile makes the milestone
 * fail, which is the point.
 *
 * The table below is listed before both GL tables in so_dynamic_libraries, so
 * the game binds these instead of the raw entry points. Shader calls forward
 * through the GLES2 glad globals. Draw calls must select the live GLES1 table:
 * this fixed-function game never initialises the separate GLES2 glad globals.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>

#include "khronos/glad.h"

#include "so_util.h"
#include "thunk_gen.h"

/* Written by the game's thread, read by the loader's. */
static std::atomic<int> g_shaders_ok(0);
static std::atomic<int> g_shaders_failed(0);
static std::atomic<int> g_programs_ok(0);
static std::atomic<int> g_programs_failed(0);
static std::atomic<long> g_draws(0);
static std::atomic<long> g_textures(0);

extern DynLibFunction symtable_gles1[];

static uintptr_t find_gles1_function(const char *name)
{
    for (int i = 0; symtable_gles1[i].symbol; i++) {
        if (strcmp(symtable_gles1[i].symbol, name) == 0)
            return symtable_gles1[i].func;
    }
    return 0;
}

static void dump_log(const char *what, unsigned int obj, bool is_shader)
{
    GLint len = 0;
    if (is_shader)
        glad_glGetShaderiv(obj, GL_INFO_LOG_LENGTH, &len);
    else
        glad_glGetProgramiv(obj, GL_INFO_LOG_LENGTH, &len);

    if (len <= 1) {
        fprintf(stderr, "GL: %s failed (driver gave no log)\n", what);
        fflush(stderr);
        return;
    }

    char *log = (char *)malloc((size_t)len + 1);
    if (!log)
        return;
    log[0] = '\0';
    if (is_shader)
        glad_glGetShaderInfoLog(obj, len, NULL, log);
    else
        glad_glGetProgramInfoLog(obj, len, NULL, log);
    log[len] = '\0';

    fprintf(stderr, "GL: %s failed:\n%s\n", what, log);
    fflush(stderr);
    free(log);
}

extern "C" void probe_glCompileShader(GLuint shader)
{
    glad_glCompileShader(shader);

    GLint ok = 0;
    glad_glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok) {
        g_shaders_ok++;
        return;
    }

    g_shaders_failed++;
    dump_log("shader compile", shader, true);
}

extern "C" void probe_glLinkProgram(GLuint program)
{
    glad_glLinkProgram(program);

    GLint ok = 0;
    glad_glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok) {
        g_programs_ok++;
        return;
    }

    g_programs_failed++;
    dump_log("program link", program, false);
}

/*
 * Draw calls.
 *
 * The game imports exactly these two draw entry points. Counting here rather
 * than in the frame loop is the difference the milestone cares about: a game
 * that clears the screen to a colour and presents it swaps buffers just as
 * happily as one that renders, so frames are not evidence of content. A draw
 * call is.
 *
 * Neither wrapper inspects or rewrites its arguments; they are forwarded
 * verbatim to the driver, so a miscount is the only thing that can go wrong
 * here, never a misdraw.
 */
extern "C" void probe_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    g_draws++;
    using DrawArrays = void (*)(GLenum, GLint, GLsizei);
    static DrawArrays draw =
        (DrawArrays)find_gles1_function("glDrawArrays");
    if (draw)
        draw(mode, first, count);
}

extern "C" void probe_glDrawElements(GLenum mode, GLsizei count, GLenum type,
                                     const void *indices)
{
    g_draws++;
    using DrawElements = void (*)(GLenum, GLsizei, GLenum, const void *);
    static DrawElements draw =
        (DrawElements)find_gles1_function("glDrawElements");
    if (draw)
        draw(mode, count, type, indices);
}

/*
 * Texture uploads are the fixed-function equivalent of a material becoming
 * live GPU content. The dimensions are deliberately not filtered: mip levels
 * and small UI textures are still real uploads performed by the engine.
 */
extern "C" void probe_glTexImage2D(GLenum target, GLint level,
                                   GLint internalformat, GLsizei width,
                                   GLsizei height, GLint border, GLenum format,
                                   GLenum type, const void *pixels)
{
    g_textures++;
    using TexImage2D = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
                                GLenum, GLenum, const void *);
    static TexImage2D upload =
        (TexImage2D)find_gles1_function("glTexImage2D");
    if (upload)
        upload(target, level, internalformat, width, height, border,
               format, type, pixels);
}

extern "C" long android_gl_draw_calls(void) { return g_draws.load(); }
extern "C" long android_gl_textures_uploaded(void) { return g_textures.load(); }

extern "C" int android_gl_shaders_compiled(void) { return g_shaders_ok.load(); }
extern "C" int android_gl_shaders_failed(void)   { return g_shaders_failed.load(); }
extern "C" int android_gl_programs_linked(void)  { return g_programs_ok.load(); }
extern "C" int android_gl_programs_failed(void)  { return g_programs_failed.load(); }

/*
 * Neither function takes or returns a float, so select_either() hands the game
 * the pointer directly with no ABI bridge - same as the GLES1 entries these
 * shadow.
 */
DynLibFunction symtable_glprobe[] = {
    THUNK_SPECIFIC("glCompileShader", probe_glCompileShader),
    THUNK_SPECIFIC("glLinkProgram",   probe_glLinkProgram),
    THUNK_SPECIFIC("glDrawArrays",    probe_glDrawArrays),
    THUNK_SPECIFIC("glDrawElements",  probe_glDrawElements),
    THUNK_SPECIFIC("glTexImage2D",    probe_glTexImage2D),
    { NULL, 0 },
};
