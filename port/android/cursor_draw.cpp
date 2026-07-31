/*
 * Software cursor for Dead Space's touch-only menus.
 *
 * The game imports GLES 1.1 and the real R36S driver does not expose a desktop
 * hardware cursor under KMS/DRM. A compact arrow is therefore painted into the
 * default framebuffer just before swap. glClear plus scissor is used instead
 * of shaders or vertex arrays: it works on pure GLES 1.1 and touches very
 * little state. Every changed value is restored before the next game frame.
 */
#include <algorithm>
#include <string.h>

#include <SDL2/SDL.h>

#include "khronos/glad.h"
#include "trace.h"

#include "cursor_draw.h"
#include "input_bridge.h"

static uintptr_t find_gles1_function(const char *name)
{
    /*
     * This is host armhf code, so it must call the raw driver pointer. The
     * entries in symtable_gles1 are softfp thunks built for the armeabi game;
     * calling glClearColor through one from hardfp host code loses three float
     * arguments and produced the red cross seen in the first local capture.
     */
    return (uintptr_t)SDL_GL_GetProcAddress(name);
}

struct CursorGl {
    using BindFramebuffer = void (*)(GLenum, GLuint);
    using Clear = void (*)(GLbitfield);
    using ClearColor = void (*)(GLfloat, GLfloat, GLfloat, GLfloat);
    using ColorMask = void (*)(GLboolean, GLboolean, GLboolean, GLboolean);
    using Disable = void (*)(GLenum);
    using Enable = void (*)(GLenum);
    using GetBooleanv = void (*)(GLenum, GLboolean *);
    using GetFloatv = void (*)(GLenum, GLfloat *);
    using GetIntegerv = void (*)(GLenum, GLint *);
    using IsEnabled = GLboolean (*)(GLenum);
    using Scissor = void (*)(GLint, GLint, GLsizei, GLsizei);

    BindFramebuffer bind_framebuffer;
    Clear clear;
    ClearColor clear_color;
    ColorMask color_mask;
    Disable disable;
    Enable enable;
    GetBooleanv get_boolean;
    GetFloatv get_float;
    GetIntegerv get_integer;
    IsEnabled is_enabled;
    Scissor scissor;
    bool ready;
};

static CursorGl load_cursor_gl(void)
{
    CursorGl gl = {
        (CursorGl::BindFramebuffer)find_gles1_function("glBindFramebufferOES"),
        (CursorGl::Clear)find_gles1_function("glClear"),
        (CursorGl::ClearColor)find_gles1_function("glClearColor"),
        (CursorGl::ColorMask)find_gles1_function("glColorMask"),
        (CursorGl::Disable)find_gles1_function("glDisable"),
        (CursorGl::Enable)find_gles1_function("glEnable"),
        (CursorGl::GetBooleanv)find_gles1_function("glGetBooleanv"),
        (CursorGl::GetFloatv)find_gles1_function("glGetFloatv"),
        (CursorGl::GetIntegerv)find_gles1_function("glGetIntegerv"),
        (CursorGl::IsEnabled)find_gles1_function("glIsEnabled"),
        (CursorGl::Scissor)find_gles1_function("glScissor"),
        false,
    };
    gl.ready = gl.bind_framebuffer && gl.clear && gl.clear_color &&
               gl.color_mask && gl.disable && gl.enable && gl.get_boolean &&
               gl.get_float && gl.get_integer && gl.is_enabled && gl.scissor;
    trace("menu cursor GL: %s", gl.ready ? "GLES1 scissor backend ready"
                                         : "missing GLES1 function");
    return gl;
}

static void clear_rect(CursorGl &gl, int x, int y, int width, int height,
                       int fb_width, int fb_height)
{
    int x0 = std::max(0, x);
    int y0 = std::max(0, y);
    int x1 = std::min(fb_width, x + width);
    int y1 = std::min(fb_height, y + height);
    if (x1 <= x0 || y1 <= y0)
        return;
    gl.scissor(x0, y0, x1 - x0, y1 - y0);
    gl.clear(GL_COLOR_BUFFER_BIT);
}

static void draw_pattern_runs(CursorGl &gl, const char *const *pattern,
                              int rows, char pixel, int x, int y,
                              int fb_width, int fb_height)
{
    for (int row = 0; row < rows; row++) {
        const char *line = pattern[row];
        for (int begin = 0; line[begin];) {
            while (line[begin] && line[begin] != pixel)
                begin++;
            if (!line[begin])
                break;
            int end = begin + 1;
            while (line[end] == pixel)
                end++;
            clear_rect(gl, x + begin, y - row, end - begin, 1,
                       fb_width, fb_height);
            begin = end;
        }
    }
}

extern "C" void android_cursor_draw(int fb_width, int fb_height)
{
    float cx = 0.0f, cy = 0.0f;
    int visible = 0;
    android_input_cursor_position(&cx, &cy, &visible);
    if (!visible || fb_width <= 0 || fb_height <= 0)
        return;

    static CursorGl gl = load_cursor_gl();
    if (!gl.ready)
        return;

    GLint previous_fb = 0;
    GLint previous_scissor[4] = {0, 0, fb_width, fb_height};
    GLfloat previous_clear[4] = {0, 0, 0, 0};
    GLboolean previous_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean previous_scissor_enabled = gl.is_enabled(GL_SCISSOR_TEST);

    gl.get_integer(GL_FRAMEBUFFER_BINDING, &previous_fb);
    gl.get_integer(GL_SCISSOR_BOX, previous_scissor);
    gl.get_float(GL_COLOR_CLEAR_VALUE, previous_clear);
    gl.get_boolean(GL_COLOR_WRITEMASK, previous_mask);

    gl.bind_framebuffer(GL_FRAMEBUFFER, 0);
    gl.enable(GL_SCISSOR_TEST);
    gl.color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    int x = (int)cx;
    int y = fb_height - 1 - (int)cy;

    /*
     * Pixel-art pointer with the hot spot at its upper-left tip. It remains
     * legible over both the bright menus and the dark game without relying on
     * blending, textures or any GLES feature beyond the existing scissor path.
     */
    static const char *const pointer[] = {
        "#..............",
        "##.............",
        "#o#............",
        "#oo#...........",
        "#ooo#..........",
        "#oooo#.........",
        "#ooooo#........",
        "#oooooo#.......",
        "#ooooooo#......",
        "#oooooooo#.....",
        "#ooooo#####....",
        "#oo#oo#........",
        "#o#.#oo#.......",
        "##..#oo#.......",
        "#....#oo#......",
        ".....#oo#......",
        "......##.......",
    };
    const int pointer_rows = sizeof(pointer) / sizeof(pointer[0]);

    gl.clear_color(0.0f, 0.0f, 0.0f, 1.0f);
    draw_pattern_runs(gl, pointer, pointer_rows, '#', x, y,
                      fb_width, fb_height);
    gl.clear_color(0.82f, 0.96f, 1.0f, 1.0f);
    draw_pattern_runs(gl, pointer, pointer_rows, 'o', x, y,
                      fb_width, fb_height);

    gl.clear_color(previous_clear[0], previous_clear[1],
                   previous_clear[2], previous_clear[3]);
    gl.color_mask(previous_mask[0], previous_mask[1],
                  previous_mask[2], previous_mask[3]);
    gl.scissor(previous_scissor[0], previous_scissor[1],
               previous_scissor[2], previous_scissor[3]);
    if (!previous_scissor_enabled)
        gl.disable(GL_SCISSOR_TEST);
    gl.bind_framebuffer(GL_FRAMEBUFFER, (GLuint)previous_fb);
}
