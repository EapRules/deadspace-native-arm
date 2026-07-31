#include "platform.h"
#include "jni.h"
#include "jni_internals.h"
#include "trace.h"
#include "ea_sensors.h"

/* ------------------------------------------------------------------ *
 * PowerManagerAndroid
 * ------------------------------------------------------------------ */

void PowerManagerAndroid::ctor(JNIEnv *env, jobject obj, jclass clazz)
{
    (void)env; (void)obj; (void)clazz;
    trace("PowerManagerAndroid constructed");
}

/*
 * Ignored, and that is the correct behaviour rather than a shortcut.
 *
 * On Android this takes a WakeLock so the screen does not dim mid-cutscene.
 * A PortMaster handheld running a game in the foreground has no idle timeout
 * to suppress - the launcher owns the session and nothing is going to blank
 * the panel underneath us. Taking the call and doing nothing is the honest
 * implementation of "the screen will stay on".
 */
void PowerManagerAndroid::ApplyKeepAwake(JNIEnv *env, jobject obj, jboolean keep_awake)
{
    (void)env; (void)obj;
    trace("PowerManagerAndroid.ApplyKeepAwake(%s) - nothing to do, the panel "
          "does not sleep under the launcher", keep_awake ? "true" : "false");
}

const ManagedMethod PowerManagerAndroidMethods[] = {
    ManagedMethod::RegisterNonVirtual<&PowerManagerAndroid::ctor>(
        PowerManagerAndroid::clazz, "<init>", "()V"),
    ManagedMethod::Register<&PowerManagerAndroid::ApplyKeepAwake>(
        PowerManagerAndroid::clazz, "ApplyKeepAwake", "(Z)V"),
    {NULL},
};

Class PowerManagerAndroid::clazz = {
    .classpath        = "com/ea/blast/PowerManagerAndroid",
    .classname        = "PowerManagerAndroid",
    .managed_methods  = PowerManagerAndroidMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = sizeof(PowerManagerAndroid),
};

/* ------------------------------------------------------------------ *
 * DeviceOrientationHandlerAndroidDelegate
 * ------------------------------------------------------------------ */

void DeviceOrientationHandlerAndroidDelegate::ctor(JNIEnv *env, jobject obj, jclass clazz)
{
    (void)env; (void)obj; (void)clazz;
    trace("DeviceOrientationHandlerAndroidDelegate constructed "
          "(fixed landscape, no rotation sensor)");
}

/*
 * The engine calls this when the activity regains focus, to re-read the
 * orientation it may have missed while backgrounded. There is no backgrounding
 * here and no orientation to have missed.
 */
void DeviceOrientationHandlerAndroidDelegate::OnLifeCycleFocusGained(JNIEnv *env, jobject obj)
{
    (void)env; (void)obj;
}

void DeviceOrientationHandlerAndroidDelegate::SetEnabled(JNIEnv *env, jobject obj, jboolean enabled)
{
    (void)env; (void)obj;
    trace("DeviceOrientationHandler.SetEnabled(%s) - no rotation sensor on "
          "this device", enabled ? "true" : "false");
}

const ManagedMethod DeviceOrientationHandlerMethods[] = {
    ManagedMethod::RegisterNonVirtual<&DeviceOrientationHandlerAndroidDelegate::ctor>(
        DeviceOrientationHandlerAndroidDelegate::clazz, "<init>", "()V"),
    ManagedMethod::Register<&DeviceOrientationHandlerAndroidDelegate::OnLifeCycleFocusGained>(
        DeviceOrientationHandlerAndroidDelegate::clazz, "OnLifeCycleFocusGained", "()V"),
    ManagedMethod::Register<&DeviceOrientationHandlerAndroidDelegate::SetEnabled>(
        DeviceOrientationHandlerAndroidDelegate::clazz, "SetEnabled", "(Z)V"),
    {NULL},
};

Class DeviceOrientationHandlerAndroidDelegate::clazz = {
    .classpath        = "com/ea/blast/DeviceOrientationHandlerAndroidDelegate",
    .classname        = "DeviceOrientationHandlerAndroidDelegate",
    .managed_methods  = DeviceOrientationHandlerMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = sizeof(DeviceOrientationHandlerAndroidDelegate),
};

