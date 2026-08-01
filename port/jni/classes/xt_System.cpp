#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <vector>

#include <SDL2/SDL.h>

#include "platform.h"
#include "jni.h"
#include "jni_internals.h"
#include "xt_System.h"

/*
 * The nine static methods xt::java::initJNI() caches on this class, with the
 * signatures it asks for. A signature that does not match exactly yields a
 * NULL method id, and the engine calls through it anyway - so the ones below
 * are transcribed from the binary, not from a Java source nobody has.
 */

/* ------------------------------------------------------------------ *
 * Device description
 * ------------------------------------------------------------------ */

/*
 * xt::java::getDeviceInfo() (0x985e0) takes this string, replaces every '\n'
 * with ':', splits on ':' and then copies nine of the resulting tokens into
 * its DeviceInfo *without checking how many there are*. Too few tokens is an
 * out-of-bounds read of the array followed by strlen() on whatever the length
 * word happened to be - which is exactly how the first version of this file
 * crashed the game.
 *
 * The tokens it takes are 1, 3, 5, 7, 9, 11, 13, 15 and 17 - the array is
 * addressed at offsets 32, 96, 160 ... 544 with an xt::String stride of 32, so
 * it reads every *second* one. That is the shape of "Label: value" lines: with
 * the newlines turned into colons, the even tokens are the labels and the odd
 * tokens are the values, and the engine keeps the values. So this must emit
 * label/value pairs, at least nine of them, and no value may contain a ':' or
 * a newline of its own or every field after it shifts by one.
 *
 * The only consumer is getDeviceId(), which matches the values against the
 * handsets the engine knows and otherwise logs "Unknown device! Assuming
 * generic Android." - which is the correct answer for an R36S, so the point of
 * this is to describe the real machine rather than to impersonate a phone.
 */
static void sanitise(char *s)
{
    for (; *s; s++) {
        if (*s == ':' || *s == '\n' || *s == '\r')
            *s = ' ';
    }
}

static const char *read_first_line(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    size_t n = fread(buf, 1, len - 1, f);
    fclose(f);
    if (n == 0)
        return NULL;

    buf[n] = '\0';
    char *nl = strpbrk(buf, "\r\n");
    if (nl)
        *nl = '\0';

    return buf[0] ? buf : NULL;
}

static jstring xtSystem_getDeviceInfo(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;

    /*
     * Cached: the engine's DeleteLocalRef is a no-op in this port, so every
     * call would otherwise leak the String and its buffer.
     */
    static String *cached = NULL;
    if (cached)
        return (jstring)cached;

    const char *override_info = getenv("DEADSPACE_DEVICE_INFO");
    if (override_info) {
        cached = new String(override_info);
        fprintf(stderr, "xtSystem.getDeviceInfo (override): %s\n", override_info);
        return (jstring)cached;
    }

    /*
     * Everything below is static, not automatic, and that is load-bearing: the
     * engine calls this from its own thread, and a kilobyte of stack buffers
     * here was enough to corrupt that thread's frame - the game then crashed
     * inside its own parse of the perfectly good string this returned. The
     * function is a one-shot (the result is cached) and only ever runs on that
     * one thread, so statics are safe and free.
     */
    static struct utsname un;
    if (uname(&un) != 0)
        memset(&un, 0, sizeof(un));

    static char model_buf[128];
    const char *model = read_first_line("/proc/device-tree/model", model_buf, sizeof(model_buf));
    if (!model)
        model = read_first_line("/sys/devices/virtual/dmi/id/product_name", model_buf, sizeof(model_buf));

    static char vendor_buf[128];
    const char *vendor = read_first_line("/sys/devices/virtual/dmi/id/sys_vendor", vendor_buf, sizeof(vendor_buf));

    const char *machine = un.machine[0] ? un.machine : "unknown";

    /*
     * Android's Build fields, in Build's own order, answered for this port -
     * twelve pairs where nine would do, because a spare pair costs nothing and
     * a missing one is an out-of-bounds read.
     */
    struct { const char *label; const char *value; } fields[12] = {
        { "Manufacturer", vendor ? vendor : "unknown"              },
        { "Model",        model ? model : machine                  },
        { "Device",       un.nodename[0] ? un.nodename : "unknown" },
        { "Product",      "deadspace-portmaster"                   },
        { "Brand",        "portmaster"                             },
        { "Hardware",     machine                                  },
        { "Board",        machine                                  },
        { "OS",           un.sysname[0] ? un.sysname : "Linux"     },
        { "Release",      un.release[0] ? un.release : "unknown"   },
        { "SDK",          "0" /* not Android: there is no API level */ },
        { "ABI",          "armeabi-v7a"                            },
        { "Build",        "deadspace native loader"                },
    };

    static char info[1024];
    size_t used = 0;
    for (int i = 0; i < 12; i++) {
        static char value[64];
        snprintf(value, sizeof(value), "%s",
                 fields[i].value[0] ? fields[i].value : "unknown");
        /* A ':' or a newline inside a value would be read as a separator and
         * would shift every field after it. */
        sanitise(value);

        int n = snprintf(info + used, sizeof(info) - used, "%s%s:%s",
                         i ? "\n" : "", fields[i].label, value);
        if (n < 0 || (size_t)n >= sizeof(info) - used) {
            /* Drop the half-written pair rather than emit a lone token: an
             * odd token count is what shifts every value after it. */
            info[used] = '\0';
            break;
        }
        used += (size_t)n;
    }
    info[sizeof(info) - 1] = '\0';

    /* One line, once: this is the string the engine's device matching is
     * about to be judged on, and it is the only way to see it went out whole. */
    static char oneline[sizeof(info)];
    snprintf(oneline, sizeof(oneline), "%s", info);
    for (char *c = oneline; *c; c++) {
        if (*c == '\n')
            *c = ' ';
    }
    fprintf(stderr, "xtSystem.getDeviceInfo: %s\n", oneline);

    cached = new String(info);
    return (jstring)cached;
}

