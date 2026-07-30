/*
 * The import tables the bionic ELF loader binds into libdeadspace.so.
 *
 * The game's dynamic table lists nine shared objects: libandroid, libEGL,
 * libGLESv2, libOpenSLES, liblog, libm, libstdc++, libdl and libc. None of
 * them exist on the R36S in a form an Android .so can link against, so each
 * one is answered here by a table of host functions.
 *
 * so_resolve_link() walks these in order and takes the first match, which is
 * why the port's own tables come before the generic bionic libc one.
 *
 * A symbol that is in none of them stays unresolved. The loader points it at a
 * PLT stub that aborts on first call, and src/main.cpp reports it by name
 * before anything runs - see report_unresolved_symbols().
 */
#include "so_util.h"
#include "thunk_gen.h"

#include "android_api.h"

extern DynLibFunction symtable_libc[];      /* thunks/libc/libc_table.cpp   */
extern DynLibFunction symtable_gles2[];     /* thunks/khronos/gles2.cpp     */
extern DynLibFunction symtable_gles1[];     /* thunks/khronos/gles1.cpp     */
extern DynLibFunction symtable_egl[];       /* android/egl_shim.cpp         */
extern DynLibFunction symtable_log[];       /* android/log.cpp              */
extern DynLibFunction symtable_opensles[];  /* android/opensles.cpp         */
extern DynLibFunction symtable_libm[];      /* src/symtab_libm.cpp          */
extern DynLibFunction symtable_time[];      /* src/symtab_time.cpp          */
extern DynLibFunction symtable_pthread[];   /* src/symtab_pthread.cpp       */
extern DynLibFunction symtable_stat[];      /* src/symtab_stat.cpp          */
extern DynLibFunction symtable_glprobe[];   /* src/symtab_glprobe.cpp       */
extern DynLibFunction symtable_unwind[];    /* src/symtab_unwind.cpp        */
extern DynLibFunction symtable_bionic[];    /* src/symtab_bionic.cpp        */
extern DynLibFunction symtable_off[];       /* src/symtab_off.cpp           */

/*
 * libandroid.so — the 42 symbols the game actually imports, implemented in
 * android/asset_manager.cpp and android/platform.cpp.
 *
 * THUNK_DIRECT is not decoration: the game is armeabi-v7a, which passes floats
 * in core registers, while this loader is built hardfp. The macro inspects
 * each prototype at compile time and inserts an ABI bridge only where a float
 * actually crosses the boundary (AMotionEvent_getX and friends), leaving the
 * rest as direct pointers.
 */
DynLibFunction symtable_android[] = {
    /* asset_manager.h */
    THUNK_DIRECT(AAssetManager_open),
    THUNK_DIRECT(AAssetManager_openDir),
    THUNK_DIRECT(AAssetDir_getNextFileName),
    THUNK_DIRECT(AAssetDir_close),
    THUNK_DIRECT(AAsset_getBuffer),
    THUNK_DIRECT(AAsset_getLength),
    THUNK_DIRECT(AAsset_openFileDescriptor),
    THUNK_DIRECT(AAsset_close),

    /* configuration.h */
    THUNK_DIRECT(AConfiguration_new),
    THUNK_DIRECT(AConfiguration_delete),
    THUNK_DIRECT(AConfiguration_fromAssetManager),
    THUNK_DIRECT(AConfiguration_getLanguage),
    THUNK_DIRECT(AConfiguration_getCountry),

    /* native_window.h */
    THUNK_DIRECT(ANativeWindow_getWidth),
    THUNK_DIRECT(ANativeWindow_getHeight),
    THUNK_DIRECT(ANativeWindow_setBuffersGeometry),

    /* looper.h */
    THUNK_DIRECT(ALooper_prepare),
    THUNK_DIRECT(ALooper_pollAll),
    THUNK_DIRECT(ALooper_addFd),

    /* input.h */
    THUNK_DIRECT(AInputEvent_getType),
    THUNK_DIRECT(AInputEvent_getDeviceId),
    THUNK_DIRECT(AInputEvent_getSource),
    THUNK_DIRECT(AKeyEvent_getAction),
    THUNK_DIRECT(AKeyEvent_getFlags),
    THUNK_DIRECT(AKeyEvent_getKeyCode),
    THUNK_DIRECT(AKeyEvent_getMetaState),
    THUNK_DIRECT(AMotionEvent_getAction),
    THUNK_DIRECT(AMotionEvent_getFlags),
    THUNK_DIRECT(AMotionEvent_getMetaState),
    THUNK_DIRECT(AMotionEvent_getPointerCount),
    THUNK_DIRECT(AMotionEvent_getPointerId),
    THUNK_DIRECT(AMotionEvent_getX),
    THUNK_DIRECT(AMotionEvent_getY),
    THUNK_DIRECT(AMotionEvent_getHistorySize),
    THUNK_DIRECT(AMotionEvent_getHistoricalX),
    THUNK_DIRECT(AMotionEvent_getHistoricalY),
    /* Neither of these is in the game's PLT - it looks them up with dlsym,
     * because they postdate the API level this 2013 build targets. They still
     * have to be here: our dlsym answers out of these tables, and a miss
     * returns NULL into a function pointer the game calls without checking.
     * See AMotionEvent_getAxisValue() in android/platform.cpp. */
    THUNK_DIRECT(AMotionEvent_getAxisValue),
    THUNK_DIRECT(AMotionEvent_getHistoricalAxisValue),
    THUNK_DIRECT(AInputQueue_getEvent),
    THUNK_DIRECT(AInputQueue_preDispatchEvent),
    THUNK_DIRECT(AInputQueue_finishEvent),
    THUNK_DIRECT(AInputQueue_attachLooper),
    THUNK_DIRECT(AInputQueue_detachLooper),

    /* native_activity.h */
    THUNK_DIRECT(ANativeActivity_finish),

    { NULL, 0 },
};

