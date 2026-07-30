/*
 * Software cursor, drawn over the game just before the buffers are swapped.
 *
 * The menus in this build are touch targets with no gamepad navigation, so the
 * port synthesises a pointer (android/platform.cpp). A pointer you cannot see
 * is unusable, and KMS/DRM has no hardware cursor - PortMaster's own porting
 * notes call for a software cursor for exactly this reason.
 *
 * Everything here is done on the assumption that the game owns the GL state.
 * Every piece of state this touches is saved and put back, because the game
 * draws the next frame expecting to find its own state intact.
 */
#include <stdint.h>
#include <stddef.h>

#include "khronos/glad.h"
#include "trace.h"

extern "C" void android_cursor_position(float *x, float *y, int *visible);

static GLuint g_prog = 0;
static GLuint g_vbo  = 0;
static GLint  g_loc_pos = -1;
static GLint  g_loc_rect = -1;
static GLint  g_loc_colour = -1;
static bool   g_failed = false;

/* A filled triangle with a dark outline: readable on both bright and dark art. */
static const char *kVert =
    "attribute vec2 a_pos;\n"
    "uniform vec4 u_rect;\n"          /* xy = origin in clip space, zw = size */
    "void main() {\n"
    "  gl_Position = vec4(u_rect.xy + a_pos * u_rect.zw, 0.0, 1.0);\n"
    "}\n";

static const char *kFrag =
    "precision mediump float;\n"
    "uniform vec4 u_colour;\n"
    "void main() { gl_FragColor = u_colour; }\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glad_glCreateShader(type);
    glad_glShaderSource(sh, 1, &src, NULL);
    glad_glCompileShader(sh);
    GLint ok = 0;
    glad_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256] = {0};
        glad_glGetShaderInfoLog(sh, sizeof(log) - 1, NULL, log);
        trace("cursor: shader failed: %s", log);
        glad_glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static bool ensure_program(void)
{
    if (g_prog)
        return true;
    if (g_failed)
        return false;

    GLuint vs = compile(GL_VERTEX_SHADER, kVert);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !fs) {
        g_failed = true;
        return false;
    }

    g_prog = glad_glCreateProgram();
    glad_glAttachShader(g_prog, vs);
    glad_glAttachShader(g_prog, fs);
    glad_glLinkProgram(g_prog);
    glad_glDeleteShader(vs);
    glad_glDeleteShader(fs);

    GLint ok = 0;
    glad_glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        trace("cursor: program link failed");
        g_prog = 0;
        g_failed = true;
        return false;
    }

    g_loc_pos    = glad_glGetAttribLocation(g_prog, "a_pos");
    g_loc_rect   = glad_glGetUniformLocation(g_prog, "u_rect");
    g_loc_colour = glad_glGetUniformLocation(g_prog, "u_colour");

    /* An arrow, as a triangle fan in a unit square with the tip at 0,0. */
    static const float verts[] = {
        0.00f,  0.00f,
        0.00f, -1.00f,
        0.28f, -0.72f,
        0.62f, -0.62f,
    };
    glad_glGenBuffers(1, &g_vbo);
    glad_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glad_glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    return true;
}

