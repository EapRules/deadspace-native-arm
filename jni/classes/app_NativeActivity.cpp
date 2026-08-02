#include "jni.h"
#include "jni_internals.h"
#include "app_NativeActivity.h"
#include "lang_ClassLoader.h"

/*
 * Activity.getClassLoader().
 *
 * The game asks the activity for this as soon as it is running, and it is not
 * optional: its whole Java-side glue is reached through the loader this
 * returns. There is one loader for the process, so the same object comes back
 * every time, exactly as the framework does.
 */
static jobject NativeActivity_getClassLoader(JNIEnv *env, jobject self)
{
    (void)env; (void)self;
    return (jobject)&g_class_loader;
}

const ManagedMethod appNativeActivityClassMethods[] = {
    ManagedMethod::Register<&NativeActivity_getClassLoader>(
        AppNativeActivity::clazz, "getClassLoader", "()Ljava/lang/ClassLoader;"),
    {NULL},
};

Class AppNativeActivity::clazz = {
    .classpath = "android/app/NativeActivity",
    .classname = "app_NativeActivity",
    .managed_methods = appNativeActivityClassMethods,
    .native_methods = {NULL},
    .fields = {NULL},
    .instance_size = sizeof(AppNativeActivity),
};

AppNativeActivity g_native_activity_object;

static const int registered = ClassRegistry::register_class(AppNativeActivity::clazz);