/* ------------------------------------------------------------------ *
 * Display density
 * ------------------------------------------------------------------ */

/*
 * Declared "()F" in Java, but the engine calls it through
 * _JNIEnv::CallStaticBooleanMethod and then does `vmov s0, r0; vcvt.f32.u32`
 * (xt::java::getDisplayDensityScale, 0x98b00) - it reads the return value out
 * of r0 as an *unsigned integer*, not as float bits. Returning a jfloat here
 * would hand it 0x3f800000 and it would compute 1065353216.0.
 *
 * So this returns an integer on purpose: 1, i.e. a scale of 1.0. The port
 * renders at the panel's native 640x480 with no density scaling, which is what
 * a scale of 1 means. Nothing inside libdeadspace.so calls
 * getDisplayDensityScale (no bl to it anywhere in .text), so this value is
 * currently observed by nobody - which is a reason to keep it honest and
 * cheap, not a reason to return garbage.
 */
static jint xtSystem_getDisplayDensityInfo(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    return 1;
}

/* ------------------------------------------------------------------ *
 * Network
 * ------------------------------------------------------------------ */

/*
 * False, and it is not a placeholder: this port implements no networking at
 * all. Leaderboards, ads and IAP are out of scope, so there is no code that
 * could use a connection even where one exists. Claiming a network is
 * available would send the engine down paths that can only time out.
 */
static jboolean xtSystem_getNetworkAvailability(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    return JNI_FALSE;
}

/* ------------------------------------------------------------------ *
 * Input device naming
 * ------------------------------------------------------------------ */

/*
 * The engine uses the name to recognise particular pads. SDL already knows the
 * real one, so report that and nothing else. A NULL return is a legal answer
 * here - xt::java::getInputDeviceName (0x9812c) tests for it and yields an
 * empty name - and it is the truthful one when no controller is open, which is
 * the case whenever SDL's game-controller subsystem has not been initialised.
 */
static jstring xtSystem_getInputDeviceNameByDeviceId(JNIEnv *env, jclass clazz, jint device_id)
{
    (void)env; (void)clazz;

    /*
     * Never NULL. The engine asks this while setting its controller up and
     * dereferences the answer without checking - returning null force-closed
     * the game the moment a stick moved in controller mode. It is the same
     * rule the audio shim learned: hand back something valid, always.
     *
     * This used to be harmless only because the gamepad subsystem was never
     * initialised, so the code path was dead. Opening the controllers for real
     * woke it up.
     */
    (void)device_id;

    const char *name = NULL;

    /* Deliberately not asking SDL for the real name: "GO-Super Gamepad" is
     * exactly the kind of string the engine cannot classify. */
    if (false && SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
        /* Whichever pad is actually open, rather than a guessed instance id:
         * instance ids are handed out by SDL and 0 need not exist. */
        for (int i = 0; i < SDL_NumJoysticks() && !name; i++) {
            if (!SDL_IsGameController(i))
                continue;
            SDL_GameController *pad = SDL_GameControllerOpen(i);
            if (pad)
                name = SDL_GameControllerName(pad);
            if (!name)
                name = SDL_GameControllerNameForIndex(i);
        }
    }

    /*
     * The name is not informational: the engine classifies the pad by it and
     * picks a button profile to match. Strings it compares against, found in
     * the binary, are "Microsoft", "Sony", "NVIDIA", "NVIDIA SHIELD" and
     * "OUYA" - and there is a createAndroidOUYAConfig() to go with them.
     *
     * Handing it a name outside that set left it with no profile, so the
     * function pointer it later calls stayed null and the game jumped to
     * address zero the moment a stick moved. So report a pad it knows.
     * Microsoft is the closest match to the handheld's own layout: two sticks,
     * a d-pad, four face buttons, two shoulders and two triggers.
     *
     * DEADSPACE_PAD_NAME overrides it, so the other profiles can be tried
     * without a rebuild.
     */
    const char *forced = getenv("DEADSPACE_PAD_NAME");
    if (forced && *forced)
        return (jstring) new String(forced);

    if (!name || !name[0])
        name = "Microsoft X-Box 360 pad";

    return (jstring) new String(name);
}

