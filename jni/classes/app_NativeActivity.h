#pragma once
#include "jni.h"
#include "jni_internals.h"

/*
 * android.app.NativeActivity — the object the platform hands to the game as
 * ANativeActivity::clazz, and the one argument of xtPlay.init().
 *
 * It carries no state of its own: everything the game can reach through it is
 * already reachable through the ANativeActivity struct. It exists because a
 * null `clazz` would make the game's very first GetObjectClass() dereference
 * zero, and because JNI method lookups need a class to hang off.
 */
class AppNativeActivity : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
};

/* The single instance, owned by the loader. */
extern AppNativeActivity g_native_activity_object;
