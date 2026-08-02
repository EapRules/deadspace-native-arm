#include <stdio.h>

#include "platform.h"
#include "jni.h"
#include "jni_internals.h"
#include "trace.h"
#include "ea_SystemAndroidDelegate.h"

/*
 * How this one was pinned down, because the log alone points at the wrong
 * class.
 *
 * The run dies at text+0x0031e5a8, `ldr r3,[r7]` with r7 = 0, deterministic.
 * The two lines immediately above it in the log are the failed FindClass of
 * com/ea/blast/DisplayAndroidDelegate and of
 * com/ea/blast/DeviceOrientationHandlerAndroidDelegate, which is why the
 * previous two iterations both nominated the display delegate as the cause.
 * It is not. Reading the crash site and then the core dump gives a different
 * answer:
 *
 *   +0x31e598  ldr r7,[r5,#0xa0]      cached pointer, NULL on first use
 *   +0x31e5a0  beq +0x31e5c4          -> lazily fetch it
 *   +0x31e5d0  mov r1,#100            key
 *   +0x31e5e4  ldr pc,[r3,#12]        manager->vtable[3](key, 0)
 *   +0x31e5ec  str r0,[r5,#0xa0]      ... and cache whatever came back
 *   +0x31e5a8  ldr r3,[r7]            <- faults, nothing checked r0
 *
 * That vtable slot is EAMCore::ModuleRegistry's lookup (text+0x23833c): it
 * walks the global registry map, matches an entry on (field+64 == key,
 * field+56 == 0), and hands the match to vtable[4] (text+0x2393b4), which
 * scans the manager's vector of registered module factories at this+48..+52
 * for one whose type matches. No match -> NULL.
 *
 * The core dump names the key. The registry entry that matched holds a
 * std::string at +16 and another at +68, and both read "Accelerometer"; its
 * key at +64 is 0x64 = 100. So the engine is asking the module registry for
 * the Accelerometer module. The descriptor is there - it is a static catalog
 * entry, the engine ships one per module type ("The accelerometer" /
 * "Accelerometer" at .rodata 0x3f5c10). What is missing is a factory: the
 * manager's vector holds three of them and none provides an accelerometer.
 *
 * And that is downstream of this class. The factories are registered per piece
 * of hardware the *device profile* claims exists, and the device profile is
 * read entirely through com/ea/blast/SystemAndroidDelegate - which was not in
 * the fake registry, so GetAccelerometerCount() came back through a NULL
 * jmethodID as an empty answer, no accelerometer was declared, no
 * AccelerometerAndroid factory was built, and the first consumer to ask the
 * registry for one got NULL and dereferenced it without looking.
 *
 * Why "1" and not "0" for the accelerometer count, which would be the honest
 * answer for an R36S: returning 0 is what the port already does today, by
 * accident, and the crash *is* that behaviour. The caller at +0x31e578 asks
 * the registry unconditionally and has no NULL path. Declaring the hardware
 * present is also what the working Vita port does, and there is no
 * accelerometer on a Vita either. The device profile the engine wants is a
 * plausible Android handset, not an inventory of the handheld.
 *
 * The values themselves, and which of them are load-bearing, are documented on
 * SYSTEM_ANDROID_DELEGATE_PROPERTIES in the header.
 */

/*
 * The constructor. Nothing to construct - the delegate is a pure lookup table -
 * but it has to exist, because iface_NewObjectV refuses a class with
 * instance_size 0 and the engine builds the delegate before calling anything
 * on it.
 */
void SystemAndroidDelegate::ctor(JNIEnv *env, jobject obj, jclass clazz)
{
    (void)env;
    (void)obj;
    (void)clazz;

    trace("SystemAndroidDelegate constructed (accelerometer=1, keyboard=1, "
          "reporting sony/R800 for the Xperia Play input path)");
}

/*
 * One getter per property.
 *
 * Each String is built once and kept: DeleteLocalRef is a no-op in this port,
 * so returning a fresh String per call would leak one every time the engine
 * re-reads a property. Same reasoning as GetAppDataDirectoryDelegate and
 * xt_System's getDeviceInfo. The engine reads them during startup and again
 * whenever it re-queries EAMCore::System, so this is not a one-shot path.
 */
#define SYSTEM_ANDROID_DELEGATE_DEFINE(name, value)                            \
    jstring SystemAndroidDelegate::name(JNIEnv *env, jobject obj)              \
    {                                                                          \
        (void)env;                                                             \
        (void)obj;                                                             \
        static String *cached = NULL;                                          \
        if (!cached)                                                           \
            cached = new String(value);                                        \
        return (jstring)cached;                                                \
    }
SYSTEM_ANDROID_DELEGATE_PROPERTIES(SYSTEM_ANDROID_DELEGATE_DEFINE)
#undef SYSTEM_ANDROID_DELEGATE_DEFINE

/*
 * Registered through ManagedMethod::Register directly rather than through the
 * REGISTER_METHOD macro, for the reason spelled out in
 * ea_GetAppDataDirectoryDelegate.cpp: DEDUCE_METHOD_HELPER cannot express a
 * plain (JNIEnv*, jobject, ...) instance method.
 */
const ManagedMethod SystemAndroidDelegateMethods[] = {
    ManagedMethod::RegisterNonVirtual<&SystemAndroidDelegate::ctor>(
        SystemAndroidDelegate::clazz, "<init>", "()V"),

#define SYSTEM_ANDROID_DELEGATE_ENTRY(name, value)                             \
    ManagedMethod::Register<&SystemAndroidDelegate::name>(                     \
        SystemAndroidDelegate::clazz, #name, "()Ljava/lang/String;"),
    SYSTEM_ANDROID_DELEGATE_PROPERTIES(SYSTEM_ANDROID_DELEGATE_ENTRY)
#undef SYSTEM_ANDROID_DELEGATE_ENTRY

    {NULL},
};

Class SystemAndroidDelegate::clazz = {
    .classpath        = "com/ea/blast/SystemAndroidDelegate",
    .classname        = "SystemAndroidDelegate",
    .managed_methods  = SystemAndroidDelegateMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = sizeof(SystemAndroidDelegate),
};

static const int registered =
    ClassRegistry::register_class(SystemAndroidDelegate::clazz);