extern "C" void android_cursor_draw(int fb_width, int fb_height)
{
    float cx, cy;
    int visible = 0;
    android_cursor_position(&cx, &cy, &visible);
    if (!visible || fb_width <= 0 || fb_height <= 0)
        return;
    if (!ensure_program())
        return;

    /* --- save every piece of state this function changes --- */
    GLint  prev_prog = 0, prev_buf = 0, prev_fb = 0;
    GLint  prev_viewport[4];
    GLboolean prev_blend   = glad_glIsEnabled(GL_BLEND);
    GLboolean prev_depth   = glad_glIsEnabled(GL_DEPTH_TEST);
    GLboolean prev_cull    = glad_glIsEnabled(GL_CULL_FACE);
    GLboolean prev_scissor = glad_glIsEnabled(GL_SCISSOR_TEST);
    GLint  prev_src_rgb = 0, prev_dst_rgb = 0, prev_src_a = 0, prev_dst_a = 0;
    GLint  prev_enabled = 0, prev_size = 0, prev_type = 0, prev_norm = 0, prev_stride = 0;
    void  *prev_ptr = NULL;
    GLint  prev_attr_buf = 0;

    glad_glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glad_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_buf);
    glad_glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fb);
    glad_glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glad_glGetIntegerv(GL_BLEND_SRC_RGB, &prev_src_rgb);
    glad_glGetIntegerv(GL_BLEND_DST_RGB, &prev_dst_rgb);
    glad_glGetIntegerv(GL_BLEND_SRC_ALPHA, &prev_src_a);
    glad_glGetIntegerv(GL_BLEND_DST_ALPHA, &prev_dst_a);

    if (g_loc_pos >= 0) {
        GLuint idx = (GLuint)g_loc_pos;
        glad_glGetVertexAttribiv(idx, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &prev_enabled);
        glad_glGetVertexAttribiv(idx, GL_VERTEX_ATTRIB_ARRAY_SIZE, &prev_size);
        glad_glGetVertexAttribiv(idx, GL_VERTEX_ATTRIB_ARRAY_TYPE, &prev_type);
        glad_glGetVertexAttribiv(idx, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &prev_norm);
        glad_glGetVertexAttribiv(idx, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &prev_stride);
        glad_glGetVertexAttribiv(idx, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &prev_attr_buf);
        glad_glGetVertexAttribPointerv(idx, GL_VERTEX_ATTRIB_ARRAY_POINTER, &prev_ptr);
    }

    /* --- draw --- */
    glad_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glad_glViewport(0, 0, fb_width, fb_height);
    glad_glDisable(GL_DEPTH_TEST);
    glad_glDisable(GL_CULL_FACE);
    glad_glDisable(GL_SCISSOR_TEST);
    glad_glEnable(GL_BLEND);
    glad_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glad_glUseProgram(g_prog);
    glad_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    if (g_loc_pos >= 0) {
        glad_glEnableVertexAttribArray((GLuint)g_loc_pos);
        glad_glVertexAttribPointer((GLuint)g_loc_pos, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
    }

    /* Pixel position -> clip space. Y is flipped: touch coordinates grow down. */
    float ndc_x = (cx / (float)fb_width)  * 2.0f - 1.0f;
    float ndc_y = 1.0f - (cy / (float)fb_height) * 2.0f;
    float size  = 22.0f;                       /* px, on a 640x480 panel */
    float sx    = (size / (float)fb_width)  * 2.0f;
    float sy    = (size / (float)fb_height) * 2.0f;

    /* Outline first, offset by a pixel, so it reads against light artwork. */
    float ox = (1.5f / (float)fb_width) * 2.0f;
    float oy = (1.5f / (float)fb_height) * 2.0f;
    glad_glUniform4f(g_loc_rect, ndc_x - ox, ndc_y + oy, sx * 1.18f, sy * 1.18f);
    glad_glUniform4f(g_loc_colour, 0.0f, 0.0f, 0.0f, 0.75f);
    glad_glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    glad_glUniform4f(g_loc_rect, ndc_x, ndc_y, sx, sy);
    glad_glUniform4f(g_loc_colour, 1.0f, 1.0f, 1.0f, 0.95f);
    glad_glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    /* --- put everything back --- */
    if (g_loc_pos >= 0) {
        GLuint idx = (GLuint)g_loc_pos;
        glad_glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_attr_buf);
        glad_glVertexAttribPointer(idx, prev_size, (GLenum)prev_type,
                                   (GLboolean)prev_norm, prev_stride, prev_ptr);
        if (!prev_enabled)
            glad_glDisableVertexAttribArray(idx);
    }
    glad_glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_buf);
    glad_glUseProgram((GLuint)prev_prog);
    glad_glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fb);
    glad_glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glad_glBlendFuncSeparate((GLenum)prev_src_rgb, (GLenum)prev_dst_rgb,
                             (GLenum)prev_src_a,   (GLenum)prev_dst_a);
    if (!prev_blend)   glad_glDisable(GL_BLEND);
    if (prev_depth)    glad_glEnable(GL_DEPTH_TEST);
    if (prev_cull)     glad_glEnable(GL_CULL_FACE);
    if (prev_scissor)  glad_glEnable(GL_SCISSOR_TEST);
}