/* ------------------------------------------------------------------ *
 * Things a handheld cannot show
 * ------------------------------------------------------------------ */

static const char *utf8(jstring s)
{
    String *str = (String *)s;
    return (str && str->str) ? str->str : "";
}

/*
 * No browser, no toast surface, no dialog: the game runs full-screen from a
 * shell script with no window manager. Rather than silently swallow them,
 * these say what the game asked for - it lands in the port's log.txt, which is
 * the only place a message can go here.
 */
static void xtSystem_launchBrowser(JNIEnv *env, jclass clazz, jstring url)
{
    (void)env; (void)clazz;
    warning("xtSystem: no browser on this device; not opening '%s'.\n", utf8(url));
}

static void xtSystem_displayToast(JNIEnv *env, jclass clazz, jstring text)
{
    (void)env; (void)clazz;
    fprintf(stderr, "[toast] %s\n", utf8(text));
}

static void xtSystem_displayDialog(JNIEnv *env, jclass clazz, jstring text)
{
    (void)env; (void)clazz;
    fprintf(stderr, "[dialog] %s\n", utf8(text));
}

/* ------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------ */

/*
 * init() is called by xt::java::initJNI() with ANativeActivity::clazz once the
 * method ids are cached; deinit() on shutdown. On Android they wire up the
 * activity for the helpers above. Here every helper answers from SDL or the
 * kernel and needs nothing from the activity, so there is genuinely nothing to
 * hold on to - and saying so once in the log is worth more than a silent
 * no-op, because it is the line that proves the class was found and called.
 */
static void xtSystem_init(JNIEnv *env, jclass clazz, jobject activity)
{
    (void)env; (void)clazz; (void)activity;
    fprintf(stderr, "xtSystem.init: system integration up (no activity state needed).\n");
}

static void xtSystem_deinit(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
}

const ManagedMethod xtSystemClassMethods[] = {
    ManagedMethod::RegisterStatic<&xtSystem_init>(
        xtSystem::clazz, "init", "(Landroid/app/NativeActivity;)V"),
    ManagedMethod::RegisterStatic<&xtSystem_deinit>(
        xtSystem::clazz, "deinit", "()V"),
    ManagedMethod::RegisterStatic<&xtSystem_getDeviceInfo>(
        xtSystem::clazz, "getDeviceInfo", "()Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&xtSystem_getDisplayDensityInfo>(
        xtSystem::clazz, "getDisplayDensityInfo", "()F"),
    ManagedMethod::RegisterStatic<&xtSystem_getNetworkAvailability>(
        xtSystem::clazz, "getNetworkAvailability", "()Z"),
    ManagedMethod::RegisterStatic<&xtSystem_getInputDeviceNameByDeviceId>(
        xtSystem::clazz, "getInputDeviceNameByDeviceId", "(I)Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&xtSystem_launchBrowser>(
        xtSystem::clazz, "launchBrowser", "(Ljava/lang/String;)V"),
    ManagedMethod::RegisterStatic<&xtSystem_displayToast>(
        xtSystem::clazz, "displayToast", "(Ljava/lang/String;)V"),
    ManagedMethod::RegisterStatic<&xtSystem_displayDialog>(
        xtSystem::clazz, "displayDialog", "(Ljava/lang/String;)V"),
    {NULL},
};

Class xtSystem::clazz = {
    .classpath        = "xtSystem",
    .classname        = "xtSystem",
    .managed_methods  = xtSystemClassMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = 0,
};

static const int registered = ClassRegistry::register_class(xtSystem::clazz);
