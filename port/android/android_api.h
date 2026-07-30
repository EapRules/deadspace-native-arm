/*
 * Minimal Android NDK surface needed by libdeadspace.so.
 *
 * Only what the game actually imports is declared here: the 42 libandroid
 * symbols found in the .so's dynamic table, no more. Struct layouts must match
 * the armeabi-v7a NDK exactly, because the game indexes into them directly.
 */
#ifndef DEADSPACE_ANDROID_API_H
#define DEADSPACE_ANDROID_API_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Opaque handles. The game only ever passes these around as pointers.
 * ------------------------------------------------------------------ */
typedef struct AAssetManager AAssetManager;
typedef struct AAsset AAsset;
typedef struct AAssetDir AAssetDir;
typedef struct ALooper ALooper;
typedef struct AInputQueue AInputQueue;
typedef struct AInputEvent AInputEvent;
typedef struct ANativeWindow ANativeWindow;
typedef struct AConfiguration AConfiguration;

/* ------------------------------------------------------------------ *
 * asset_manager.h
 * ------------------------------------------------------------------ */
enum {
    AASSET_MODE_UNKNOWN   = 0,
    AASSET_MODE_RANDOM    = 1,
    AASSET_MODE_STREAMING = 2,
    AASSET_MODE_BUFFER    = 3,
};

AAsset      *AAssetManager_open(AAssetManager *mgr, const char *filename, int mode);
AAssetDir   *AAssetManager_openDir(AAssetManager *mgr, const char *dirName);
const char  *AAssetDir_getNextFileName(AAssetDir *assetDir);
void         AAssetDir_close(AAssetDir *assetDir);
const void  *AAsset_getBuffer(AAsset *asset);
off_t        AAsset_getLength(AAsset *asset);
int          AAsset_openFileDescriptor(AAsset *asset, off_t *outStart, off_t *outLength);
void         AAsset_close(AAsset *asset);

/* ------------------------------------------------------------------ *
 * configuration.h  (only language/country are used, for localisation)
 * ------------------------------------------------------------------ */
AConfiguration *AConfiguration_new(void);
void            AConfiguration_delete(AConfiguration *config);
void            AConfiguration_fromAssetManager(AConfiguration *out, AAssetManager *am);
void            AConfiguration_getLanguage(AConfiguration *config, char *outLanguage);
void            AConfiguration_getCountry(AConfiguration *config, char *outCountry);

/* ------------------------------------------------------------------ *
 * native_window.h
 * ------------------------------------------------------------------ */
int32_t ANativeWindow_getWidth(ANativeWindow *window);
int32_t ANativeWindow_getHeight(ANativeWindow *window);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window,
                                         int32_t width, int32_t height, int32_t format);

/* ------------------------------------------------------------------ *
 * looper.h
 * ------------------------------------------------------------------ */
enum {
    ALOOPER_PREPARE_ALLOW_NON_CALLBACKS = 1 << 0,
};
enum {
    ALOOPER_POLL_WAKE     = -1,
    ALOOPER_POLL_CALLBACK = -2,
    ALOOPER_POLL_TIMEOUT  = -3,
    ALOOPER_POLL_ERROR    = -4,
};
enum {
    ALOOPER_EVENT_INPUT  = 1 << 0,
    ALOOPER_EVENT_OUTPUT = 1 << 1,
    ALOOPER_EVENT_ERROR  = 1 << 2,
    ALOOPER_EVENT_HANGUP = 1 << 3,
    ALOOPER_EVENT_INVALID = 1 << 4,
};

typedef int (*ALooper_callbackFunc)(int fd, int events, void *data);

ALooper *ALooper_prepare(int opts);
int      ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents, void **outData);
int      ALooper_addFd(ALooper *looper, int fd, int ident, int events,
                       ALooper_callbackFunc callback, void *data);

/* ------------------------------------------------------------------ *
 * input.h
 * ------------------------------------------------------------------ */
enum {
    AINPUT_EVENT_TYPE_KEY    = 1,
    AINPUT_EVENT_TYPE_MOTION = 2,
};
enum {
    AKEY_EVENT_ACTION_DOWN     = 0,
    AKEY_EVENT_ACTION_UP       = 1,
    AKEY_EVENT_ACTION_MULTIPLE = 2,
};
enum {
    AMOTION_EVENT_ACTION_MASK          = 0xff,
    AMOTION_EVENT_ACTION_POINTER_INDEX_MASK = 0xff00,
    AMOTION_EVENT_ACTION_DOWN          = 0,
    AMOTION_EVENT_ACTION_UP            = 1,
    AMOTION_EVENT_ACTION_MOVE          = 2,
    AMOTION_EVENT_ACTION_CANCEL        = 3,
    AMOTION_EVENT_ACTION_OUTSIDE       = 4,
    AMOTION_EVENT_ACTION_POINTER_DOWN  = 5,
    AMOTION_EVENT_ACTION_POINTER_UP    = 6,
};
enum {
    AINPUT_SOURCE_UNKNOWN     = 0x00000000,
    AINPUT_SOURCE_KEYBOARD    = 0x00000101,
    AINPUT_SOURCE_DPAD        = 0x00000201,
    AINPUT_SOURCE_GAMEPAD     = 0x00000401,
    AINPUT_SOURCE_TOUCHSCREEN = 0x00001002,
    AINPUT_SOURCE_JOYSTICK    = 0x01000010,
};