/* ------------------------------------------------------------------ *
 * AccelerometerAndroidDelegate
 * ------------------------------------------------------------------ */

void AccelerometerAndroidDelegate::ctor(JNIEnv *env, jobject obj, jclass clazz)
{
    (void)env; (void)obj; (void)clazz;
    trace("AccelerometerAndroidDelegate constructed (no physical sensor; "
          "L2/R2 provide synthetic gestures)");
}

/*
 * Accepted without starting a physical sensor. android/input_bridge.cpp calls
 * the game's NativeOnAcceleration export directly when L2 or R2 reproduces a
 * measured gesture. This Java-side switch therefore has no device resource to
 * acquire, but the class and method must still exist for renderer startup.
 */
void AccelerometerAndroidDelegate::SetEnabled(JNIEnv *env, jobject obj, jboolean enabled)
{
    (void)env; (void)obj;
    trace("Accelerometer.SetEnabled(%s) - synthetic L2/R2 source needs no "
          "physical listener",
          enabled ? "true" : "false");
}

void AccelerometerAndroidDelegate::SetUpdateFrequency(JNIEnv *env, jobject obj, jint hz)
{
    (void)env; (void)obj;
    (void)hz;
}

const ManagedMethod AccelerometerMethods[] = {
    ManagedMethod::RegisterNonVirtual<&AccelerometerAndroidDelegate::ctor>(
        AccelerometerAndroidDelegate::clazz, "<init>", "()V"),
    ManagedMethod::Register<&AccelerometerAndroidDelegate::SetEnabled>(
        AccelerometerAndroidDelegate::clazz, "SetEnabled", "(Z)V"),
    ManagedMethod::Register<&AccelerometerAndroidDelegate::SetUpdateFrequency>(
        AccelerometerAndroidDelegate::clazz, "SetUpdateFrequency", "(I)V"),
    {NULL},
};

Class AccelerometerAndroidDelegate::clazz = {
    .classpath        = "com/ea/blast/AccelerometerAndroidDelegate",
    .classname        = "AccelerometerAndroidDelegate",
    .managed_methods  = AccelerometerMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = sizeof(AccelerometerAndroidDelegate),
};

/* ------------------------------------------------------------------ *
 * TouchSurfaceAndroid
 * ------------------------------------------------------------------ */

/*
 * False, deliberately.
 *
 * There is no touchscreen at all on this device - input arrives as a gamepad,
 * translated in android/platform.cpp. Claiming multi-touch would put the
 * engine on its pinch/two-finger code paths, waiting for a second pointer that
 * can never arrive.
 *
 * This is the same class of decision that cost Ice Rage three card trips: that
 * engine rejected synthetic touch events outright and the port had to feed it
 * joystick events instead. Answering "no touch" up front is the version of
 * that lesson applied before the fact rather than after.
 */
jboolean TouchSurfaceAndroid::IsTouchScreenMultiTouch(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    trace("TouchSurfaceAndroid.IsTouchScreenMultiTouch -> false "
          "(no touchscreen; input comes from the pad)");
    return JNI_FALSE;
}

const ManagedMethod TouchSurfaceAndroidMethods[] = {
    ManagedMethod::RegisterStatic<&TouchSurfaceAndroid::IsTouchScreenMultiTouch>(
        TouchSurfaceAndroid::clazz, "IsTouchScreenMultiTouch", "()Z"),
    {NULL},
};

Class TouchSurfaceAndroid::clazz = {
    .classpath        = "com/ea/blast/TouchSurfaceAndroid",
    .classname        = "TouchSurfaceAndroid",
    .managed_methods  = TouchSurfaceAndroidMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = sizeof(TouchSurfaceAndroid),
};

static const int registered_power  = ClassRegistry::register_class(PowerManagerAndroid::clazz);
static const int registered_orient = ClassRegistry::register_class(DeviceOrientationHandlerAndroidDelegate::clazz);
static const int registered_accel  = ClassRegistry::register_class(AccelerometerAndroidDelegate::clazz);
static const int registered_touch  = ClassRegistry::register_class(TouchSurfaceAndroid::clazz);
