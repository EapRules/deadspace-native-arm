/*
 * Dead Space loader — entry point.
 *
 * The port never ships game data. It takes the path to the user's own APK,
 * mounts it as the Android asset source, and maps the armeabi-v7a
 * libdeadspace.so out of it with the bionic ELF loader vendored from
 * gmloader-next.
 *
 * Milestone M3 starts here: the module is mapped, relocated, every import it
 * declares is bound to a real host implementation, and the activity object is
 * built and handed to ANativeActivity_onCreate.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <zip.h>

#include "so_util.h"
#include "khronos/gles2.h"

#include "jni.h"
#include "classes/app_NativeActivity.h"

#include "android_api.h"
#include "crash.h"
#include "trace.h"

extern "C" void android_assets_init(zip_t *apk);
extern "C" AAssetManager *android_assets_manager(void);
extern "C" void android_platform_init(SDL_Window *win, int w, int h);
extern "C" void android_platform_open_controllers(void);
extern "C" ANativeWindow *android_platform_window(void);
extern "C" AInputQueue *android_platform_queue(void);
extern "C" bool android_platform_finished(void);
extern "C" void android_egl_init(SDL_Window *win, SDL_GLContext gl);
extern "C" bool android_egl_is_current(void);
extern "C" long android_egl_frames(void);
extern "C" int android_gl_shaders_compiled(void);
extern "C" int android_gl_shaders_failed(void);
extern "C" int android_gl_programs_linked(void);
extern "C" int android_gl_programs_failed(void);
extern "C" long android_gl_draw_calls(void);
extern "C" long android_assets_opened(void);

/*
 * What the engine actually did, in one line.
 *
 * Frames alone do not tell a running game from a stalled one holding a
 * surface: a solid-colour clear presents just as happily as a rendered scene.
 * So the run also reports the three things only real content produces -
 * assets read out of the APK, GLSL programs the driver linked, and draw calls
 * issued. Every one of those numbers is incremented at the single place the
 * work actually happens (AAssetManager_open, glLinkProgram, glDraw*), never
 * here; this function only reads them.
 *
 * Printed on every way out of the run, the failures included, because the
 * counts are most useful precisely when something went wrong.
 */
static void trace_summary(void)
{
    trace("summary assets=%ld programs=%d draws=%ld",
          android_assets_opened(), android_gl_programs_linked(),
          android_gl_draw_calls());
}

/* The game ships one ABI only; there is no arm64 build of Dead Space.
 * so_load_module() takes a bare soname and prefixes "lib/<abi>/" itself, the
 * same way the platform linker does, so the full path is only used for the
 * "is this really the right APK?" check. */
static const char *kNativeLib     = "libdeadspace.so";
static const char *kNativeLibPath = "lib/armeabi-v7a/libdeadspace.so";

/* The R36S panel. The window is created at this size so the game's very first
 * eglQuerySurface already reports the resolution it will render at. */
static const int kWidth  = 640;
static const int kHeight = 480;

/*
 * The API level the game is told it is running on.
 *
 * 19 (KitKat) is the floor that still has every NDK entry point the game
 * imports, and it is old enough that the engine never takes a code path
 * guarded by a newer level - runtime permissions, scoped storage, the
 * AAssetManager_fromJava/AHardwareBuffer surface. Claiming a higher level
 * would only invite calls into platform APIs this loader does not answer.
 */
static const int32_t kSdkVersion = 19;

/*
 * The game's writable storage. On Android this is /data/data/<pkg>/files and
 * it always exists before onCreate; here it has to be created, because the
 * engine treats a failed open of its savegame directory as a fatal error
 * rather than as "no save yet".
 *
 * The PortMaster launcher points DEADSPACE_DATA_DIR at the port's own data
 * directory; the default keeps a standalone run self-contained.
 */
static char g_data_path[PATH_MAX];

static const char *ensure_data_dir(void)
{
    const char *base = getenv("DEADSPACE_DATA_DIR");
    if (!base || !*base)
        base = "gamedata";

    snprintf(g_data_path, sizeof(g_data_path), "%s", base);

    /* mkdir -p: every component, ignoring the ones that already exist. */
    for (char *p = g_data_path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(g_data_path, 0755) != 0 && errno != EEXIST)
            return NULL;
        *p = '/';
    }
    if (mkdir(g_data_path, 0755) != 0 && errno != EEXIST)
        return NULL;

    return g_data_path;
}

