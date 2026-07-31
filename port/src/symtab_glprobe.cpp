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
#include <algorithm>
#include <new>
#include <vector>

#include <SDL2/SDL.h>

#include "khronos/glad.h"
#include "atc_decompress.h"
#include "third_party/powervr/PVRTDecompress.h"

#include "gl_diag.h"
#include "so_util.h"
#include "thunk_gen.h"
#include "trace.h"

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

static bool extension_present(const char *extensions, const char *wanted)
{
    if (!extensions || !wanted || !*wanted)
        return false;
    size_t length = strlen(wanted);
    const char *at = extensions;
    while ((at = strstr(at, wanted)) != NULL) {
        bool left = at == extensions || at[-1] == ' ';
        bool right = at[length] == '\0' || at[length] == ' ';
        if (left && right)
            return true;
        at += length;
    }
    return false;
}

static bool driver_supports_atc(void)
{
    static int supported = -1;
    if (supported >= 0)
        return supported != 0;

    using GetString = const GLubyte *(*)(GLenum);
    GetString get_string =
        (GetString)SDL_GL_GetProcAddress("glGetString");
    const char *extensions = get_string
        ? (const char *)get_string(GL_EXTENSIONS) : NULL;
    supported = extension_present(
        extensions, "GL_AMD_compressed_ATC_texture") ? 1 : 0;
    trace("ATC: native driver support %s; software fallback %s",
          supported ? "present" : "absent",
          supported ? "disabled" : "enabled");
    return supported != 0;
}

static bool atc_format(GLenum format, bool *explicit_alpha)
{
    switch (format) {
    case GL_ATC_RGB_AMD:
        *explicit_alpha = false;
        return true;
    case GL_ATC_RGBA_EXPLICIT_ALPHA_AMD:
        *explicit_alpha = true;
        return true;
    default:
        return false;
    }
}

static bool decode_atc(GLenum format, GLsizei width, GLsizei height,
                       GLsizei image_size, const void *data,
                       std::vector<unsigned char> *rgba)
{
    bool explicit_alpha = false;
    if (!atc_format(format, &explicit_alpha) || width <= 0 || height <= 0 ||
        image_size < 0 || !data)
        return false;

    const std::size_t blocks_x = (static_cast<std::size_t>(width) + 3) / 4;
    const std::size_t blocks_y = (static_cast<std::size_t>(height) + 3) / 4;
    const std::size_t bytes_per_block = explicit_alpha ? 16u : 8u;
    if (blocks_x > SIZE_MAX / blocks_y ||
        blocks_x * blocks_y > static_cast<std::size_t>(image_size) /
                                  bytes_per_block)
        return false;

    const std::size_t decoded_size = static_cast<std::size_t>(width) *
                                     static_cast<std::size_t>(height) * 4u;
    try {
        rgba->resize(decoded_size);
    } catch (const std::bad_alloc &) {
        trace("ATC: cannot allocate %llu-byte decode buffer",
              (unsigned long long)decoded_size);
        return false;
    }

    const bool ok = explicit_alpha
        ? atc::decode_rgba_explicit(data, static_cast<std::size_t>(image_size),
                                    width, height, rgba->data())
        : atc::decode_rgb(data, static_cast<std::size_t>(image_size),
                          width, height, rgba->data());
    if (!ok)
        rgba->clear();
    return ok;
}

static bool pvrtc_format(GLenum format, bool *two_bpp, bool *opaque)
{
    switch (format) {
    case GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG:
        *two_bpp = false; *opaque = true;  return true;
    case GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG:
        *two_bpp = true;  *opaque = true;  return true;
    case GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG:
        *two_bpp = false; *opaque = false; return true;
    case GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG:
        *two_bpp = true;  *opaque = false; return true;
    default:
        return false;
    }
}

static bool decode_pvrtc(GLenum format, GLsizei width, GLsizei height,
                         GLsizei image_size, const void *data,
                         std::vector<unsigned char> *rgba)
{
    bool two_bpp = false, opaque = false;
    if (!pvrtc_format(format, &two_bpp, &opaque) ||
        width <= 0 || height <= 0 || image_size < 0 || !data)
        return false;

    uint64_t stored_width = std::max<uint64_t>(
        (uint64_t)width, two_bpp ? 16u : 8u);
    uint64_t stored_height = std::max<uint64_t>((uint64_t)height, 8u);
    uint64_t expected = stored_width * stored_height *
                        (two_bpp ? 2u : 4u) / 8u;
    uint64_t decoded_size = (uint64_t)width * (uint64_t)height * 4u;
    if (expected > (uint64_t)image_size ||
        decoded_size > (uint64_t)SIZE_MAX)
        return false;

    try {
        rgba->resize((size_t)decoded_size);
        pvr::PVRTDecompressPVRTC(data, two_bpp ? 1u : 0u,
                                (uint32_t)width, (uint32_t)height,
                                rgba->data());
    } catch (const std::bad_alloc &) {
        trace("PVRTC: cannot allocate %llu-byte decode buffer",
              (unsigned long long)decoded_size);
        return false;
    }

    if (opaque) {
        for (size_t i = 3; i < rgba->size(); i += 4)
            (*rgba)[i] = 255;
    }
    return true;
}