/*
 * Every DT_NEEDED entry the loader answers itself instead of mapping.
 *
 * libEAMGameDeadSpace.so declares exactly five - libc, libstdc++, libm,
 * liblog, libGLESv1_CM - and notably not libandroid: this game is not a
 * NativeActivity. The rest of the list is kept because it costs nothing and
 * the entries are inert for a module that never names them.
 *
 * libstdc++.so is listed but unused: the game statically links its own C++
 * runtime, and its dynamic table contains no mangled C++ import at all.
 *
 * libGLESv1_CM.so is the one that had to be added. so_load_module() walks
 * DT_NEEDED and treats anything not in this list as another module to map, so
 * without the entry it goes looking for lib/armeabi-v7a/libGLESv1_CM.so inside
 * the game tree - which is of course not there, it comes from the system image
 * on a device - and the load fails before a single relocation is applied.
 */
const char *so_builtin_libs[] = {
    "libc.so",
    "libdl.so",
    "libm.so",
    "libstdc++.so",
    "liblog.so",
    "libandroid.so",
    "libz.so",
    "libEGL.so",
    "libGLESv2.so",
    "libGLESv1_CM.so",
    "libOpenSLES.so",
    NULL,
};

/* No binary patching is needed so far; the game's code runs as shipped. */
DynLibFunction *so_static_patches[] = {
    NULL,
};

DynLibFunction *so_dynamic_libraries[] = {
    /* Override the generated libc table where the host disagrees with bionic
     * about the size or layout of a type; must therefore come first. */
    symtable_time,
    symtable_pthread,
    symtable_stat,
    /* off_t is 8 bytes on this host and 4 in the game; mmap/lseek/ftruncate
     * therefore disagree about register layout, not just field width. */
    symtable_off,
    /* The imports the generated bionic table left commented out: bionic-only
     * atomics, three OBJECT symbols, variadic fcntl, and readdir_r with its
     * own dirent layout. Ahead of symtable_libc because readdir_r's layout
     * disagreement is exactly the kind the generated entry gets wrong. */
    symtable_bionic,
    /* Shadows the host libgcc's __gnu_Unwind_Find_exidx, which cannot see a
     * module this loader mapped; must therefore come before symtable_libc. */
    symtable_unwind,
    symtable_android,
    symtable_log,
    symtable_egl,
    /* Shadows two entries of symtable_gles2 to read back the compile/link
     * status the game never prints; must therefore come before it. */
    symtable_glprobe,
    /* Fixed function, and the only GL table this game needs: it imports 190
     * GLES 1.1 entry points and not one shader call. symtable_gles2 stays in
     * the list below it because it costs nothing and the two do not overlap -
     * no name is in both - but every gl* the game asks for is answered here. */
    symtable_gles1,
    symtable_gles2,
    symtable_opensles,
    symtable_libm,
    symtable_libc,
    NULL,
};
