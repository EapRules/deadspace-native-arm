#include <stdio.h>

#include "platform.h"
#include "so_util.h"
#include "jni.h"
#include "jni_internals.h"
#include "trace.h"
#include "eamobile_Query.h"

/*
 * Query.isContentReady - the reason the engine drew 600 empty frames.
 *
 * What was observed, before any of this existed: the loader reached M5 with 600
 * NativeOnDrawFrame calls in ~1.2 s of wall clock under qemu, and `qemu-arm
 * -strace` showed exactly four syscalls touching the game directory in the
 * whole run - all four of them the .so being mapped. Zero asset files opened.
 * The log had the matching pair repeated 599 times, once per frame:
 *
 *     FindClass: no fake class registered for 'com/eamobile/Query'
 *     GetStaticMethodID: NULL class dereference ... 'isContentReady'
 *
 * That is a poll, not a one-off probe. The engine asks every frame whether its
 * content has arrived and does nothing else until the answer is yes; because
 * the class was missing the jmethodID came back NULL, jni.cpp's null-guard in
 * CallStaticBooleanMethod returned 0, and the gate never opened. It never
 * crashed - it just never started. That is why the frame counter alone cannot
 * distinguish a running game from a parked one, and why instrumenting fopen /
 * glTexImage2D / glDraw* before fixing this would only have produced
 * "assets=0 textures=0 draws=0".
 *
 * The signature is read out of the binary, not guessed: the literal
 * "isContentReady" sits at file offset 0x3e74c8 and is immediately followed by
 * "()Z" at 0x3e74d8 - static, no arguments, boolean.
 *
 * Answering JNI_TRUE unconditionally is correct here rather than a shortcut.
 * On Android this class lives in the launcher and reports on the DLC download;
 * a PortMaster install has the extracted asset tree in place before the process
 * starts, so there is no state to model and nothing that could later become
 * false. The alternative - forwarding to something in the .so, as EAIO.Startup
 * does - is not available: the game exports no Java_com_eamobile_Query_* symbol
 * (checked against the export table), because on Android this class is plain
 * Java, not a native declaration.
 */
static jboolean Query_isContentReady(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;

    /* Logged once. The engine polls this every frame and a per-call trace would
     * bury the rest of the run in 600 identical lines - which is exactly how
     * the failure above stayed invisible for an iteration. */
    static bool announced = false;
    if (!announced) {
        announced = true;
        trace("Query.isContentReady -> true (assets are on disk, no DLC fetch)");
    }

    return JNI_TRUE;
}

/*
 * The content package version.
 *
 * The engine asks for this right after it starts reading published/, and until
 * now got "Class Query does not have static method getVersion" in the log and a
 * NULL back. On Android it returns the version of the downloadable content
 * package; here the content is whatever the user copied to the card and there
 * is no manifest to read a version out of.
 *
 * "1.1.33" is not invented - it is the Xperia Play release this port pins by
 * sha1 and the harness re-checks on every run. Answering with the version we
 * actually require is the only answer that stays true: a made-up string would
 * be a claim about content nobody verified, and an empty one would put the
 * engine on a "no content installed" path while the content sits right there.
 */
static jstring Query_getVersion(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;

    /* Built once and kept: DeleteLocalRef is a no-op in this port, so a fresh
     * String per call would leak one every time. Same reasoning as
     * SystemAndroidDelegate's property getters. */
    static String *cached = NULL;
    if (!cached) {
        cached = new String("1.1.33");
        trace("Query.getVersion -> 1.1.33 (the pinned Xperia Play build)");
    }
    return (jstring)cached;
}

/*
 * How much memory the engine believes it has, in megabytes.
 *
 * Not yet requested in any run log - it is added because the Vita port
 * implements it and because of where the current M4 fault lands. The engine
 * dies with an empty std::vector whose push_back never happened, and the push
 * is guarded by an allocator returning NULL:
 *
 *     2dd518  bl 3007bc          ; the allocator singleton
 *     2dd538  ldr pc, [ip, #12]  ; allocate(44, ...)
 *     2dd53c  subs r5, r0, #0    ; and check for NULL
 *
 * An engine that sized its pool from a zero answer here would fail every
 * allocation exactly like that. This is a hypothesis, not a diagnosis, and it
 * is cheap enough to settle by answering honestly rather than by argument.
 *
 * 256 is what the Vita port returns and it is also true of the target: the R36S
 * has 1 GB with the CFW and framebuffer already in it, and no port should try
 * to claim all of it. If the engine ever does scale a pool from this number,
 * the log line makes that visible in one run.
 */
static jlong Query_getTotalMemory(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;

    static bool announced = false;
    if (!announced) {
        announced = true;
        trace("Query.getTotalMemory -> 256 MB");
    }
    return 256;
}

const ManagedMethod QueryClassMethods[] = {
    ManagedMethod::RegisterStatic<&Query_getTotalMemory>(
        Query::clazz, "getTotalMemory", "()J"),
    ManagedMethod::RegisterStatic<&Query_isContentReady>(
        Query::clazz, "isContentReady", "()Z"),
    ManagedMethod::RegisterStatic<&Query_getVersion>(
        Query::clazz, "getVersion", "()Ljava/lang/String;"),
    {NULL},
};

Class Query::clazz = {
    .classpath        = "com/eamobile/Query",
    .classname        = "Query",
    .managed_methods  = QueryClassMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = 0,
};

static const int registered = ClassRegistry::register_class(Query::clazz);
