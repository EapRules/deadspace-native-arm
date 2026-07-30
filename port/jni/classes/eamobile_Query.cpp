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

const ManagedMethod QueryClassMethods[] = {
    ManagedMethod::RegisterStatic<&Query_isContentReady>(
        Query::clazz, "isContentReady", "()Z"),
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
