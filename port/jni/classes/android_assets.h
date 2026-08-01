#pragma once
#include "jni.h"
#include "jni_internals.h"

/*
 * The asset-IO half of the fake Java layer: the four classes the engine walks
 * through to reach a file, in one file because they are one path.
 *
 *     MainActivity.GetInstance()          -> the activity
 *     activity.getAssets()                -> the AssetManager
 *     assets.open() / openFd() / list()   -> a stream, a descriptor, a listing
 *     stream.read() / skip() / close()
 *     descriptor.getLength()
 *
 * Every signature here was transcribed from the descriptor the engine handed
 * to GetMethodID (see harness/results/02-run.log), never guessed: lookup is an
 * exact strcmp on name *and* descriptor, so a plausible-but-wrong signature
 * fails identically to a class that was never registered, and leaves the same
 * uninformative line behind.
 *
 * One surprise worth writing down, because it contradicts what the class names
 * suggest. The engine calls FindClass("java/io/InputStream") once and then
 * looks up all six of read/close/skip/open/openFd/list on that single jclass -
 * including the three that belong to AssetManager on real Android. The run log
 * proves it: exactly one FindClass warning appears for java/io/InputStream and
 * six NULL-class GetMethodID lines follow it, with no FindClass of
 * android/content/res/AssetManager anywhere. So all six live on InputStream
 * here. The Vita port lumps them into one file for the same reason.
 */

/* com/ea/blast/MainActivity */
class MainActivity : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    static jobject GetInstance(JNIEnv *env, jclass clazz);
    static jobject getAssets(JNIEnv *env, jobject obj);
};

/* android/content/res/AssetManager - registered so the object getAssets()
 * returns has a class of its own; its methods are the ones on InputStream. */
class AssetManager : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
};

/* java/io/InputStream, carrying the AssetManager methods too - see above. */
class InputStream : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    static jint        read(JNIEnv *env, jobject obj, jbyteArray b, jint off, jint len);
    static void        closeStream(JNIEnv *env, jobject obj);
    static jlong       skip(JNIEnv *env, jobject obj, jlong n);
    static jobject     openStream(JNIEnv *env, jobject obj, jstring name);
    static jobject     openFd(JNIEnv *env, jobject obj, jstring name);
    static jobjectArray listAssets(JNIEnv *env, jobject obj, jstring path);
};

/* android/content/res/AssetFileDescriptor */
class AssetFileDescriptor : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    static jlong getLength(JNIEnv *env, jobject obj);
};
