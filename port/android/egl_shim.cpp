/*
 * EGL over SDL.
 *
 * The game drives its own EGL: it asks for a display, picks a config, makes a
 * window surface out of the ANativeWindow it was handed, creates a GLES2
 * context and swaps it. There is no EGL on the R36S that will accept an
 * ANativeWindow, so these entry points sit on top of the one SDL window and
 * the one SDL GLES2 context the loader owns.
 *
 * Nothing here invents a result the host cannot back: eglMakeCurrent really
 * calls SDL_GL_MakeCurrent, eglSwapBuffers really swaps, eglQuerySurface
 * really reports the window size. Where the game asks something SDL cannot
 * answer (config attributes it never reads back), the call fails with a real
 * EGL error code rather than a plausible-looking zero.
 */
#include <SDL2/SDL.h>
#include <stdint.h>
#include <string.h>

#include <atomic>

#include "fb_probe.h"

extern "C" void android_cursor_draw(int fb_width, int fb_height);
#include "so_util.h"
#include "trace.h"

extern DynLibFunction symtable_gles2[];
extern DynLibFunction symtable_glprobe[];

extern "C" {

typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef int32_t      EGLint;
typedef void        *EGLDisplay;
typedef void        *EGLConfig;
typedef void        *EGLSurface;
typedef void        *EGLContext;
typedef void        *EGLNativeWindowType;
typedef void       (*__eglMustCastToProperFunctionPointerType)(void);

#define EGL_FALSE               0
#define EGL_TRUE                1
#define EGL_NO_DISPLAY          ((EGLDisplay)0)
#define EGL_NO_SURFACE          ((EGLSurface)0)
#define EGL_NO_CONTEXT          ((EGLContext)0)

#define EGL_SUCCESS             0x3000
#define EGL_NOT_INITIALIZED     0x3001
#define EGL_BAD_ATTRIBUTE       0x3004
#define EGL_BAD_PARAMETER       0x300C

#define EGL_BUFFER_SIZE         0x3020
#define EGL_ALPHA_SIZE          0x3021
#define EGL_BLUE_SIZE           0x3022
#define EGL_GREEN_SIZE          0x3023
#define EGL_RED_SIZE            0x3024
#define EGL_DEPTH_SIZE          0x3025
#define EGL_STENCIL_SIZE        0x3026
#define EGL_SAMPLES             0x3031
#define EGL_SURFACE_TYPE        0x3033
#define EGL_NATIVE_VISUAL_ID    0x302E
#define EGL_RENDERABLE_TYPE     0x3040
#define EGL_HEIGHT              0x3056
#define EGL_WIDTH               0x3057
#define EGL_DRAW                0x3059
#define EGL_READ                0x305A
#define EGL_OPENGL_ES_API       0x30A0

/* Opaque non-null cookies. The game only ever passes them back to us. */
#define THE_DISPLAY ((EGLDisplay)(uintptr_t)0xE61D1591)
#define THE_CONFIG  ((EGLConfig )(uintptr_t)0xE61CF6C0)
#define THE_SURFACE ((EGLSurface)(uintptr_t)0xE6157FAC)
#define THE_CONTEXT ((EGLContext)(uintptr_t)0xE61C07E1)

static SDL_Window    *g_window = NULL;
static SDL_GLContext  g_gl     = NULL;
static EGLint         g_error  = EGL_SUCCESS;
static bool           g_current = false;

/*
 * Frames the game has presented.
 *
 * eglSwapBuffers is the only place a frame can be counted honestly: it is the
 * game's own statement that it finished drawing one, as opposed to the loader
 * guessing from a timer. It is written on the game's thread and read on the
 * loader's, hence the atomic — a plain long would be a data race, and on a
 * weakly ordered ARM the reader could sit on a stale value forever.
 */
static std::atomic<long> g_frames{0};

static EGLBoolean fail(EGLint err)
{
    g_error = err;
    return EGL_FALSE;
}

/* Called by the loader once the real window and context exist. */
void android_egl_init(SDL_Window *window, SDL_GLContext gl)
{
    g_window = window;
    g_gl     = gl;
}

EGLint eglGetError(void)
{
    EGLint e = g_error;
    g_error = EGL_SUCCESS;
    return e;
}

EGLDisplay eglGetDisplay(void *display_id)
{
    (void)display_id;
    return g_window ? THE_DISPLAY : EGL_NO_DISPLAY;
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
    if (dpy != THE_DISPLAY)
        return fail(EGL_NOT_INITIALIZED);
    if (major) *major = 1;
    if (minor) *minor = 4;
    return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay dpy)
{
    return dpy == THE_DISPLAY ? EGL_TRUE : fail(EGL_NOT_INITIALIZED);
}

EGLBoolean eglBindAPI(EGLenum api)
{
    return api == EGL_OPENGL_ES_API ? EGL_TRUE : fail(EGL_BAD_PARAMETER);
}

/*
 * There is one config: the one SDL already gave us. Reporting a single choice
 * is honest - the caller cannot pick a pixel format we are able to change
 * after the fact, because the context is already alive.
 */
EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                           EGLConfig *configs, EGLint config_size,
                           EGLint *num_config)
{
    (void)attrib_list;
    if (dpy != THE_DISPLAY)
        return fail(EGL_NOT_INITIALIZED);

    if (configs && config_size > 0)
        configs[0] = THE_CONFIG;
    if (num_config)
        *num_config = (configs && config_size <= 0) ? 0 : 1;
    return EGL_TRUE;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                              EGLint attribute, EGLint *value)
{
    if (dpy != THE_DISPLAY || config != THE_CONFIG || !value)
        return fail(EGL_BAD_PARAMETER);

    SDL_GLattr want;
    switch (attribute) {
    case EGL_RED_SIZE:     want = SDL_GL_RED_SIZE;     break;
    case EGL_GREEN_SIZE:   want = SDL_GL_GREEN_SIZE;   break;
    case EGL_BLUE_SIZE:    want = SDL_GL_BLUE_SIZE;    break;
    case EGL_ALPHA_SIZE:   want = SDL_GL_ALPHA_SIZE;   break;
    case EGL_DEPTH_SIZE:   want = SDL_GL_DEPTH_SIZE;   break;
    case EGL_STENCIL_SIZE: want = SDL_GL_STENCIL_SIZE; break;
    case EGL_SAMPLES:      want = SDL_GL_MULTISAMPLESAMPLES; break;
    case EGL_BUFFER_SIZE: {
        int r = 0, g = 0, b = 0, a = 0;
        SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &r);
        SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &g);
        SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &b);
        SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &a);
        *value = r + g + b + a;
        return EGL_TRUE;
    }
    /* The game passes this straight to ANativeWindow_setBuffersGeometry,
     * where the format is ignored: SDL owns the surface format. */
    case EGL_NATIVE_VISUAL_ID:
        *value = 0;
        return EGL_TRUE;
    default:
        return fail(EGL_BAD_ATTRIBUTE);
    }

    int v = 0;
    if (SDL_GL_GetAttribute(want, &v) != 0)
        return fail(EGL_BAD_ATTRIBUTE);
    *value = v;
    return EGL_TRUE;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                  EGLNativeWindowType win, const EGLint *attrib_list)
{
    (void)win; (void)attrib_list;
    if (dpy != THE_DISPLAY || config != THE_CONFIG) {
        fail(EGL_BAD_PARAMETER);
        return EGL_NO_SURFACE;
    }
    return THE_SURFACE;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface)
{
    (void)surface;
    return dpy == THE_DISPLAY ? EGL_TRUE : fail(EGL_BAD_PARAMETER);
}

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config,
                            EGLContext share_context, const EGLint *attrib_list)
{
    (void)share_context; (void)attrib_list;
    if (dpy != THE_DISPLAY || config != THE_CONFIG || !g_gl) {
        fail(EGL_BAD_PARAMETER);
        return EGL_NO_CONTEXT;
    }
    return THE_CONTEXT;
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
    (void)ctx;
    return dpy == THE_DISPLAY ? EGL_TRUE : fail(EGL_BAD_PARAMETER);
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                          EGLContext ctx)
{
    (void)read;
    if (dpy != THE_DISPLAY)
        return fail(EGL_NOT_INITIALIZED);

    if (ctx == EGL_NO_CONTEXT || draw == EGL_NO_SURFACE) {
        SDL_GL_MakeCurrent(g_window, NULL);
        g_current = false;
        return EGL_TRUE;
    }

    if (SDL_GL_MakeCurrent(g_window, g_gl) != 0) {
        SDL_Log("eglMakeCurrent: SDL_GL_MakeCurrent failed: %s", SDL_GetError());
        return fail(EGL_BAD_PARAMETER);
    }
    g_current = true;
    return EGL_TRUE;
}

