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
#include "viewport_scale.h"

/* Written by the game's thread, read by the loader's. */
static std::atomic<int> g_shaders_ok(0);
static std::atomic<int> g_shaders_failed(0);
static std::atomic<int> g_programs_ok(0);
static std::atomic<int> g_programs_failed(0);
static std::atomic<long> g_draws(0);
static std::atomic<long> g_textures(0);
static std::atomic<long> g_rgba_uploaded(0);
static std::atomic<long> g_subimages_uploaded(0);
static std::atomic<long> g_atc_decoded(0);
static std::atomic<long> g_pvrtc_native(0);
static std::atomic<long> g_pvrtc_decoded(0);
static std::atomic<long> g_compressed_passthrough(0);
static std::atomic<long> g_decode_failed(0);

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
    g_rgba_uploaded++;
    using TexImage2D = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
                                GLenum, GLenum, const void *);
    static TexImage2D upload =
        (TexImage2D)find_gles1_function("glTexImage2D");
    static int diagnostic_lines = 0;
    if (gl_diag_enabled()) {
        gl_diag_before("glTexImage2D");
        if (diagnostic_lines++ < 128) {
            unsigned int sample = 2166136261u;
            if (pixels) {
                const unsigned char *bytes =
                    static_cast<const unsigned char *>(pixels);
                size_t bytes_per_pixel = 4;
                if (type == GL_UNSIGNED_SHORT_4_4_4_4 ||
                    type == GL_UNSIGNED_SHORT_5_5_5_1 ||
                    type == GL_UNSIGNED_SHORT_5_6_5)
                    bytes_per_pixel = 2;
                else if (format == GL_RGB)
                    bytes_per_pixel = 3;
                else if (format == GL_LUMINANCE || format == GL_ALPHA)
                    bytes_per_pixel = 1;
                else if (format == GL_LUMINANCE_ALPHA)
                    bytes_per_pixel = 2;
                const size_t available = width > 0 && height > 0
                    ? static_cast<size_t>(width) *
                      static_cast<size_t>(height) * bytes_per_pixel : 0;
                const size_t sample_size = std::min<size_t>(64, available);
                for (size_t i = 0; i < sample_size; i++)
                    sample = (sample ^ bytes[i]) * 16777619u;
            }
            trace("GLDIAG: texture upload target=0x%04x level=%d "
                  "internal=0x%04x format=0x%04x type=0x%04x size=%dx%d "
                  "border=%d pixels=%p sample=%08x",
                  target, level, internalformat, format, type, width, height,
                  border, pixels, sample);
        }
    }
    if (upload) {
        upload(target, level, internalformat, width, height, border,
               format, type, pixels);
        if (gl_diag_enabled())
            gl_diag_after("glTexImage2D");
    }
}

