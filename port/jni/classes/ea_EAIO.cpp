#include <stdio.h>

#include "platform.h"
#include "so_util.h"
#include "jni.h"
#include "jni_internals.h"
#include "trace.h"
#include "ea_EAIO.h"

extern so_module *deadspace_module(void);

/*
 * EAIO.Startup - forwarded to the game's own native implementation.
 *
 * This is the first thing NativeOnDrawFrame does, and until this class existed
 * it was also where the port died. The sequence, read out of the disassembly
 * at 0x2367dc rather than guessed:
 *
 *     ldr pc, [r3, #24]       -> JNIEnv slot 6   = FindClass("com/ea/EAIO/EAIO")
 *     ldr pc, [ip, #0x1c4]    -> JNIEnv slot 113 = GetStaticMethodID("Startup",
 *                                "(Landroid/content/res/AssetManager;)V")
 *     ...                        then it calls what it got back
 *
 * FindClass returns NULL for a class nobody registered, GetStaticMethodID then
 * returns NULL *without warning* because its own diagnostic is guarded on a
 * non-NULL class, and the engine calls straight through it. The result is a
 * SIGSEGV with pc = 0 and a link register pointing into this loader rather than
 * into the game, which reads like a bug in our code and is not one - it is a
 * class that was never there.
 *
 * The forward is the whole implementation, and deliberately so. On Android this
 * method is declared native: there is no Java behind it to emulate, only a
 * different way of reaching code that is already in the .so we mapped. Writing
 * a plausible-looking stub here instead would leave the engine's I/O layer
 * uninitialised and fail later, somewhere with no evidence attached.
 *
 * The AssetManager argument is passed through untouched. This game never calls
 * AAssetManager_open - it does not import libandroid at all - so what arrives
 * is whatever the engine handed us, and it only has to survive the round trip.
 */
static void EAIO_Startup(JNIEnv *env, jclass clazz, jobject asset_manager)
{
    (void)clazz;

    static void (*native_startup)(JNIEnv *, void *, jobject) = NULL;

    if (!native_startup) {
        so_module *mod = deadspace_module();
        if (!mod) {
            warning("EAIO.Startup called before the module was loaded.\n");
            return;
        }
        native_startup = (void (*)(JNIEnv *, void *, jobject))
            so_symbol(mod, "Java_com_ea_EAIO_EAIO_Startup");
        if (!native_startup) {
            warning("EAIO.Startup: the game exports no "
                    "Java_com_ea_EAIO_EAIO_Startup to forward to.\n");
            return;
        }
    }

    trace("EAIO.Startup -> native (assetManager=%p)", (void *)asset_manager);

    /* Second argument is the jobject 'this' on Android; the method is static,
     * so the game ignores it. The Vita port passes NULL here for the same
     * reason and that build works. */
    native_startup(env, NULL, asset_manager);
}

const ManagedMethod EAIOClassMethods[] = {
    ManagedMethod::RegisterStatic<&EAIO_Startup>(
        EAIO::clazz, "Startup", "(Landroid/content/res/AssetManager;)V"),
    {NULL},
};

Class EAIO::clazz = {
    .classpath        = "com/ea/EAIO/EAIO",
    .classname        = "EAIO",
    .managed_methods  = EAIOClassMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = 0,
};

static const int registered = ClassRegistry::register_class(EAIO::clazz);