EGLContext eglGetCurrentContext(void)
{
    return g_current ? THE_CONTEXT : EGL_NO_CONTEXT;
}

/*
 * Not part of EGL: the loader asks whether the game has actually taken the
 * context. The game brings GL up on its own thread, so this is the only way
 * the loader's thread can tell the difference between "still loading assets"
 * and "never got a context".
 */
bool android_egl_is_current(void)
{
    return g_current;
}

EGLSurface eglGetCurrentSurface(EGLint readdraw)
{
    (void)readdraw;
    return g_current ? THE_SURFACE : EGL_NO_SURFACE;
}

EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface,
                           EGLint attribute, EGLint *value)
{
    if (dpy != THE_DISPLAY || surface != THE_SURFACE || !value)
        return fail(EGL_BAD_PARAMETER);

    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(g_window, &w, &h);
    switch (attribute) {
    case EGL_WIDTH:  *value = w; return EGL_TRUE;
    case EGL_HEIGHT: *value = h; return EGL_TRUE;
    default:         return fail(EGL_BAD_ATTRIBUTE);
    }
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{
    if (dpy != THE_DISPLAY || surface != THE_SURFACE)
        return fail(EGL_BAD_PARAMETER);

    /*
     * Read the pixels back before presenting them. This is the game's thread,
     * the only one holding the context, and the back buffer still holds the
     * frame it just drew - after SDL_GL_SwapWindow it does not. The probe
     * turns itself off as soon as it has seen a drawn frame, so the cost is
     * paid only while the answer is still "black".
     */
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(g_window, &w, &h);
    android_fb_probe(g_frames.load(std::memory_order_relaxed) + 1, w, h);

    /* The pointer the port synthesises for the menus has to be visible, and
     * this is the last moment the game's frame still exists. */
    android_cursor_draw(w, h);

    SDL_GL_SwapWindow(g_window);
    /* Counted after the swap, not before: a frame the driver refused to present
     * is not a frame. */
    trace("frame %ld", g_frames.fetch_add(1, std::memory_order_relaxed) + 1);
    return EGL_TRUE;
}

/*
 * Not part of EGL: how many frames the game has presented so far. The loader's
 * thread polls this to decide when a bounded run has seen enough.
 */
long android_egl_frames(void)
{
    return g_frames.load(std::memory_order_relaxed);
}

/*
 * The game resolves GL extensions through here. It has to come out of the
 * same table the loader binds into the module, otherwise the game would get a
 * hardfp host pointer for a softfp call and corrupt its float arguments.
 */
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procname)
{
    if (!procname)
        return NULL;
    /* The probes shadow two entries of symtable_gles2; resolving them here
     * first keeps a runtime lookup from bypassing the instrumentation. */
    for (int i = 0; symtable_glprobe[i].symbol; i++) {
        if (strcmp(symtable_glprobe[i].symbol, procname) == 0)
            return (__eglMustCastToProperFunctionPointerType)symtable_glprobe[i].func;
    }
    for (int i = 0; symtable_gles2[i].symbol; i++) {
        if (strcmp(symtable_gles2[i].symbol, procname) == 0)
            return (__eglMustCastToProperFunctionPointerType)symtable_gles2[i].func;
    }
    return NULL;
}

} /* extern "C" */

/*
 * The table the loader binds into libdeadspace.so for its libEGL.so imports.
 */
#include "thunk_gen.h"

DynLibFunction symtable_egl[] = {
    THUNK_DIRECT(eglBindAPI),
    THUNK_DIRECT(eglChooseConfig),
    THUNK_DIRECT(eglCreateContext),
    THUNK_DIRECT(eglCreateWindowSurface),
    THUNK_DIRECT(eglDestroyContext),
    THUNK_DIRECT(eglDestroySurface),
    THUNK_DIRECT(eglGetConfigAttrib),
    THUNK_DIRECT(eglGetCurrentContext),
    THUNK_DIRECT(eglGetCurrentSurface),
    THUNK_DIRECT(eglGetDisplay),
    THUNK_DIRECT(eglGetError),
    THUNK_DIRECT(eglGetProcAddress),
    THUNK_DIRECT(eglInitialize),
    THUNK_DIRECT(eglMakeCurrent),
    THUNK_DIRECT(eglQuerySurface),
    THUNK_DIRECT(eglSwapBuffers),
    THUNK_DIRECT(eglTerminate),
    { NULL, 0 },
};