int32_t AInputEvent_getType(const AInputEvent *event);
int32_t AInputEvent_getDeviceId(const AInputEvent *event);
int32_t AInputEvent_getSource(const AInputEvent *event);

int32_t AKeyEvent_getAction(const AInputEvent *key_event);
int32_t AKeyEvent_getFlags(const AInputEvent *key_event);
int32_t AKeyEvent_getKeyCode(const AInputEvent *key_event);
int32_t AKeyEvent_getMetaState(const AInputEvent *key_event);

int32_t AMotionEvent_getAction(const AInputEvent *motion_event);
int32_t AMotionEvent_getFlags(const AInputEvent *motion_event);
int32_t AMotionEvent_getMetaState(const AInputEvent *motion_event);
size_t  AMotionEvent_getPointerCount(const AInputEvent *motion_event);
int32_t AMotionEvent_getPointerId(const AInputEvent *motion_event, size_t pointer_index);
float   AMotionEvent_getX(const AInputEvent *motion_event, size_t pointer_index);
float   AMotionEvent_getY(const AInputEvent *motion_event, size_t pointer_index);
size_t  AMotionEvent_getHistorySize(const AInputEvent *motion_event);
float   AMotionEvent_getHistoricalX(const AInputEvent *motion_event,
                                    size_t pointer_index, size_t history_pos);
float   AMotionEvent_getHistoricalY(const AInputEvent *motion_event,
                                    size_t pointer_index, size_t history_pos);
/* Resolved by the game through dlsym rather than the PLT - see the definition
 * in android/platform.cpp for why, and for what happens when they are absent. */
float   AMotionEvent_getAxisValue(const AInputEvent *motion_event, int32_t axis,
                                  size_t pointer_index);
float   AMotionEvent_getHistoricalAxisValue(const AInputEvent *motion_event, int32_t axis,
                                            size_t pointer_index, size_t history_pos);

int32_t AInputQueue_getEvent(AInputQueue *queue, AInputEvent **outEvent);
int32_t AInputQueue_preDispatchEvent(AInputQueue *queue, AInputEvent *event);
void    AInputQueue_finishEvent(AInputQueue *queue, AInputEvent *event, int handled);
void    AInputQueue_attachLooper(AInputQueue *queue, ALooper *looper, int ident,
                                 ALooper_callbackFunc callback, void *data);
void    AInputQueue_detachLooper(AInputQueue *queue);

/* ------------------------------------------------------------------ *
 * native_activity.h
 *
 * The game receives a pointer to ANativeActivity and reads its fields
 * directly, so field order and offsets are load-bearing. This matches the
 * NDK definition for 32-bit ARM.
 * ------------------------------------------------------------------ */
typedef struct ANativeActivity ANativeActivity;
typedef struct ARect { int32_t left, top, right, bottom; } ARect;

typedef struct ANativeActivityCallbacks {
    void (*onStart)(ANativeActivity *activity);
    void (*onResume)(ANativeActivity *activity);
    void *(*onSaveInstanceState)(ANativeActivity *activity, size_t *outSize);
    void (*onPause)(ANativeActivity *activity);
    void (*onStop)(ANativeActivity *activity);
    void (*onDestroy)(ANativeActivity *activity);
    void (*onWindowFocusChanged)(ANativeActivity *activity, int hasFocus);
    void (*onNativeWindowCreated)(ANativeActivity *activity, ANativeWindow *window);
    void (*onNativeWindowResized)(ANativeActivity *activity, ANativeWindow *window);
    void (*onNativeWindowRedrawNeeded)(ANativeActivity *activity, ANativeWindow *window);
    void (*onNativeWindowDestroyed)(ANativeActivity *activity, ANativeWindow *window);
    void (*onInputQueueCreated)(ANativeActivity *activity, AInputQueue *queue);
    void (*onInputQueueDestroyed)(ANativeActivity *activity, AInputQueue *queue);
    void (*onContentRectChanged)(ANativeActivity *activity, const ARect *rect);
    void (*onConfigurationChanged)(ANativeActivity *activity);
    void (*onLowMemory)(ANativeActivity *activity);
} ANativeActivityCallbacks;

struct ANativeActivity {
    ANativeActivityCallbacks *callbacks;
    void *vm;                    /* JavaVM*   */
    void *env;                   /* JNIEnv*   */
    void *clazz;                 /* jobject   */
    const char *internalDataPath;
    const char *externalDataPath;
    int32_t sdkVersion;
    void *instance;              /* free for the app to use */
    AAssetManager *assetManager;
    const char *obbPath;
};

void ANativeActivity_finish(ANativeActivity *activity);

/* Entry point exported by the game's .so. */
typedef void ANativeActivity_createFunc(ANativeActivity *activity,
                                        void *savedState, size_t savedStateSize);

#ifdef __cplusplus
}
#endif

#endif /* DEADSPACE_ANDROID_API_H */
