#pragma once
#include "jni.h"
#include "jni_internals.h"

/*
 * com/ea/blast/DisplayAndroidDelegate — what the engine believes the screen is.
 *
 * Built during renderer bring-up, immediately before the first frame, and read
 * for panel size, DPI and orientation. Every signature here was transcribed
 * from the descriptors the engine passed to GetMethodID, not guessed: method
 * lookup is an exact strcmp on name *and* signature, so a plausible-looking
 * "()V" that should have been "(Z)V" fails exactly like a missing class, and
 * with no new evidence in the log to tell the two apart.
 */
class DisplayAndroidDelegate : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    static void  ctor(JNIEnv *env, jobject obj, jclass clazz);
    static jint  GetDefaultWidth(JNIEnv *env, jobject obj);
    static jint  GetDefaultHeight(JNIEnv *env, jobject obj);
    static jfloat GetDpiX(JNIEnv *env, jobject obj);
    static jfloat GetDpiY(JNIEnv *env, jobject obj);
    static jint  GetStdOrientation(JNIEnv *env, jobject obj);
    static void  SetStdOrientation(JNIEnv *env, jobject obj, jint orientation);
};
