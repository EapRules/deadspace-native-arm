/*
 * Framebuffer probe: does the game actually draw anything?
 *
 * This is the one check that a stub cannot pass by accident. Every other
 * milestone can be reached by a shim that answers "fine" to everything - the
 * module loads, onCreate returns, a context appears, swaps happen - and the
 * result is a black screen that looks perfect in the logs. So the pixels get
 * read back and the port has to prove it painted something.
 *
 * Three constraints shape it:
 *
 *  - It runs on the GAME's thread, from inside eglSwapBuffers, because that is
 *    the thread that owns the context. The loader's thread has none.
 *  - It reads BEFORE the swap. After SDL_GL_SwapWindow the back buffer is
 *    undefined by specification, and under llvmpipe it really is.
 *  - It latches. glReadPixels flushes the whole pipeline, which is brutal on a
 *    software rasteriser inside qemu, so once a non-black frame is seen the
 *    probe switches itself off for the rest of the run and costs nothing.
 *
 * The probe leaves GL exactly as it found it (framebuffer binding, pack
 * alignment) with one honest exception: it cannot restore a pending GL error,
 * so it drains the error queue around itself and says so if it found one.
 */
#include <SDL2/SDL.h>

#include <stdlib.h>
#include <string.h>

/* Not -I'd: the GL types live with the Khronos thunks. */
#include "thunks/khronos/glad.h"

#include "fb_probe.h"
#include "so_util.h"
#include "trace.h"

/* One in this many frames is read back while the screen is still black. */
#define DEFAULT_INTERVAL 10

/*
 * A channel value below this counts as black. Not zero: a fade-in from black
 * legitimately produces 1-2/255 for a frame or two, and calling that "the game
 * renders" would be exactly the kind of generous reading this probe exists to
 * prevent. 16/255 is dim but unmistakably drawn.
 */
#define DEFAULT_THRESHOLD 16

static bool           g_done     = false;   /* found it, or gave up */
static int            g_interval = -1;      /* <0 = not read from the env yet */
static int            g_threshold = DEFAULT_THRESHOLD;
static unsigned char *g_buf      = NULL;
static size_t         g_buf_size = 0;

/*
 * The fixed-function loader keeps its driver pointers private to gles1.cpp.
 * The GLES2 glad globals in glad.h are a different table and are intentionally
 * never initialised for this game. Resolve the five integer-only functions
 * from the live GLES1 symbol table instead; none needs a softfp bridge.
 */
extern DynLibFunction symtable_gles1[];

static uintptr_t find_gles1_function(const char *name)
{
    for (int i = 0; symtable_gles1[i].symbol; i++) {
        if (strcmp(symtable_gles1[i].symbol, name) == 0)
            return symtable_gles1[i].func;
    }
    return 0;
}

static int env_int(const char *name, int def)
{
    const char *s = getenv(name);
    if (!s || !*s)
        return def;
    return atoi(s);
}

void android_fb_probe(long frame, int width, int height)
{
    if (g_done)
        return;

    if (g_interval < 0) {
        g_interval  = env_int("DEADSPACE_FB_PROBE_INTERVAL", DEFAULT_INTERVAL);
        g_threshold = env_int("DEADSPACE_FB_PROBE_THRESHOLD", DEFAULT_THRESHOLD);
        if (g_interval == 0) {
            g_done = true;          /* explicitly switched off */
            return;
        }
        if (g_interval < 1)
            g_interval = DEFAULT_INTERVAL;
        /* Clamped, not wrapped: an out-of-range threshold truncating to 0 would
         * turn this check into one that always says yes, which is the exact
         * failure it exists to catch. */
        if (g_threshold < 1)   g_threshold = 1;
        if (g_threshold > 255) g_threshold = 255;
    }

    if (frame % g_interval != 0)
        return;

    if (width <= 0 || height <= 0)
        return;

    using ReadPixels = void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                                void *);
    using GetIntegerv = void (*)(GLenum, GLint *);
    using GetError = GLenum (*)(void);
    using BindFramebuffer = void (*)(GLenum, GLuint);
    using PixelStorei = void (*)(GLenum, GLint);

    static ReadPixels read_pixels =
        (ReadPixels)find_gles1_function("glReadPixels");
    static GetIntegerv get_integerv =
        (GetIntegerv)find_gles1_function("glGetIntegerv");
    static GetError get_error =
        (GetError)find_gles1_function("glGetError");
    static BindFramebuffer bind_framebuffer =
        (BindFramebuffer)find_gles1_function("glBindFramebufferOES");
    static PixelStorei pixel_storei =
        (PixelStorei)find_gles1_function("glPixelStorei");

    if (!read_pixels || !get_integerv || !get_error ||
        !bind_framebuffer || !pixel_storei) {
        trace("framebuffer probe disabled: GL entry points unresolved");
        g_done = true;
        return;
    }

    size_t need = (size_t)width * (size_t)height * 4;
    if (need > g_buf_size) {
        unsigned char *nb = (unsigned char *)realloc(g_buf, need);
        if (!nb) {
            trace("framebuffer probe disabled: out of memory (%zu bytes)", need);
            g_done = true;
            return;
        }
        g_buf      = nb;
        g_buf_size = need;
    }

    /* Drain whatever was pending so the error we look at afterwards is ours.
     * This is the probe's only side effect on the game and the reason it stops
     * running the moment it has an answer. */
    bool had_pending = false;
    while (get_error() != GL_NO_ERROR)
        had_pending = true;

    /* The game normally swaps with the default framebuffer bound, but if it
     * left an FBO bound we would be reading its offscreen target instead of
     * what reaches the screen. */
    GLint prev_fbo = 0;
    get_integerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    if (prev_fbo != 0)
        bind_framebuffer(GL_FRAMEBUFFER, 0);

    GLint prev_align = 4;
    get_integerv(GL_PACK_ALIGNMENT, &prev_align);
    if (prev_align != 1)
        pixel_storei(GL_PACK_ALIGNMENT, 1);

    memset(g_buf, 0, need);
    read_pixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, g_buf);

    GLenum err = get_error();

    if (prev_align != 1)
        pixel_storei(GL_PACK_ALIGNMENT, prev_align);
    if (prev_fbo != 0)
        bind_framebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    while (get_error() != GL_NO_ERROR)
        ;

    if (err != GL_NO_ERROR) {
        trace("framebuffer probe disabled: glReadPixels failed (0x%04x)", err);
        g_done = true;
        return;
    }

    unsigned char best = 0;
    long lit = 0;
    for (size_t i = 0; i < need; i += 4) {
        unsigned char r = g_buf[i], g = g_buf[i + 1], b = g_buf[i + 2];
        unsigned char m = r > g ? r : g;
        if (b > m) m = b;
        if (m > best) best = m;
        if ((int)m >= g_threshold) lit++;
    }

    if (lit > 0) {
        trace("framebuffer non-black at frame %ld: %ld/%ld pixels >= %d, "
              "brightest channel %d%s",
              frame, lit, (long)(need / 4), g_threshold, (int)best,
              had_pending ? " (a GL error was already pending)" : "");
        /* Answered. Never stall the pipeline again. */
        g_done = true;
        free(g_buf);
        g_buf = NULL;
        g_buf_size = 0;
        return;
    }

    trace("framebuffer still black at frame %ld (brightest channel %d)",
          frame, (int)best);
}
