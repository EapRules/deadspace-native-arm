/*
 * Dead Space loader — entry point.
 *
 * This file used to be the Minigore 2 / Ice Rage driver: open an APK, map the
 * game's .so out of it, build an ANativeActivity and hand it to
 * ANativeActivity_onCreate, then watch the engine run its own frame loop.
 *
 * None of that applies here, and the run log said so in one line:
 *
 *     FATAL: cannot open APK '/game': Operation not supported
 *
 * because the harness passes a *directory*. That error is only the surface of
 * it. Dead Space is not a NativeActivity game at all:
 *
 *   - it has no DT_NEEDED on libandroid.so and exports no
 *     ANativeActivity_onCreate; its DT_NEEDED list is exactly
 *     libc / libstdc++ / libm / liblog / libGLESv1_CM
 *   - it exports JNI_OnLoad plus 68 Java_* entry points and expects a Java
 *     layer to call them, frame by frame. That layer is this file.
 *   - EA shipped it with the assets outside the package; what circulates is
 *     the extracted tree, so there is no zip to open in the first place.
 *
 * So the driver is inverted with respect to the other two ports: *we* own the
 * frame loop and call into the engine, instead of feeding lifecycle commands
 * to an engine that owns its own.
 *
 * The call order below is not deduced from the symbol names - it is copied
 * from deadspace_main() in the Vita port (vita-ref/loader/main.c, MIT), which
 * runs this exact build. Two of its choices are worth naming because the
 * obvious alternative is wrong:
 *
 *   - the entry point is NativeOnCreate. The .so also exports runEntryPoint,
 *     which reads like the real one; the working port never calls it.
 *   - NativeOnSurfaceChanged is never called either. The renderer takes its
 *     resolution once, at surface-created time.
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

#include "so_util.h"
#include "khronos/gles2.h"

#include "jni.h"

#include "crash.h"
#include "trace.h"

/*
 * The library, and where it lives inside the game tree.
 *
 * "armeabi", not "armeabi-v7a": this is a 2011 Xperia Play build, from before
 * the v7a split was routine, and it is genuinely ARMv5TE+VFPv2 code. The
 * loader's own search path derives the ABI directory from the host
 * architecture and would look under armeabi-v7a, so the game directory is
 * pointed at with the ABI folder already included - see so_set_options()
 * below.
 */
static const char *kNativeLib    = "libEAMGameDeadSpace.so";
static const char *kNativeLibDir = "lib/armeabi";

/* The R36S panel. Fixed: the engine asks for the surface size once and the
 * Vita port never calls NativeOnSurfaceChanged, so there is no resize path to
 * exercise. */
static const int kWidth  = 640;
static const int kHeight = 480;

/*
 * Walk the module's dynamic symbol table and name every import that nothing
 * answers.
 *
 * The loader itself does not do this: it points unresolved jump slots at a
 * stub that aborts the first time the game calls one, which surfaces a single
 * missing symbol per run, from inside a crash, with no stack. Auditing up
 * front lists all of them at once.
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

/*
 * The loader's post-relocation hook (see so_util.h).
 *
 * "The ELF mapped and relocated" and "every import found an implementation"
 * are two different facts and are reported as two, in that order. Collapsing
 * them - printing the milestone line only on full success - is what the first
 * run of this driver did, and the log it produced said "module never loaded"
 * about a module that had in fact mapped, relocated and then died inside a
 * static constructor calling one of 205 unbound imports. Naming them is the
 * whole point of being here rather than after so_initialize().
 */
extern "C" int so_after_relocate(so_module *mod)
{
    trace("module loaded");

    int missing = report_unresolved_symbols(mod);
    if (missing == 0)
        return 0;

    fatal("%d import(s) of %s have no implementation (listed above).\n"
          "       Running the game now would fault on the first call to any\n"
          "       of them, from inside a static constructor, with nothing but\n"
          "       an address to go on.",
          missing, mod->soname ? mod->soname : "the module");
    return 1;
}

