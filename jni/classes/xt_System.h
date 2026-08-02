#pragma once
#include "jni.h"
#include "jni_internals.h"

/*
 * xtSystem — the engine's Java glue for everything that is not the store.
 *
 * The game asks for it by the bare name "xtSystem" (no package), through
 * activity.getClassLoader().loadClass(), and then caches nine static method
 * ids in xt::java::initJNI() before calling xtSystem.init(activity). The names
 * and signatures below are not guesses: they were read out of
 * libdeadspace.so's literal pools at 0x9832c.
 *
 * Every method here answers for the port truthfully. There is no toast, no
 * browser and no dialog on a handheld running from a PortMaster script, so
 * those calls say what the game wanted to show and where it went, in the log,
 * instead of pretending a UI appeared. Nothing here reports success for work
 * that did not happen.
 */
class xtSystem : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
};
