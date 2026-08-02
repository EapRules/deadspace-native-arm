#pragma once
#include "jni.h"
#include "jni_internals.h"

/*
 * java.lang.ClassLoader.
 *
 * The game reaches its Java glue the long way round: activity.getClassLoader()
 * and then loader.loadClass("com.ea.blast.xtPlay"),
 * rather than JNI's FindClass. That is the standard workaround for calling
 * FindClass off the main thread, where the JNI class lookup has no application
 * class loader to consult - and this port runs the engine on its own thread
 * too, so the game takes that path here as well.
 *
 * There is exactly one loader, and it can only ever hand out the classes this
 * port actually implements. Asked for anything else it answers "not found",
 * which is a true statement and one the game already knows how to handle: it
 * logs that the store integration is missing and carries on without it.
 */
class LangClassLoader : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
};

/* The single instance, returned by NativeActivity.getClassLoader(). */
extern LangClassLoader g_class_loader;