int main(int argc, char **argv)
{
    /*
     * Unbuffered from the first line, because the log is the only diagnostic
     * that leaves the console and a crash must not take it with it.
     *
     * Doing it here rather than with stdbuf is the difference between working
     * and not: stdbuf injects a 64-bit libstdbuf.so via LD_PRELOAD, which a
     * 32-bit process like this one cannot load at all.
     */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <deadspace-directory>\n"
                "\n"
                "The directory is your own copy of the game - the extracted\n"
                "tree with lib/armeabi/ and assets/published/ in it. It is\n"
                "never bundled with this port.\n",
                argv[0]);
        return 2;
    }

    const char *game_dir = argv[1];

    char lib_dir[PATH_MAX];
    char lib_path[PATH_MAX];
    snprintf(lib_dir,  sizeof(lib_dir),  "%s/%s", game_dir, kNativeLibDir);
    snprintf(lib_path, sizeof(lib_path), "%s/%s", lib_dir, kNativeLib);

    struct stat st;
    if (stat(lib_path, &st) != 0) {
        fatal("'%s' does not exist.\n"
              "       This does not look like an extracted Dead Space tree.",
              lib_path);
        return 1;
    }
    trace("native library found: %s (%lld bytes)", lib_path, (long long)st.st_size);

    /*
     * Bring GL up before the module is linked, not after: the GL import table
     * is filled in by asking the driver for each entry point, and there is no
     * driver to ask until a context is current. Relocating first would bind
     * every gl* call to null.
     *
     * A failure here is deliberately *not* fatal. The point of separating the
     * milestones is that "does every import resolve?" and "is there a usable
     * GLES context?" are different questions with different answers; aborting
     * on the second one would make the first unanswerable from the log. The
     * run continues and the missing context shows up later, at the milestone
     * that is actually about it.
     */
    SDL_Window   *window = NULL;
    SDL_GLContext gl     = NULL;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        trace("SDL_Init(video) failed: %s", SDL_GetError());
    } else {
        /*
         * Fixed function, whichever way this machine provides it.
         *
         * The game imports 190 fixed-function entry points (glMatrixMode,
         * glVertexPointer, the OES matrix palette) and exactly zero shader
         * ones, so an ES2 context resolves nothing the engine calls.
         *
         * But asking for an ES 1.1 context is not portable either, and that is
         * the trap that cost two agent-loop iterations. Under the harness
         * (Mesa + llvmpipe) eglCreateContext returns EGL_BAD_ALLOC for client
         * version 1 while version 2 succeeds, because Debian's Mesa is built
         * without GLES1 - libGLESv1_CM.so.1 is there, but it is glvnd's
         * dispatch stub with no vendor behind it. The failure reads like an
         * EGL attribute problem and is not one.
         *
         * A desktop compatibility-profile context has the same fixed function
         * natively - GLES 1.1 is the subset, not a different API - and Mesa
         * exports all 190 names including the glClearColorx fixed-point family
         * and the *OES entry points. Measured: ES1 fails, ES2 and desktop both
         * succeed, and desktop draws a triangle that reads back correctly.
         *
         * So: try ES 1.1 first, because a device whose driver really speaks it
         * (the console's Mali blob does) should use it directly; fall back to
         * compatibility profile, which is what the harness gets. Either way
         * gl_provider_open() in gles1.cpp decides which library the 190 imports
         * are bound to, and says so in the log.
         */
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);

        window = SDL_CreateWindow("Dead Space",
                                  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  kWidth, kHeight, SDL_WINDOW_OPENGL);
        if (!window) {
            trace("SDL_CreateWindow failed: %s", SDL_GetError());
        } else {
            gl = SDL_GL_CreateContext(window);
            if (!gl) {
                trace("no native GLES 1.1 context (%s) - falling back to a "
                      "desktop compatibility profile, which has the same fixed "
                      "function", SDL_GetError());

                /* The window was created for an ES pixel format; a profile
                 * change needs a fresh one or SDL keeps the old config. */
                SDL_DestroyWindow(window);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                                    SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);

                window = SDL_CreateWindow("Dead Space",
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          kWidth, kHeight, SDL_WINDOW_OPENGL);
                if (!window)
                    trace("SDL_CreateWindow (compat) failed: %s", SDL_GetError());
                else if (!(gl = SDL_GL_CreateContext(window)))
                    trace("no fixed-function context at all: %s", SDL_GetError());
            }
        }
    }

    if (gl) {
        load_gles1_funcs();

        const GLubyte *(*get_string)(GLenum) =
            (const GLubyte *(*)(GLenum))SDL_GL_GetProcAddress("glGetString");
        if (get_string) {
            const char *ver = (const char *)get_string(0x1F02 /* GL_VERSION  */);
            const char *rnd = (const char *)get_string(0x1F01 /* GL_RENDERER */);
            trace("GL_VERSION=%s | GL_RENDERER=%s", ver ? ver : "?", rnd ? rnd : "?");
        }
    }

    /*
     * The fake JavaVM has to exist before the module is linked: the engine's
     * static initialisers run during so_load_module() and some of them reach
     * for the VM.
     */
    JavaVM *vm  = NULL;
    JNIEnv *env = NULL;
    if (JNI_CreateJavaVM(&vm, &env, NULL) != JNI_OK || !vm || !env) {
        fatal("could not create the JNI environment.");
        return 1;
    }

    /*
     * Map, relocate and link the module.
     *
     * so_load_module() takes a bare soname and searches "<alt>/<abi>/<name>"
     * and "lib/<abi>/<name>", with <abi> derived from the host - armeabi-v7a
     * here. This game ships under lib/armeabi, so neither pattern matches and
     * the alternative search path is set to the ABI directory itself, which
     * makes the loader's own second attempt ("lib/armeabi-v7a/...") the one
     * that misses and the direct one the one that hits.
     *
     * The zip argument is NULL and stays NULL: every DT_NEEDED this module
     * declares is answered by this loader (see so_builtin_libs in symtab.cpp),
     * so nothing is ever looked for inside an archive.
     *
     * The VM argument is NULL on purpose, and it is not an oversight:
     * so_load_module() calls JNI_OnLoad itself when a module exports one, and
     * for this game that would run it in the wrong place. The working boot
     * sequence puts JNI_OnLoad first on the game thread, ahead of
     * NativeOnCreate - passing NULL here leaves that call to us, below.
     */
    so_set_options(NULL, lib_dir);

    so_module *mod = so_load_module(kNativeLib, NULL, NULL);
    if (!mod) {
        /* so_after_relocate() already said which imports were missing, if
         * that is what stopped it; anything else is a mapping failure. */
        fatal("could not load '%s' from '%s'.", kNativeLib, lib_dir);
        fflush(NULL);
        _exit(1);
    }

    /* From here on the game's own code runs, and a fault inside it would
     * otherwise be reported as a bare address with no context. */
    crash_report_init(mod, kNativeLib);

    /* ---------------------------------------------------------------- *
     * The boot sequence, in the order the Vita port uses.
     *
     * It runs on this thread rather than on a dedicated one. The Vita port
     * spawns a pthread purely to get a 1 MB stack, which is larger than its
     * default; on Linux the main thread already has 8 MB, so the thread would
     * buy nothing and only add a place for a crash to lose its backtrace.
     * ---------------------------------------------------------------- */
    auto JNI_OnLoad =
        (int (*)(JavaVM *, void *))so_symbol(mod, "JNI_OnLoad");
    auto NativeOnCreate =
        (void (*)(void))so_symbol(mod, "Java_com_ea_blast_MainActivity_NativeOnCreate");
    auto NativeOnSurfaceCreated =
        (void (*)(void))so_symbol(mod, "Java_com_ea_blast_AndroidRenderer_NativeOnSurfaceCreated");
    auto NativeOnVisibilityChanged =
        (void (*)(JNIEnv *, void *, int, int))so_symbol(mod, "Java_com_ea_blast_KeyboardAndroid_NativeOnVisibilityChanged");
    auto NativeOnDrawFrame =
        (void (*)(void))so_symbol(mod, "Java_com_ea_blast_AndroidRenderer_NativeOnDrawFrame");

    if (!JNI_OnLoad || !NativeOnCreate || !NativeOnSurfaceCreated || !NativeOnDrawFrame) {
        fatal("%s is missing one of the entry points the port drives:\n"
              "       JNI_OnLoad=%p NativeOnCreate=%p NativeOnSurfaceCreated=%p\n"
              "       NativeOnDrawFrame=%p",
              kNativeLib, (void *)JNI_OnLoad, (void *)NativeOnCreate,
              (void *)NativeOnSurfaceCreated, (void *)NativeOnDrawFrame);
        fflush(NULL);
        _exit(1);
    }

    JNI_OnLoad(vm, NULL);
    trace("JNI_OnLoad returned");

    /* EAAudioCore__Startup() belongs here, between JNI_OnLoad and
     * NativeOnCreate. It is not written yet: the engine drives audio through
     * JNI against a Java AudioTrack, not OpenSL ES, so the inherited
     * android/opensles.cpp answers none of it. Left out rather than stubbed
     * wrong, and noted so the gap is visible in the sequence. */

    NativeOnCreate();
    trace("NativeOnCreate returned");

    NativeOnSurfaceCreated();
    trace("surface created");

    /* 0x42424242 is the Vita port's placeholder jobject: the engine stores the
     * pointer and never dereferences it, and a recognisable value makes it
     * obvious in a fault address if that assumption is ever wrong. */
    if (NativeOnVisibilityChanged)
        NativeOnVisibilityChanged(env, (void *)0x42424242, 600, 1);

    /*
     * A bounded run. DEADSPACE_FRAME_LIMIT stops the process after that many
     * frames so an automated run terminates on a fact rather than on a
     * stopwatch; unset (the normal case for a player) means run forever.
     */
    const char *limit_env   = getenv("DEADSPACE_FRAME_LIMIT");
    const long  frame_limit = limit_env ? atol(limit_env) : 0;

    long frames = 0;
    while (frame_limit == 0 || frames < frame_limit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                goto done;
        }

        NativeOnDrawFrame();
        frames++;

        if (window)
            SDL_GL_SwapWindow(window);

        /* One line per frame would drown the log; the harness reads the last
         * one, so the cadence only has to be fine enough to be current. */
        if (frames % 10 == 0)
            trace("frames=%ld", frames);
    }

done:
    trace("frames=%ld", frames);
    trace("run finished: %ld frames", frames);

    /*
     * The engine started threads of its own and they are still running.
     * Tearing the window and the GL context down from here would pull them out
     * from under those threads and produce a segfault that has nothing to do
     * with why the loader stopped. Leave it to the kernel.
     */
    fflush(NULL);
    _exit(0);
}