/*
 * Walk the module's dynamic symbol table and name every import that nothing
 * answers.
 *
 * The loader itself does not do this: it points unresolved jump slots at a
 * stub that aborts the first time the game calls one, which surfaces a single
 * missing symbol per run, from inside a crash, with no stack. Auditing up
 * front lists all of them at once and before any game code executes.
 *
 * Undefined *weak* symbols are not failures: resolving them to zero is what a
 * real dynamic linker does, and the game tests them for null before use.
 */
static int report_unresolved_symbols(so_module *mod)
{
    int missing = 0;

    for (int i = 0; i < mod->num_dynsym; i++) {
        Elf_Sym *sym = &mod->dynsym[i];
        if (sym->st_shndx != SHN_UNDEF)
            continue;

        const char *name = mod->dynstr + sym->st_name;
        if (!name || !*name)
            continue;

        if (so_resolve_link(mod, name))
            continue;

        if (ELF32_ST_BIND(sym->st_info) == STB_WEAK) {
            trace("weak import left null: %s", name);
            continue;
        }

        fprintf(stderr, "unresolved symbol: %s\n", name);
        missing++;
    }

    fflush(stderr);
    return missing;
}

int main(int argc, char **argv)
{
    /*
     * Unbuffered from the first line, because the log is the only diagnostic
     * that leaves the console and a crash must not take it with it.
     *
     * Doing it here rather than with stdbuf is the difference between working
     * and not: stdbuf injects a 64-bit libstdbuf.so via LD_PRELOAD, which a
     * 32-bit process like this one cannot load at all. The launcher checks for
     * that now, but relying on an external tool for something this important
     * was the wrong shape - the program can simply ask for it itself.
     */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <deadspace.apk>\n"
                "\n"
                "The APK is your own copy of the game; it is never bundled\n"
                "with this port.\n",
                argv[0]);
        return 2;
    }

    const char *apk_path = argv[1];

    int zerr = 0;
    zip_t *apk = zip_open(apk_path, ZIP_RDONLY, &zerr);
    if (!apk) {
        zip_error_t err;
        zip_error_init_with_code(&err, zerr);
        fatal("cannot open APK '%s': %s", apk_path, zip_error_strerror(&err));
        zip_error_fini(&err);
        return 1;
    }
    trace("apk opened: %s (%lld entries)", apk_path,
          (long long)zip_get_num_entries(apk, 0));

    /* From here on AAssetManager_* reads straight out of the zip. */
    android_assets_init(apk);

    zip_stat_t st;
    zip_stat_init(&st);
    if (zip_stat(apk, kNativeLibPath, 0, &st) != 0) {
        fatal("'%s' is not inside '%s'.\n"
              "       This does not look like the armeabi-v7a build of\n"
              "       Dead Space.",
              kNativeLibPath, apk_path);
        zip_close(apk);
        return 1;
    }
    trace("native library found: %s (%llu bytes)", kNativeLibPath,
          (unsigned long long)st.size);

    /*
     * The window and the GLES2 context have to exist before the module is
     * relocated, not after: the GL import table is filled in by asking the
     * driver for each entry point, and there is no driver to ask until a
     * context is current. Relocating first would bind every gl* call to null.
     */
    /*
     * Game controller support is not optional here, and it was missing until
     * now: android/platform.cpp translates pad input into the Android events
     * the engine reads, but SDL was only ever initialised for video, so none
     * of that code had ever run. The engine reached its menu and waited for an
     * input that could not arrive.
     *
     * Joystick/gamecontroller init is allowed to fail: a device with no pad
     * attached should still boot to the menu rather than refuse to start.
     */
    /*
     * Pick the GLES driver explicitly, before anything loads a GL library.
     *
     * On a CFW built around Mesa/Panfrost rather than the Mali blob - ROCKNIX
     * on this same RK3326 - the driver exposes desktop OpenGL 3.1 *and* GLES 3.1
     * on the same device. Left to itself, the X11/GLX path hands back a desktop
     * context, the game's GLSL ES shaders refuse to compile, and the result is a
     * black screen with working audio: a failure that looks like the port is
     * broken and says nothing about why.
     *
     * This is not forcing a video driver, which would break Wayland and X11
     * differently. It only chooses which GL library gets loaded, and it is inert
     * on the devices that only ever had GLES to offer, like this one with libmali.
     *
     * Credit where due: diagnosed and documented by NextOs-Ports in
     * sonic4ep2-nextos (STUDY_DEVICE_COMPAT.md). We have no ROCKNIX device to
     * verify it on, so DEADSPACE_NO_FORCE_GLES exists to turn it back off.
     */
    if (!getenv("DEADSPACE_NO_FORCE_GLES")) {
        SDL_SetHint("SDL_OPENGL_ES_DRIVER", "1");
        SDL_SetHint("SDL_VIDEO_X11_FORCE_EGL", "1");
        trace("forcing the GLES/EGL driver (for Mesa/Panfrost CFWs)");
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fatal("SDL_Init failed: %s", SDL_GetError());
        zip_close(apk);
        return 1;
    }

    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0)
        trace("no controller subsystem: %s", SDL_GetError());
    else
        android_platform_open_controllers();

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);

    SDL_Window *window = SDL_CreateWindow(
        "Dead Space", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        kWidth, kHeight, SDL_WINDOW_OPENGL);
    if (!window) {
        fatal("SDL_CreateWindow failed: %s", SDL_GetError());
        zip_close(apk);
        return 1;
    }

    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl) {
        fatal("could not create a GLES2 context: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        zip_close(apk);
        return 1;
    }

    android_platform_init(window, kWidth, kHeight);
    android_egl_init(window, gl);

    /* Fills symtable_gles2 from the live driver. */
    load_gles2_funcs();

    /*
     * Say out loud which GL we ended up on.
     *
     * A desktop GL context here is fatal in practice - the game's shaders are
     * GLSL ES and will not compile against GLSL 1.40 - but it fails as a black
     * screen with working sound, which is the least informative symptom
     * possible. One line in the log turns a night of guessing into an answer.
     */
    {
        const GLubyte *(*get_string)(GLenum) =
            (const GLubyte *(*)(GLenum))SDL_GL_GetProcAddress("glGetString");
        if (get_string) {
            const char *ver = (const char *)get_string(0x1F02 /* GL_VERSION */);
            const char *rnd = (const char *)get_string(0x1F01 /* GL_RENDERER */);
            trace("GL_VERSION=%s | GL_RENDERER=%s", ver ? ver : "?", rnd ? rnd : "?");
            if (ver && !strstr(ver, "ES") && !strstr(ver, "es"))
                trace("WARNING: this is desktop GL, not GLES - the game's shaders "
                      "will not compile and the screen will stay black. A CFW on "
                      "Mesa/Panfrost needs the GLES driver forced; see "
                      "DEADSPACE_NO_FORCE_GLES.");
        }
    }

    /*
     * The fake JavaVM has to exist before the module is linked, not after:
     * so_load_module() calls JNI_OnLoad if the module exports one, and that
     * hook receives the JavaVM* as its first argument. Handing it NULL would
     * crash inside the load, i.e. before there is anything to report.
     */
    JavaVM  *vm  = NULL;
    JNIEnv  *env = NULL;
    if (JNI_CreateJavaVM(&vm, &env, NULL) != JNI_OK || !vm || !env) {
        fatal("could not create the JNI environment.");
        zip_close(apk);
        return 1;
    }

    /*
     * Map, relocate and link the module. so_load_module also walks DT_NEEDED,
     * but every dependency the game declares (libc, libm, libdl, liblog,
     * libandroid, libz) is provided by this loader rather than loaded from the
     * APK, so libdeadspace.so is the only object that gets mapped.
     */
    so_module *mod = so_load_module(kNativeLib, apk, vm);
    if (!mod) {
        fatal("could not map '%s' out of the APK.", kNativeLib);
        zip_close(apk);
        return 1;
    }

    int missing = report_unresolved_symbols(mod);
    if (missing > 0) {
        fatal("%d import(s) of %s have no implementation (listed above).\n"
              "       Running the game now would abort on the first call to\n"
              "       any of them.",
              missing, kNativeLib);
        zip_close(apk);
        return 1;
    }

    trace("module loaded");

    /* From here on the game's own code runs, and a fault inside it would
     * otherwise be reported as a bare address with no context. */
    crash_report_init(mod, kNativeLib);

    /* ---------------------------------------------------------------- *
     * M3: hand the game an ANativeActivity and start it.
     * ---------------------------------------------------------------- */
    const char *data_path = ensure_data_dir();
    if (!data_path) {
        fatal("cannot create the save directory '%s': %s\n"
              "       Set DEADSPACE_DATA_DIR to a writable location.",
              g_data_path, strerror(errno));
        zip_close(apk);
        return 1;
    }
    trace("data dir: %s", data_path);

    /*
     * The framework guarantees the trailing slash on these paths, and the
     * engine concatenates without adding one, so it is not cosmetic.
     */
    char data_dir[PATH_MAX + 2];
    snprintf(data_dir, sizeof(data_dir), "%s/", data_path);

    static ANativeActivityCallbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));

    static ANativeActivity activity;
    memset(&activity, 0, sizeof(activity));
    activity.callbacks = &callbacks;
    activity.vm        = vm;
    activity.env       = env;
    activity.clazz     = (void *)&g_native_activity_object;
    /* Both paths point at the same place: the port has no equivalent of the
     * internal/external split, and the engine only ever writes saves. */
    activity.internalDataPath = data_dir;
    activity.externalDataPath = data_dir;
    activity.obbPath          = data_dir;
    activity.sdkVersion       = kSdkVersion;
    activity.instance         = NULL;
    activity.assetManager     = android_assets_manager();

    ANativeActivity_createFunc *on_create =
        (ANativeActivity_createFunc *)so_symbol(mod, "ANativeActivity_onCreate");
    if (!on_create) {
        fatal("%s exports no ANativeActivity_onCreate.", kNativeLib);
        zip_close(apk);
        return 1;
    }

    trace("calling onCreate");
    on_create(&activity, NULL, 0);
    trace("onCreate returned");

    /* ---------------------------------------------------------------- *
     * M4: make the activity visible so the game brings GL up.
     *
     * onCreate only spawned the game's thread; that thread is parked inside
     * native_app_glue waiting to be told it has a window. Everything below
     * happens on the game's thread, driven from here through the callback
     * table it filled in during onCreate.
     * ---------------------------------------------------------------- */

    /*
     * Hand the GL context over. SDL allows a context to be current on one
     * thread at a time, and it was made current here because load_gles2_funcs()
     * needed a live driver to query. From this point the game owns it, so this
     * thread has to let go or the game's eglMakeCurrent will fail.
     */
    SDL_GL_MakeCurrent(window, NULL);

    /*
     * Every one of these is a command on native_app_glue's pipe, and each
     * blocks this thread until the game's thread has read it back out. That
     * makes the order load-bearing in a way the Android docs do not spell out:
     * the window is what starts the engine's bring-up, and the game's thread
     * disappears into it for as long as it takes to create the GL context and
     * load its assets. Anything sent afterwards sits unread in the pipe with
     * this thread blocked on it, so the window goes last.
     */
    trace("driving the lifecycle");
    if (callbacks.onStart)
        callbacks.onStart(&activity);
    if (callbacks.onResume)
        callbacks.onResume(&activity);
    if (callbacks.onInputQueueCreated)
        callbacks.onInputQueueCreated(&activity, android_platform_queue());
    if (callbacks.onWindowFocusChanged)
        callbacks.onWindowFocusChanged(&activity, 1);
    if (callbacks.onNativeWindowCreated)
        callbacks.onNativeWindowCreated(&activity, android_platform_window());
    trace("activity resumed");

    /*
     * Wait for the engine to finish its own GL bring-up. It loads assets out
     * of the APK first, so this is not instant; the deadline exists only so a
     * game that never gets there fails with a diagnosis instead of hanging
     * until the harness kills it.
     *
     * Nothing SDL is touched from here: the game's thread is inside
     * ALooper_pollAll, which is what pumps SDL now.
     */
    const char *gl_wait_env = getenv("DEADSPACE_GL_TIMEOUT");
    const int   gl_wait_s   = gl_wait_env ? atoi(gl_wait_env) : 60;

    bool gl_ready = false;
    for (int waited_ms = 0; waited_ms < gl_wait_s * 1000; waited_ms += 20) {
        if (android_gl_shaders_failed() > 0 || android_gl_programs_failed() > 0)
            break;

        if (android_egl_is_current() && android_gl_shaders_compiled() > 0) {
            gl_ready = true;
            break;
        }

        if (android_platform_finished()) {
            fatal("the game ended the activity before GL came up.");
            fflush(NULL);
            _exit(1);
        }

        usleep(20 * 1000);
    }

    if (android_gl_shaders_failed() > 0 || android_gl_programs_failed() > 0) {
        fatal("the game's shaders do not build on this driver: %d shader(s)\n"
              "       failed to compile and %d program(s) failed to link\n"
              "       (the driver's own logs are above).",
              android_gl_shaders_failed(), android_gl_programs_failed());
        fflush(NULL);
        _exit(1);
    }

    if (!gl_ready) {
        fatal("the game never brought GL up within %ds: EGL context %s,\n"
              "       %d shader(s) compiled. Set DEADSPACE_GL_TIMEOUT to wait\n"
              "       longer if the asset load is simply slow here.",
              gl_wait_s,
              android_egl_is_current() ? "is current" : "was never made current",
              android_gl_shaders_compiled());
        fflush(NULL);
        _exit(1);
    }

    trace("gl ready (%d shaders compiled, %d programs linked)",
          android_gl_shaders_compiled(), android_gl_programs_linked());

    /* ---------------------------------------------------------------- *
     * M5: the game owns the process now.
     *
     * There is deliberately no frame loop here. The engine runs its own, on
     * its own thread, and drives it from ALooper_pollAll; a second loop on
     * this thread would only fight it for the GL context. So this thread does
     * nothing but watch: it counts the frames the game presents through
     * eglSwapBuffers and decides when the run is over.
     *
     * Nothing SDL is touched from here — see the note above eglSwapBuffers.
     * ---------------------------------------------------------------- */

    /*
     * A bounded run. DEADSPACE_FRAME_LIMIT stops the process after that many
     * frames so an automated run terminates on a fact rather than on a
     * stopwatch; unset (the normal case for a player) means run until the game
     * or the user says otherwise.
     */
    const char *limit_env   = getenv("DEADSPACE_FRAME_LIMIT");
    const long  frame_limit = limit_env ? atol(limit_env) : 0;

    /*
     * A game that is alive but has stopped presenting is the failure this loop
     * exists to catch: without it the process would idle until something else
     * killed it and the log would say nothing about why. The window is
     * generous because the engine streams assets between frames.
     */
    const char *stall_env = getenv("DEADSPACE_FRAME_STALL");
    const int   stall_s   = stall_env ? atoi(stall_env) : 30;

    if (frame_limit > 0)
        trace("running until %ld frames", frame_limit);

    long last_frames = 0;
    int  stalled_ms  = 0;

    while (!android_platform_finished()) {
        long frames = android_egl_frames();

        if (frames != last_frames) {
            last_frames = frames;
            stalled_ms  = 0;
        } else if (stall_s > 0 && stalled_ms >= stall_s * 1000) {
            fatal("the game stopped presenting after %ld frame(s): nothing was\n"
                  "       swapped for %ds and the activity is still running.",
                  frames, stall_s);
            trace_summary();
            fflush(NULL);
            _exit(1);
        }

        if (frame_limit > 0 && frames >= frame_limit) {
            trace("frame limit reached (%ld frames)", frames);
            break;
        }

        usleep(20 * 1000);
        stalled_ms += 20;
    }

    trace("run finished: %ld frames%s", android_egl_frames(),
          android_platform_finished() ? " (the game ended the activity)" : "");
    trace_summary();

    /*
     * onCreate started the game's own thread and it is still running. Tearing
     * the window, the GL context and the zip down from here would pull them
     * out from under it and produce a segfault that has nothing to do with why
     * the loader stopped. Leave that to the kernel.
     */
    fflush(NULL);
    _exit(0);
}
