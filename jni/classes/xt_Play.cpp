#include <stdio.h>
#include <vector>

#include "platform.h"
#include "jni.h"
#include "jni_internals.h"
#include "xt_Play.h"

/*
 * init() is called from xt::java::init() with ANativeActivity::clazz, once
 * cacheStoreMethodIds() has the five ids. On Android it starts the ad and
 * billing SDKs; here there is nothing to start, and the log line is the proof
 * that the class was found and entered.
 */
static void xtPlay_init(JNIEnv *env, jclass clazz, jobject activity)
{
    (void)env; (void)clazz; (void)activity;
    fprintf(stderr, "xtPlay.init: no ad or billing backend in this port.\n");
}

static void xtPlay_deinit(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
}

/*
 * There is no ad network linked into this port, so there is never an ad to
 * show. This is the answer that keeps the rest honest: the engine only calls
 * displayRewardedVideoAd() after this says yes.
 */
static jboolean xtPlay_isRewardedVideoAdAvailable(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    return JNI_FALSE;
}

/*
 * Unreachable while isRewardedVideoAdAvailable() answers false. If the engine
 * ever gets here anyway, that is a fact worth seeing in the log rather than a
 * silent no-op, because the player is being shown nothing and will be waiting
 * for a video that will not play.
 */
static void xtPlay_displayRewardedVideoAd(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    warning("xtPlay: asked to play a rewarded video ad, but this port has no ad backend.\n");
}

/*
 * No ad was ever watched, so no reward is pending. Returning true here would
 * be granting in-game currency the player did not earn - the game credits the
 * reward on the strength of this answer alone.
 */
static jboolean xtPlay_fetchVideoAdRewardPending(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    return JNI_FALSE;
}

const ManagedMethod xtPlayClassMethods[] = {
    ManagedMethod::RegisterStatic<&xtPlay_init>(
        xtPlay::clazz, "init", "(Landroid/app/NativeActivity;)V"),
    ManagedMethod::RegisterStatic<&xtPlay_deinit>(
        xtPlay::clazz, "deinit", "()V"),
    ManagedMethod::RegisterStatic<&xtPlay_isRewardedVideoAdAvailable>(
        xtPlay::clazz, "isRewardedVideoAdAvailable", "()Z"),
    ManagedMethod::RegisterStatic<&xtPlay_displayRewardedVideoAd>(
        xtPlay::clazz, "displayRewardedVideoAd", "()V"),
    ManagedMethod::RegisterStatic<&xtPlay_fetchVideoAdRewardPending>(
        xtPlay::clazz, "fetchVideoAdRewardPending", "()Z"),
    {NULL},
};

Class xtPlay::clazz = {
    .classpath        = "com/ea/blast/xtPlay",
    .classname        = "xtPlay",
    .managed_methods  = xtPlayClassMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = 0,
};

static const int registered = ClassRegistry::register_class(xtPlay::clazz);