extern "C" void probe_glTexSubImage2D(GLenum target, GLint level,
                                      GLint xoffset, GLint yoffset,
                                      GLsizei width, GLsizei height,
                                      GLenum format, GLenum type,
                                      const void *pixels)
{
    using TexSubImage2D = void (*)(GLenum, GLint, GLint, GLint, GLsizei,
                                   GLsizei, GLenum, GLenum, const void *);
    static TexSubImage2D upload =
        (TexSubImage2D)find_gles1_function("glTexSubImage2D");
    static int diagnostic_lines = 0;
    g_subimages_uploaded++;

    if (gl_diag_enabled()) {
        gl_diag_before("glTexSubImage2D");
        if (diagnostic_lines++ < 128) {
            unsigned int sample = 2166136261u;
            if (pixels) {
                const unsigned char *bytes =
                    static_cast<const unsigned char *>(pixels);
                size_t bytes_per_pixel = 4;
                if (type == GL_UNSIGNED_SHORT_4_4_4_4 ||
                    type == GL_UNSIGNED_SHORT_5_5_5_1 ||
                    type == GL_UNSIGNED_SHORT_5_6_5)
                    bytes_per_pixel = 2;
                else if (format == GL_RGB)
                    bytes_per_pixel = 3;
                else if (format == GL_LUMINANCE || format == GL_ALPHA)
                    bytes_per_pixel = 1;
                else if (format == GL_LUMINANCE_ALPHA)
                    bytes_per_pixel = 2;
                const size_t available = width > 0 && height > 0
                    ? static_cast<size_t>(width) *
                      static_cast<size_t>(height) * bytes_per_pixel : 0;
                const size_t sample_size = std::min<size_t>(64, available);
                for (size_t i = 0; i < sample_size; i++)
                    sample = (sample ^ bytes[i]) * 16777619u;
            }
            trace("GLDIAG: texture subupload target=0x%04x level=%d "
                  "offset=%d,%d format=0x%04x type=0x%04x size=%dx%d "
                  "pixels=%p sample=%08x",
                  target, level, xoffset, yoffset, format, type, width, height,
                  pixels, sample);
        }
    }
    if (upload) {
        upload(target, level, xoffset, yoffset, width, height, format, type,
               pixels);
        if (gl_diag_enabled())
            gl_diag_after("glTexSubImage2D");
    }
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

static bool driver_supports_pvrtc(void)
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
        extensions, "GL_IMG_texture_compression_pvrtc") ? 1 : 0;
    trace("PVRTC: native driver support %s; software fallback %s",
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

    if (opaque || getenv("DEADSPACE_PVRTC_FORCE_OPAQUE")) {
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
    /* Preserve the original native compressed-upload entry point for PVRTC.
     * ATC/RGBA fallback uploads use the live GLES1 table below.  On Mali,
     * routing the native PVRTC call through the thunk table changes the
     * driver's texture state and produces black models in the Full donor. */
    static CompressedUpload compressed_upload =
        (CompressedUpload)SDL_GL_GetProcAddress("glCompressedTexImage2D");
    if (!compressed_upload)
        compressed_upload =
            (CompressedUpload)find_gles1_function("glCompressedTexImage2D");
    static Upload upload = (Upload)SDL_GL_GetProcAddress("glTexImage2D");
    if (!upload)
        upload = (Upload)find_gles1_function("glTexImage2D");
    static int diagnostic_lines = 0;
    static int fallback_lines = 0;
    static bool mali_compat_logged = false;
    g_textures++;

    if (!mali_compat_logged && getenv("DEADSPACE_MALI_COMPAT")) {
        trace("Mali compatibility diagnostics: GLES1 table upload resolution active");
        mali_compat_logged = true;
    }

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
    /* ATC is always decoded on this port.  Some Mesa stacks advertise the
     * extension but either reject the old AMD enum or sample it differently;
     * the software result is deterministic on both the emulator and R36S. */
    if (known_atc && upload) {
        std::vector<unsigned char> rgba;
        if (decode_atc(internalformat, width, height, image_size, data,
                       &rgba)) {
            upload(target, level, GL_RGBA, width, height, border,
                   GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            g_atc_decoded++;
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
        g_decode_failed++;
        trace("ATC: invalid upload; forwarding to driver for GL error "
              "(level=%d format=0x%04x %dx%d bytes=%d)",
              level, internalformat, width, height, image_size);
    }

    bool known_pvrtc = false, ignored_two_bpp = false, ignored_opaque = false;
    known_pvrtc = pvrtc_format(internalformat, &ignored_two_bpp,
                              &ignored_opaque);
    /* Keep the original PVRTC path whenever the driver advertises IMG PVRTC.
     * That is the path on which the original Full donor was proven on the
     * R36S.  Software decoding is a capability fallback, never a donor-wide
     * switch, and can be forced only for diagnostics. */
    const bool force_pvrtc = getenv("DEADSPACE_PVRTC_SOFTWARE") != NULL;
    if (known_pvrtc && (force_pvrtc || !driver_supports_pvrtc()) && upload) {
        std::vector<unsigned char> rgba;
        if (decode_pvrtc(internalformat, width, height, image_size, data,
                         &rgba)) {
            upload(target, level, GL_RGBA, width, height, border,
                   GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            g_pvrtc_decoded++;
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
        g_decode_failed++;
        trace("PVRTC: invalid upload; forwarding to driver for GL error "
              "(level=%d format=0x%04x %dx%d bytes=%d)",
              level, internalformat, width, height, image_size);
    }

    if (compressed_upload) {
        compressed_upload(target, level, internalformat, width, height,
                          border, image_size, data);
        if (known_pvrtc)
            g_pvrtc_native++;
        else
            g_compressed_passthrough++;
    } else {
        g_decode_failed++;
        trace("compressed upload dropped: no GLES1 entry point format=0x%04x",
              internalformat);
    }

    if (gl_diag_enabled())
        gl_diag_after("glCompressedTexImage2D");
}

extern "C" void probe_glCompressedTexSubImage2D(
    GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
    GLsizei height, GLenum format, GLsizei image_size, const void *data)
{
    using CompressedSubUpload =
        void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum,
                 GLsizei, const void *);
    static CompressedSubUpload upload =
        (CompressedSubUpload)find_gles1_function("glCompressedTexSubImage2D");
    static int diagnostic_lines = 0;

    if (gl_diag_enabled()) {
        gl_diag_before("glCompressedTexSubImage2D");
        if (diagnostic_lines++ < 128) {
            trace("GLDIAG: compressed subupload target=0x%04x level=%d "
                  "offset=%d,%d format=0x%04x size=%dx%d bytes=%d data=%p",
                  target, level, xoffset, yoffset, format, width, height,
                  image_size, data);
        }
    }
    if (upload) {
        upload(target, level, xoffset, yoffset, width, height, format,
               image_size, data);
        if (gl_diag_enabled())
            gl_diag_after("glCompressedTexSubImage2D");
    }
}

/*
 * Aspect-correct scaling to the device's real panel.
 *
 * The engine renders at one fixed logical size - the 640x480 the loader asks
 * SDL for and hands to android_input_init - and issues a single full-screen
 * glViewport at that size, with glScissor calls in the same space. On a device
 * whose framebuffer already is that size (the R36S) nothing needs to change.
 * On a larger panel (the R36H Pro Max is 1024x768) that viewport lands in the
 * bottom-left corner and the rest of the panel stays black. We remap that one
 * full-screen viewport onto the real drawable and apply the matching affine
 * transform to scissor rectangles so clipped UI stays aligned; every other
 * viewport - a render target of a different size - is left untouched, since
 * that is exactly the call blind remapping would break.
 *
 * DEADSPACE_SCALE picks the policy:
 *   fit      (default) keep the logical aspect, letterbox, no distortion
 *   stretch  fill the panel, minor distortion
 *   integer  largest whole-number fit, letterboxed
 * The panel size comes from SDL_GL_GetDrawableSize (main.cpp calls the init);
 * DEADSPACE_PANEL_W/H override it, both to force a size on a CFW that reports
 * the wrong one and to exercise the path under the harness, which otherwise
 * renders at the logical size and would only ever take the identity branch.
 */
enum { SCALE_FIT = 0, SCALE_STRETCH = 1, SCALE_INTEGER = 2 };

static int     g_scale_active = 0;
static int     g_log_w = 640, g_log_h = 480;   /* engine's logical panel        */
static int     g_phys_w = 0, g_phys_h = 0;     /* the real drawable             */
static GLint   g_dst_x = 0, g_dst_y = 0;       /* full-screen rect on the panel */
static GLsizei g_dst_w = 0, g_dst_h = 0;
static float   g_scale_x = 1.0f, g_scale_y = 1.0f;

static int glprobe_env_int(const char *key, int fallback)
{
    const char *v = getenv(key);
    return (v && *v) ? atoi(v) : fallback;
}

extern "C" void viewport_scale_init(int phys_w, int phys_h,
                                    int log_w, int log_h)
{
    g_log_w = log_w > 0 ? log_w : 640;
    g_log_h = log_h > 0 ? log_h : 480;

    phys_w = glprobe_env_int("DEADSPACE_PANEL_W", phys_w);
    phys_h = glprobe_env_int("DEADSPACE_PANEL_H", phys_h);
    g_phys_w = phys_w;
    g_phys_h = phys_h;

    /* No panel info, or the panel already matches the logical size: pass every
     * call through untouched. This is the R36S path - identity, zero cost. */
    if (phys_w <= 0 || phys_h <= 0 ||
        (phys_w == g_log_w && phys_h == g_log_h)) {
        g_scale_active = 0;
        trace("viewport scale: identity (panel %dx%d, logical %dx%d)",
              phys_w, phys_h, g_log_w, g_log_h);
        return;
    }

    const char *m = getenv("DEADSPACE_SCALE");
    int mode = SCALE_FIT;
    if (m && !strcmp(m, "stretch"))      mode = SCALE_STRETCH;
    else if (m && !strcmp(m, "integer")) mode = SCALE_INTEGER;

    if (mode == SCALE_STRETCH) {
        g_dst_x = 0; g_dst_y = 0;
        g_dst_w = phys_w; g_dst_h = phys_h;
    } else {
        float sx = (float)phys_w / (float)g_log_w;
        float sy = (float)phys_h / (float)g_log_h;
        float s  = sx < sy ? sx : sy;          /* largest that fits          */
        if (mode == SCALE_INTEGER) {
            s = (float)(int)s;                 /* floor to a whole multiple  */
            if (s < 1.0f) s = 1.0f;
        }
        g_dst_w = (GLsizei)(g_log_w * s + 0.5f);
        g_dst_h = (GLsizei)(g_log_h * s + 0.5f);
        g_dst_x = (phys_w - g_dst_w) / 2;
        g_dst_y = (phys_h - g_dst_h) / 2;
    }

    g_scale_x = (float)g_dst_w / (float)g_log_w;
    g_scale_y = (float)g_dst_h / (float)g_log_h;
    g_scale_active = 1;
    trace("viewport scale: %s panel=%dx%d logical=%dx%d -> dst=%d,%d %dx%d",
          mode == SCALE_STRETCH ? "stretch" :
          mode == SCALE_INTEGER ? "integer" : "fit",
          phys_w, phys_h, g_log_w, g_log_h,
          g_dst_x, g_dst_y, g_dst_w, g_dst_h);
}

/*
 * The same affine the scissor path applies, for the port's own overlay.
 *
 * The software cursor lives in the engine's logical space - input_bridge.cpp
 * clamps it to 0..639 x 0..479 because that is what the touch events must
 * carry - but it is painted straight into the physical framebuffer. Without
 * this the arrow could only reach the logical rectangle in the corner of a
 * larger panel (GitHub issue #5, dArkOS at 1024x768).
 *
 * Y is top-left-origin here, unlike glScissor's bottom-left, so the offset is
 * the letterbox bar above the content rather than the one below it.
 */
extern "C" void viewport_scale_map(float *x, float *y)
{
    if (!g_scale_active)
        return;
    if (x)
        *x = (float)g_dst_x + *x * g_scale_x;
    if (y)
        *y = (float)(g_phys_h - g_dst_y - g_dst_h) + *y * g_scale_y;
}

extern "C" int viewport_scale_factor(void)
{
    if (!g_scale_active)
        return 1;
    float s = g_scale_x < g_scale_y ? g_scale_x : g_scale_y;
    int factor = (int)(s + 0.5f);
    return factor < 1 ? 1 : factor;
}

/* The engine's one full-screen viewport, the only one we remap. */
static inline int is_fullscreen_viewport(GLint x, GLint y,
                                         GLsizei w, GLsizei h)
{
    return g_scale_active && x == 0 && y == 0 &&
           w == g_log_w && h == g_log_h;
}

/*
 * Geometry diagnostics for real devices, now also the scaling seam.
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

    if (is_fullscreen_viewport(x, y, width, height)) {
        x = g_dst_x; y = g_dst_y; width = g_dst_w; height = g_dst_h;
    }

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

    /* Every scissor is in the logical panel's space (the engine uses one
     * viewport), so the same affine transform keeps clipped UI aligned with
     * the scaled content. Identity when scaling is off. */
    if (g_scale_active) {
        x = g_dst_x + (GLint)(x * g_scale_x + 0.5f);
        y = g_dst_y + (GLint)(y * g_scale_y + 0.5f);
        width  = (GLsizei)(width  * g_scale_x + 0.5f);
        height = (GLsizei)(height * g_scale_y + 0.5f);
    }

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
extern "C" long android_gl_rgba_uploaded(void) { return g_rgba_uploaded.load(); }
extern "C" long android_gl_subimages_uploaded(void)
{
    return g_subimages_uploaded.load();
}
extern "C" long android_gl_atc_decoded(void) { return g_atc_decoded.load(); }
extern "C" long android_gl_pvrtc_native(void) { return g_pvrtc_native.load(); }
extern "C" long android_gl_pvrtc_decoded(void) { return g_pvrtc_decoded.load(); }
extern "C" long android_gl_compressed_passthrough(void)
{
    return g_compressed_passthrough.load();
}
extern "C" long android_gl_decode_failed(void) { return g_decode_failed.load(); }

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
    THUNK_SPECIFIC("glTexSubImage2D", probe_glTexSubImage2D),
    THUNK_SPECIFIC("glCompressedTexImage2D",
                   probe_glCompressedTexImage2D),
    THUNK_SPECIFIC("glCompressedTexSubImage2D",
                   probe_glCompressedTexSubImage2D),
    THUNK_SPECIFIC("glViewport",      probe_glViewport),
    THUNK_SPECIFIC("glScissor",       probe_glScissor),
    { NULL, 0 },
};
