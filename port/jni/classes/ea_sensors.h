#pragma once
#include "jni.h"
#include "jni_internals.h"

/*
 * The three small com/ea/blast delegates the engine builds while standing up
 * its input and power stacks, in one file because they are one decision.
 *
 * There is no physical accelerometer, no rotation sensor and no screen-timeout
 * to suppress. These delegates still need to be *present*: the engine
 * constructs each one during renderer bring-up, and a missing class means a
 * NULL jmethodID that it walks straight into. Synthetic L2/R2 acceleration is
 * injected directly through the native export by android/input_bridge.cpp.
 *
 * Every signature was transcribed from the descriptor the engine passed to
 * GetMethodID, not guessed. Lookup is an exact strcmp on name and signature
 * together, so a wrong descriptor fails exactly like a missing class - and
 * leaves the same, uninformative log line. Note SetUpdateFrequency takes an
 * int, not the float the Vita port's comment implies.
 */
class PowerManagerAndroid : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
    static void ctor(JNIEnv *env, jobject obj, jclass clazz);
    static void ApplyKeepAwake(JNIEnv *env, jobject obj, jboolean keep_awake);
};

class DeviceOrientationHandlerAndroidDelegate : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
    static void ctor(JNIEnv *env, jobject obj, jclass clazz);
    static void OnLifeCycleFocusGained(JNIEnv *env, jobject obj);
    static void SetEnabled(JNIEnv *env, jobject obj, jboolean enabled);
};

class AccelerometerAndroidDelegate : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
    static void ctor(JNIEnv *env, jobject obj, jclass clazz);
    static void SetEnabled(JNIEnv *env, jobject obj, jboolean enabled);
    static void SetUpdateFrequency(JNIEnv *env, jobject obj, jint hz);
};

/*
 * Static-only: the engine never constructs one, it just asks the class whether
 * the screen does multi-touch.
 */
class TouchSurfaceAndroid : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
    static jboolean IsTouchScreenMultiTouch(JNIEnv *env, jclass clazz);
};
