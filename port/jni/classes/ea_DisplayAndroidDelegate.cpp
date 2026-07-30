#include <stdlib.h>

#include "platform.h"
#include "jni.h"
#include "jni_internals.h"
#include "trace.h"
#include "ea_DisplayAndroidDelegate.h"

/*
 * The panel the engine is told about.
 *
 * These are the R36S defaults and they match the surface the loader actually
 * creates, which is the honest answer: whatever the engine lays out, it lays
 * out onto that surface.
 *
 * The Vita port answers differently and it is worth recording why the
 * divergence is deliberate rather than an oversight. Its screen is 960x544 and
 * it reports 544x960 - the portrait "natural" orientation an Android phone
 * would report, with the engine expected to rotate. That build works, so the
 * engine clearly tolerates it, but it is a second unknown stacked on top of
 * the first and there is no reason to inherit it: our surface is landscape and
 * we say so.
 *
 * If it turns out wrong on the console the symptom is layout, not a crash - a
 * stretched or rotated HUD - and the env vars exist so that costs one reboot
 * instead of one rebuild and another card trip. That is the Ice Rage lesson:
 * every trip to the device should carry as many answerable questions as
 * possible.
 */
static const int kDefaultWidth  = 640;
static const int kDefaultHeight = 480;

/*
 * 229 dpi: the panel is 3.5" diagonal at 640x480, so sqrt(640^2 + 480^2) / 3.5.
 * The engine scales UI metrics by this, so a made-up round number would make
 * text and touch targets the wrong physical size. (The Vita port reports 200,
 * which is likewise an approximation of *its* real 220.)
 */
static const float kDefaultDpi = 229.0f;

static int env_int(const char *name, int fallback)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return fallback;
    int n = atoi(v);
    return n > 0 ? n : fallback;
}

void DisplayAndroidDelegate::ctor(JNIEnv *env, jobject obj, jclass clazz)
{
    (void)env; (void)obj; (void)clazz;
    trace("DisplayAndroidDelegate constructed (%dx%d @ %.0f dpi)",
          env_int("DEADSPACE_SCREEN_W", kDefaultWidth),
          env_int("DEADSPACE_SCREEN_H", kDefaultHeight),
          (double)kDefaultDpi);
}

jint DisplayAndroidDelegate::GetDefaultWidth(JNIEnv *env, jobject obj)
{
    (void)env; (void)obj;
    return env_int("DEADSPACE_SCREEN_W", kDefaultWidth);
}

jint DisplayAndroidDelegate::GetDefaultHeight(JNIEnv *env, jobject obj)
{
    (void)env; (void)obj;
    return env_int("DEADSPACE_SCREEN_H", kDefaultHeight);
}

jfloat DisplayAndroidDelegate::GetDpiX(JNIEnv *env, jobject obj)
{
    (void)env; (void)obj;
    return (jfloat)env_int("DEADSPACE_SCREEN_DPI", (int)kDefaultDpi);
}

jfloat DisplayAndroidDelegate::GetDpiY(JNIEnv *env, jobject obj)
{
    (void)env; (void)obj;
    return (jfloat)env_int("DEADSPACE_SCREEN_DPI", (int)kDefaultDpi);
}

/*
 * Orientation 0 - the display is in its default orientation and stays there.
 *
 * This is a handheld with a fixed landscape panel and no rotation sensor worth
 * the name, so there is no honest second value to return. The engine asks once
 * at startup and then trusts it.
 */
jint DisplayAndroidDelegate::GetStdOrientation(JNIEnv *env, jobject obj)
{
    (void)env; (void)obj;
    return 0;
}

/*
 * Ignored on purpose. The engine sets orientation when it thinks the device
 * rotated; nothing here can rotate, and pretending to accept it is the same
 * answer as accepting it - the next GetStdOrientation still says 0.
 */
void DisplayAndroidDelegate::SetStdOrientation(JNIEnv *env, jobject obj, jint orientation)
{
    (void)env; (void)obj;
    trace("DisplayAndroidDelegate.SetStdOrientation(%d) ignored - "
          "this panel does not rotate", (int)orientation);
}

const ManagedMethod DisplayAndroidDelegateMethods[] = {
    ManagedMethod::RegisterNonVirtual<&DisplayAndroidDelegate::ctor>(
        DisplayAndroidDelegate::clazz, "<init>", "()V"),
    ManagedMethod::Register<&DisplayAndroidDelegate::GetDefaultWidth>(
        DisplayAndroidDelegate::clazz, "GetDefaultWidth", "()I"),
    ManagedMethod::Register<&DisplayAndroidDelegate::GetDefaultHeight>(
        DisplayAndroidDelegate::clazz, "GetDefaultHeight", "()I"),
    ManagedMethod::Register<&DisplayAndroidDelegate::GetDpiX>(
        DisplayAndroidDelegate::clazz, "GetDpiX", "()F"),
    ManagedMethod::Register<&DisplayAndroidDelegate::GetDpiY>(
        DisplayAndroidDelegate::clazz, "GetDpiY", "()F"),
    ManagedMethod::Register<&DisplayAndroidDelegate::GetStdOrientation>(
        DisplayAndroidDelegate::clazz, "GetStdOrientation", "()I"),
    ManagedMethod::Register<&DisplayAndroidDelegate::SetStdOrientation>(
        DisplayAndroidDelegate::clazz, "SetStdOrientation", "(I)V"),
    {NULL},
};

Class DisplayAndroidDelegate::clazz = {
    .classpath        = "com/ea/blast/DisplayAndroidDelegate",
    .classname        = "DisplayAndroidDelegate",
    .managed_methods  = DisplayAndroidDelegateMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = sizeof(DisplayAndroidDelegate),
};

static const int registered =
    ClassRegistry::register_class(DisplayAndroidDelegate::clazz);
