#pragma once
#include "jni.h"
#include "jni_internals.h"

/*
 * com/ea/blast/xtPlay — the store and rewarded-video
 * glue. cacheStoreMethodIds() (0x97ec8) caches five static methods on it and
 * xt::java::init() calls xtPlay.init(activity); the names and signatures were
 * read out of the binary's literal pools, not guessed.
 *
 * This port has no ad SDK and no billing, and it does not pretend otherwise:
 * no rewarded video is ever available and no reward is ever pending. Answering
 * "an ad is ready" or "you earned a reward" would hand the player currency
 * nobody paid for and would send the engine into a code path with nothing
 * behind it.
 */
class xtPlay : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
};
