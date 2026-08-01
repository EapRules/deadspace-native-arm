#pragma once
#include "jni.h"
#include "jni_internals.h"

/*
 * com/ea/blast/GetAppDataDirectoryDelegate — where the engine thinks its data
 * lives.
 *
 * Two methods, both ()Ljava/lang/String;. Everything the engine reads outside
 * the .ini files is addressed relative to what they return, so a NULL answer
 * does not fail loudly - it gets sprintf'd into a path and the port spends the
 * rest of the run opening files under "(nil)/".
 *
 * They are *instance* methods, not static, and that is not a guess: the first
 * version of this class registered them static and the log answered
 *
 *     Class GetAppDataDirectoryDelegate does not have method <init>()V.
 *     Class GetAppDataDirectoryDelegate does not have method
 *           GetAppDataDirectory()Ljava/lang/String;.
 *
 * which is iface_GetMethodID (jni.cpp:325) rejecting a static entry. The
 * engine constructs a delegate and calls through the object, the same shape it
 * uses for SystemAndroidDelegate and PowerManagerAndroid.
 */
class GetAppDataDirectoryDelegate : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    static void ctor(JNIEnv *env, jobject obj, jclass clazz);
    static jstring GetAppDataDirectory(JNIEnv *env, jobject obj);
    static jstring GetExternalStorageDirectory(JNIEnv *env, jobject obj);
};