extern "C" void probe_glCompressedTexImage2D(
    GLenum target, GLint level, GLenum internalformat, GLsizei width,
    GLsizei height, GLint border, GLsizei image_size, const void *data)
{
    using CompressedUpload =
        void (*)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei,
                 const void *);
    using Upload =
        void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum,
                 GLenum, const void *);
    static CompressedUpload compressed_upload =
        (CompressedUpload)SDL_GL_GetProcAddress("glCompressedTexImage2D");
    static Upload upload =
        (Upload)SDL_GL_GetProcAddress("glTexImage2D");
    static int diagnostic_lines = 0;
    static int fallback_lines = 0;
    g_textures++;

    if (gl_diag_enabled()) {
        gl_diag_before("glCompressedTexImage2D");
        if (diagnostic_lines++ < 64) {
            trace("GLDIAG: compressed upload target=0x%04x level=%d "
                  "format=0x%04x size=%dx%d border=%d bytes=%d data=%p",
                  target, level, internalformat, width, height, border,
                  image_size, data);
        }
    }

    bool known_atc = false, ignored_explicit_alpha = false;
    known_atc = atc_format(internalformat, &ignored_explicit_alpha);
    if (known_atc && !driver_supports_atc() && upload) {
        std::vector<unsigned char> rgba;
        if (decode_atc(internalformat, width, height, image_size, data,
                       &rgba)) {
            upload(target, level, GL_RGBA, width, height, border,
                   GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            if (fallback_lines++ < 16) {
                trace("ATC: decoded level=%d format=0x%04x %dx%d "
                      "(%d compressed bytes -> %zu RGBA bytes)",
                      level, internalformat, width, height, image_size,
                      rgba.size());
            }
            if (gl_diag_enabled())
                gl_diag_after("ATC fallback glTexImage2D");
            return;
        }
        trace("ATC: invalid upload; forwarding to driver for GL error "
              "(level=%d format=0x%04x %dx%d bytes=%d)",
              level, internalformat, width, height, image_size);
    }

    bool known_pvrtc = false, ignored_two_bpp = false, ignored_opaque = false;
    known_pvrtc = pvrtc_format(internalformat, &ignored_two_bpp,
                              &ignored_opaque);
    /* Some Mali firmware advertises PVRTC but produces black textures when
     * the compressed upload is accepted.  Decode PVRTC unconditionally so
     * the Full donor is deterministic on both advertised and unsupported
     * drivers. */
    if (known_pvrtc && upload) {
        std::vector<unsigned char> rgba;
        if (decode_pvrtc(internalformat, width, height, image_size, data,
                         &rgba)) {
            upload(target, level, GL_RGBA, width, height, border,
                   GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            if (fallback_lines++ < 16) {
                trace("PVRTC: decoded level=%d format=0x%04x %dx%d "
                      "(%d compressed bytes -> %zu RGBA bytes)",
                      level, internalformat, width, height, image_size,
                      rgba.size());
            }
            if (gl_diag_enabled())
                gl_diag_after("PVRTC fallback glTexImage2D");
            return;
        }
        trace("PVRTC: invalid upload; forwarding to driver for GL error "
              "(level=%d format=0x%04x %dx%d bytes=%d)",
              level, internalformat, width, height, image_size);
    }

    if (compressed_upload) {
        compressed_upload(target, level, internalformat, width, height,
                          border, image_size, data);
    }

    if (gl_diag_enabled())
        gl_diag_after("glCompressedTexImage2D");
}

/*
 * Geometry diagnostics for real devices.
 *
 * A black/cropped frame can be a correct draw into the wrong rectangle.
 * Logging only changes, and only the first few, keeps LOADER_TRACE useful
 * without turning every frame into hundreds of lines.
 */
extern "C" void probe_glViewport(GLint x, GLint y, GLsizei width,
                                 GLsizei height)
{
    using Viewport = void (*)(GLint, GLint, GLsizei, GLsizei);
    static Viewport viewport =
        (Viewport)find_gles1_function("glViewport");
    static GLint last_x = -1, last_y = -1;
    static GLsizei last_w = -1, last_h = -1;
    static int lines = 0;

    if (lines < 24 &&
        (x != last_x || y != last_y || width != last_w || height != last_h)) {
        trace("GL viewport: x=%d y=%d width=%d height=%d",
              x, y, width, height);
        last_x = x; last_y = y; last_w = width; last_h = height;
        lines++;
    }
    if (viewport)
        viewport(x, y, width, height);
}

extern "C" void probe_glScissor(GLint x, GLint y, GLsizei width,
                                GLsizei height)
{
    using Scissor = void (*)(GLint, GLint, GLsizei, GLsizei);
    static Scissor scissor =
        (Scissor)find_gles1_function("glScissor");
    static GLint last_x = -1, last_y = -1;
    static GLsizei last_w = -1, last_h = -1;
    static int lines = 0;

    if (lines < 24 &&
        (x != last_x || y != last_y || width != last_w || height != last_h)) {
        trace("GL scissor: x=%d y=%d width=%d height=%d",
              x, y, width, height);
        last_x = x; last_y = y; last_w = width; last_h = height;
        lines++;
    }
    if (scissor)
        scissor(x, y, width, height);
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
    THUNK_SPECIFIC("glCompressedTexImage2D",
                   probe_glCompressedTexImage2D),
    THUNK_SPECIFIC("glViewport",      probe_glViewport),
    THUNK_SPECIFIC("glScissor",       probe_glScissor),
    { NULL, 0 },
};
