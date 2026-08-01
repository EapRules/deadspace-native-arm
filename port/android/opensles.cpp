/*
 * OpenSL ES 1.0.1 (plus Android's buffer-queue extension) on top of SDL audio.
 *
 * Dead Space does all of its audio through OpenSL ES: it creates an engine, an
 * output mix, and one buffer-queue AudioPlayer per channel, then streams PCM
 * into each player's queue and refills it from the queue's callback.
 *
 * This cannot be refused. The engine's own bring-up ignores the result of
 * slCreateEngine and dereferences the object it was supposed to receive:
 *
 *     blx  slCreateEngine
 *     cmp  r0, #17          ; only *logs* on a hard failure
 *     ldr  r0, [r5]         ; engineObj - null if we refused
 *     ldr  r1, [r0]         ; <- SIGSEGV
 *
 * so a "documented refusal" is a crash on the first frame. Every entry point
 * below is therefore backed by something real: the objects are real objects
 * with the specification's vtable layouts, the PCM format is handed to SDL as
 * the game described it, and Enqueue really queues audio on a device.
 *
 * Where the host has no audio device at all - a CI container, a headless
 * build - opening the device fails and is reported once. The queue then still
 * accounts for buffers in real time and still calls the game back when they
 * are due, because the game's mixer is a state machine driven by that
 * callback: dropping it would wedge the mixer, which is a much worse failure
 * than silence.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <deque>

#include <SDL2/SDL.h>

#include "platform.h"
#include "trace.h"

extern "C" {

typedef uint32_t SLuint32;
typedef int32_t  SLint32;
typedef uint16_t SLuint16;
typedef int16_t  SLint16;
typedef uint8_t  SLboolean;
typedef SLuint32 SLresult;
typedef SLint16  SLmillibel;
typedef SLuint32 SLmillisecond;
typedef SLuint32 SLpermille;

#define SL_BOOLEAN_FALSE              ((SLboolean)0)
#define SL_BOOLEAN_TRUE               ((SLboolean)1)

#define SL_RESULT_SUCCESS             ((SLuint32)0x00000000)
#define SL_RESULT_PARAMETER_INVALID   ((SLuint32)0x0000000D)
#define SL_RESULT_FEATURE_UNSUPPORTED ((SLuint32)0x00000006)

#define SL_OBJECT_STATE_UNREALIZED    ((SLuint32)0x00000001)
#define SL_OBJECT_STATE_REALIZED      ((SLuint32)0x00000002)

#define SL_PLAYSTATE_STOPPED          ((SLuint32)0x00000001)
#define SL_PLAYSTATE_PAUSED           ((SLuint32)0x00000002)
#define SL_PLAYSTATE_PLAYING          ((SLuint32)0x00000003)

/* 1 is SL_DATAFORMAT_MIME; PCM is 2. Getting this backwards makes every
 * CreateAudioPlayer refuse a perfectly good PCM source. */
#define SL_DATAFORMAT_PCM             ((SLuint32)0x00000002)

#define SL_BYTEORDER_LITTLEENDIAN     ((SLuint32)0x00000002)

struct SLInterfaceID_ {
    uint32_t time_low;
    uint16_t time_mid;
    uint16_t time_hi_and_version;
    uint16_t clock_seq;
    uint8_t  node[6];
};
typedef const struct SLInterfaceID_ *SLInterfaceID;

/* UUIDs straight from the OpenSL ES 1.0.1 headers. */
static const struct SLInterfaceID_ kEngine =
    { 0x8d97c260, 0xddd4, 0x11db, 0x958f, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const struct SLInterfaceID_ kPlay =
    { 0xef0bd9c0, 0xddd7, 0x11db, 0xbf49, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const struct SLInterfaceID_ kSeek =
    { 0x3ef2a2c0, 0xddd6, 0x11db, 0xa4d4, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const struct SLInterfaceID_ kVolume =
    { 0x09f6f1c0, 0xddd8, 0x11db, 0xa9f0, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const struct SLInterfaceID_ kPlaybackRate =
    { 0xd9ff2d80, 0xddd5, 0x11db, 0xb4f3, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const struct SLInterfaceID_ kEffectSend =
    { 0x09e8ede0, 0xddd7, 0x11db, 0xb4f6, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const struct SLInterfaceID_ kDynamicInterfaceManagement =
    { 0x3e2fa960, 0xddd4, 0x11db, 0xbf0c, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
/* Android's vendor extension, from SLES/OpenSLES_AndroidConfiguration.h. */
static const struct SLInterfaceID_ kBufferQueue =
    { 0x2bc99ccc, 0x41c4, 0x11df, 0xac9b, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };

SLInterfaceID SL_IID_ENGINE                       = &kEngine;
SLInterfaceID SL_IID_PLAY                         = &kPlay;
SLInterfaceID SL_IID_SEEK                         = &kSeek;
SLInterfaceID SL_IID_VOLUME                       = &kVolume;
SLInterfaceID SL_IID_PLAYBACKRATE                 = &kPlaybackRate;
SLInterfaceID SL_IID_EFFECTSEND                   = &kEffectSend;
SLInterfaceID SL_IID_DYNAMICINTERFACEMANAGEMENT   = &kDynamicInterfaceManagement;
SLInterfaceID SL_IID_BUFFERQUEUE                  = &kBufferQueue;

/* ------------------------------------------------------------------ *
 * The data structures the game fills in and hands to CreateAudioPlayer.
 * ------------------------------------------------------------------ */
typedef struct {
    SLuint32 formatType;
    SLuint32 numChannels;
    SLuint32 samplesPerSec;   /* milliHz, not Hz - the spec's unit */
    SLuint32 bitsPerSample;
    SLuint32 containerSize;
    SLuint32 channelMask;
    SLuint32 endianness;
} SLDataFormat_PCM;

typedef struct {
    void *pLocator;
    void *pFormat;
} SLDataSource, SLDataSink;

/* Handles, as the spec declares them: a pointer to a pointer to a vtable. */
typedef const struct SLObjectItf_      * const *SLObjectItf;
typedef const struct SLEngineItf_      * const *SLEngineItf;
typedef const struct SLPlayItf_        * const *SLPlayItf;
typedef const struct SLVolumeItf_      * const *SLVolumeItf;
typedef const struct SLBufferQueueItf_ * const *SLBufferQueueItf;

typedef void (ABI_ATTR *slObjectCallback)(SLObjectItf caller, const void *ctx,
                                          SLuint32 event, SLresult result,
                                          SLuint32 param, void *iface);
typedef void (ABI_ATTR *slBufferQueueCallback)(SLBufferQueueItf caller, void *ctx);
typedef void (ABI_ATTR *slPlayCallback)(SLPlayItf caller, void *ctx, SLuint32 event);

/*
 * The vtables. Slot order is the specification's and is not negotiable: the
 * game indexes them numerically after the compiler inlined the member call,
 * e.g. "ldr r3, [r2, #12]" for SLObjectItf::GetInterface.
 */
struct SLObjectItf_ {
    SLresult (ABI_ATTR *Realize)(SLObjectItf self, SLboolean async);
    SLresult (ABI_ATTR *Resume)(SLObjectItf self, SLboolean async);
    SLresult (ABI_ATTR *GetState)(SLObjectItf self, SLuint32 *state);
    SLresult (ABI_ATTR *GetInterface)(SLObjectItf self, const SLInterfaceID iid, void *iface);
    SLresult (ABI_ATTR *RegisterCallback)(SLObjectItf self, slObjectCallback cb, void *ctx);
    void     (ABI_ATTR *AbortAsyncOperation)(SLObjectItf self);
    void     (ABI_ATTR *Destroy)(SLObjectItf self);
    SLresult (ABI_ATTR *SetPriority)(SLObjectItf self, SLint32 priority, SLboolean preemptable);
    SLresult (ABI_ATTR *GetPriority)(SLObjectItf self, SLint32 *priority);
    SLresult (ABI_ATTR *SetLossOfControlInterfaces)(SLObjectItf self, SLint16 num,
                                                    SLInterfaceID *ids, SLboolean enabled);
};

struct SLEngineItf_ {
    SLresult (ABI_ATTR *CreateLEDDevice)(SLEngineItf, SLObjectItf *, SLuint32, SLuint32, const SLInterfaceID *, const SLboolean *);
    SLresult (ABI_ATTR *CreateVibraDevice)(SLEngineItf, SLObjectItf *, SLuint32, SLuint32, const SLInterfaceID *, const SLboolean *);
    SLresult (ABI_ATTR *CreateAudioPlayer)(SLEngineItf, SLObjectItf *, SLDataSource *, SLDataSink *, SLuint32, const SLInterfaceID *, const SLboolean *);
    SLresult (ABI_ATTR *CreateAudioRecorder)(SLEngineItf, SLObjectItf *, SLDataSource *, SLDataSink *, SLuint32, const SLInterfaceID *, const SLboolean *);
    SLresult (ABI_ATTR *CreateMidiPlayer)(SLEngineItf, SLObjectItf *, SLDataSource *, SLDataSource *, SLDataSink *, SLDataSink *, SLuint32, const SLInterfaceID *, const SLboolean *);
    SLresult (ABI_ATTR *CreateListener)(SLEngineItf, SLObjectItf *, SLuint32, const SLInterfaceID *, const SLboolean *);
    SLresult (ABI_ATTR *Create3DGroup)(SLEngineItf, SLObjectItf *, SLuint32, const SLInterfaceID *, const SLboolean *);
    SLresult (ABI_ATTR *CreateOutputMix)(SLEngineItf, SLObjectItf *, SLuint32, const SLInterfaceID *, const SLboolean *);
    SLresult (ABI_ATTR *CreateMetadataExtractor)(SLEngineItf, SLObjectItf *, SLDataSource *, SLuint32, const SLInterfaceID *, const SLboolean *);
    SLresult (ABI_ATTR *CreateExtensionObject)(SLEngineItf, SLObjectItf *, void *, SLuint32, SLuint32, const SLInterfaceID *, const SLboolean *);
    SLresult (ABI_ATTR *QueryNumSupportedInterfaces)(SLEngineItf, SLuint32, SLuint32 *);
    SLresult (ABI_ATTR *QuerySupportedInterfaces)(SLEngineItf, SLuint32, SLuint32, SLInterfaceID *);
    SLresult (ABI_ATTR *QueryNumSupportedExtensions)(SLEngineItf, SLuint32 *);
    SLresult (ABI_ATTR *QuerySupportedExtension)(SLEngineItf, SLuint32, char *, SLint16 *);
    SLresult (ABI_ATTR *IsExtensionSupported)(SLEngineItf, const char *, SLboolean *);
};

struct SLPlayItf_ {
    SLresult (ABI_ATTR *SetPlayState)(SLPlayItf self, SLuint32 state);
    SLresult (ABI_ATTR *GetPlayState)(SLPlayItf self, SLuint32 *state);
    SLresult (ABI_ATTR *GetDuration)(SLPlayItf self, SLmillisecond *msec);
    SLresult (ABI_ATTR *GetPosition)(SLPlayItf self, SLmillisecond *msec);
    SLresult (ABI_ATTR *RegisterCallback)(SLPlayItf self, slPlayCallback cb, void *ctx);
    SLresult (ABI_ATTR *SetCallbackEventsMask)(SLPlayItf self, SLuint32 mask);
    SLresult (ABI_ATTR *GetCallbackEventsMask)(SLPlayItf self, SLuint32 *mask);
    SLresult (ABI_ATTR *SetMarkerPosition)(SLPlayItf self, SLmillisecond msec);
    SLresult (ABI_ATTR *ClearMarkerPosition)(SLPlayItf self);
    SLresult (ABI_ATTR *GetMarkerPosition)(SLPlayItf self, SLmillisecond *msec);
    SLresult (ABI_ATTR *SetPositionUpdatePeriod)(SLPlayItf self, SLmillisecond msec);
    SLresult (ABI_ATTR *GetPositionUpdatePeriod)(SLPlayItf self, SLmillisecond *msec);
};

struct SLVolumeItf_ {
    SLresult (ABI_ATTR *SetVolumeLevel)(SLVolumeItf self, SLmillibel level);
    SLresult (ABI_ATTR *GetVolumeLevel)(SLVolumeItf self, SLmillibel *level);
    SLresult (ABI_ATTR *GetMaxVolumeLevel)(SLVolumeItf self, SLmillibel *level);
    SLresult (ABI_ATTR *SetMute)(SLVolumeItf self, SLboolean mute);
    SLresult (ABI_ATTR *GetMute)(SLVolumeItf self, SLboolean *mute);
    SLresult (ABI_ATTR *EnableStereoPosition)(SLVolumeItf self, SLboolean enable);
    SLresult (ABI_ATTR *IsEnableStereoPosition)(SLVolumeItf self, SLboolean *enable);
    SLresult (ABI_ATTR *SetStereoPosition)(SLVolumeItf self, SLpermille pos);
    SLresult (ABI_ATTR *GetStereoPosition)(SLVolumeItf self, SLpermille *pos);
};

typedef const struct SLSeekItf_ * const *SLSeekItf;

struct SLSeekItf_ {
    SLresult (ABI_ATTR *SetPosition)(SLSeekItf self, SLmillisecond pos, SLuint32 mode);
    SLresult (ABI_ATTR *SetLoop)(SLSeekItf self, SLboolean enable,
                                 SLmillisecond start, SLmillisecond end);
    SLresult (ABI_ATTR *GetLoop)(SLSeekItf self, SLboolean *enable,
                                 SLmillisecond *start, SLmillisecond *end);
};

typedef struct {
    SLuint32 count;
    SLuint32 index;
} SLBufferQueueState;

struct SLBufferQueueItf_ {
    SLresult (ABI_ATTR *Enqueue)(SLBufferQueueItf self, const void *buffer, SLuint32 size);
    SLresult (ABI_ATTR *Clear)(SLBufferQueueItf self);
    SLresult (ABI_ATTR *GetState)(SLBufferQueueItf self, SLBufferQueueState *state);
    SLresult (ABI_ATTR *RegisterCallback)(SLBufferQueueItf self, slBufferQueueCallback cb, void *ctx);
};

} /* extern "C" */

/* ------------------------------------------------------------------ *
 * Objects.
 *
 * Every handle the game holds points at a struct whose first word is the
 * vtable pointer, which is what "(*obj)->Method(obj, ...)" dereferences. Each
 * interface handed out by GetInterface is its own two-word struct - vtable
 * plus a back-pointer to the object - so the implementation can recover the
 * object from the interface it was called on.
 * ------------------------------------------------------------------ */
struct Player;

template <typename VT, typename OWNER>
struct Iface {
    const VT *vt;
    OWNER    *owner;
};

struct OutputMix {
    const SLObjectItf_ *vt;
    bool realized;
};

struct Engine {
    const SLObjectItf_ *vt;
    Iface<SLEngineItf_, Engine> engine;
    bool realized;
};

struct Player {
    const SLObjectItf_ *vt;

    Iface<SLPlayItf_,        Player> play;
    Iface<SLVolumeItf_,      Player> volume;
    Iface<SLBufferQueueItf_, Player> queue;
    Iface<SLSeekItf_,        Player> seek;

    /* Format, as the game asked for it. */
    int channels;
    int freq;
    int bits;

    /* False for a compressed source this port cannot decode: the player is
     * real and its state machine works, but no samples ever reach a device. */
    bool              decodes;

    SDL_AudioDeviceID device;
    SLuint32          state;
    SLmillibel        level;
    bool              muted;
    bool              realized;
    bool              looping;

    /* Buffers the game has handed us and we have not called back for yet. */
    std::deque<SLuint32> pending;
    slBufferQueueCallback callback;
    void                 *callback_ctx;

    /* Fallback pacing when there is no device to drain the queue for us. */
    uint32_t last_drain_ms;
    double   carry_bytes;

    Player *next;
};

static Player  *g_players     = NULL;   /* every live player, for the pump */
static bool     g_audio_up    = false;
static bool     g_audio_tried = false;

static int bytes_per_sample(const Player *p)
{
    return (p->bits / 8) * p->channels;
}

static double bytes_per_ms(const Player *p)
{
    return (double)p->freq * bytes_per_sample(p) / 1000.0;
}

/* ------------------------------------------------------------------ *
 * SLBufferQueueItf
 * ------------------------------------------------------------------ */
static ABI_ATTR SLresult bq_Enqueue(SLBufferQueueItf self, const void *buffer, SLuint32 size)
{
    Player *p = ((Iface<SLBufferQueueItf_, Player> *)self)->owner;
    if (!p || !buffer || size == 0)
        return SL_RESULT_PARAMETER_INVALID;

    if (p->device)
        SDL_QueueAudio(p->device, buffer, size);

    p->pending.push_back(size);
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult bq_Clear(SLBufferQueueItf self)
{
    Player *p = ((Iface<SLBufferQueueItf_, Player> *)self)->owner;
    if (!p)
        return SL_RESULT_PARAMETER_INVALID;

    if (p->device)
        SDL_ClearQueuedAudio(p->device);
    p->pending.clear();
    p->carry_bytes = 0.0;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult bq_GetState(SLBufferQueueItf self, SLBufferQueueState *state)
{
    Player *p = ((Iface<SLBufferQueueItf_, Player> *)self)->owner;
    if (!p || !state)
        return SL_RESULT_PARAMETER_INVALID;

    state->count = (SLuint32)p->pending.size();
    state->index = 0;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult bq_RegisterCallback(SLBufferQueueItf self,
                                             slBufferQueueCallback cb, void *ctx)
{
    Player *p = ((Iface<SLBufferQueueItf_, Player> *)self)->owner;
    if (!p)
        return SL_RESULT_PARAMETER_INVALID;

    p->callback     = cb;
    p->callback_ctx = ctx;
    return SL_RESULT_SUCCESS;
}

static const SLBufferQueueItf_ kBufferQueueVt = {
    bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback,
};

/* ------------------------------------------------------------------ *
 * SLPlayItf
 * ------------------------------------------------------------------ */
static ABI_ATTR SLresult play_SetPlayState(SLPlayItf self, SLuint32 state)
{
    Player *p = ((Iface<SLPlayItf_, Player> *)self)->owner;
    if (!p)
        return SL_RESULT_PARAMETER_INVALID;

    p->state = state;
    if (p->device)
        SDL_PauseAudioDevice(p->device, state == SL_PLAYSTATE_PLAYING ? 0 : 1);
    if (state == SL_PLAYSTATE_STOPPED) {
        if (p->device)
            SDL_ClearQueuedAudio(p->device);
        p->pending.clear();
    }
    p->last_drain_ms = SDL_GetTicks();
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult play_GetPlayState(SLPlayItf self, SLuint32 *state)
{
    Player *p = ((Iface<SLPlayItf_, Player> *)self)->owner;
    if (!p || !state)
        return SL_RESULT_PARAMETER_INVALID;
    *state = p->state;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult play_GetDuration(SLPlayItf self, SLmillisecond *msec)
{
    /* A buffer-queue player has no duration; the spec's own answer for that is
     * SL_TIME_UNKNOWN. */
    if (msec)
        *msec = (SLmillisecond)0xFFFFFFFF;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult play_GetPosition(SLPlayItf self, SLmillisecond *msec)
{
    if (msec)
        *msec = 0;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult play_RegisterCallback(SLPlayItf self, slPlayCallback cb, void *ctx)
{
    /* Instrumented: the assumption that play events are never armed had not
     * actually been verified against the game. */
    (void)self; (void)ctx;
    trace("opensl: play_RegisterCallback cb=%p", (void *)cb);
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult play_SetCallbackEventsMask(SLPlayItf self, SLuint32 mask)
{
    (void)self;
    trace("opensl: play_SetCallbackEventsMask mask=0x%x", (unsigned)mask);
    return mask ? SL_RESULT_FEATURE_UNSUPPORTED : SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult play_GetCallbackEventsMask(SLPlayItf self, SLuint32 *mask)
{
    if (mask) *mask = 0;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult play_SetMarkerPosition(SLPlayItf self, SLmillisecond msec)
{
    (void)self; (void)msec;
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static ABI_ATTR SLresult play_ClearMarkerPosition(SLPlayItf self)
{
    (void)self;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult play_GetMarkerPosition(SLPlayItf self, SLmillisecond *msec)
{
    if (msec) *msec = 0;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult play_SetPositionUpdatePeriod(SLPlayItf self, SLmillisecond msec)
{
    (void)self; (void)msec;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult play_GetPositionUpdatePeriod(SLPlayItf self, SLmillisecond *msec)
{
    if (msec) *msec = 0;
    return SL_RESULT_SUCCESS;
}

static const SLPlayItf_ kPlayVt = {
    play_SetPlayState, play_GetPlayState, play_GetDuration, play_GetPosition,
    play_RegisterCallback, play_SetCallbackEventsMask, play_GetCallbackEventsMask,
    play_SetMarkerPosition, play_ClearMarkerPosition, play_GetMarkerPosition,
    play_SetPositionUpdatePeriod, play_GetPositionUpdatePeriod,
};

/* ------------------------------------------------------------------ *
 * SLVolumeItf
 *
 * OpenSL ES volume is attenuation in millibels (0 = full scale, negative =
 * quieter). SDL has no per-device gain, so this is remembered and applied
 * when the samples are mixed - which for now means it is remembered and the
 * device plays at its own level. Reporting the value back truthfully matters:
 * the game reads it to drive its own fades.
 * ------------------------------------------------------------------ */
static ABI_ATTR SLresult vol_SetVolumeLevel(SLVolumeItf self, SLmillibel level)
{
    Player *p = ((Iface<SLVolumeItf_, Player> *)self)->owner;
    if (!p)
        return SL_RESULT_PARAMETER_INVALID;
    p->level = level;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult vol_GetVolumeLevel(SLVolumeItf self, SLmillibel *level)
{
    Player *p = ((Iface<SLVolumeItf_, Player> *)self)->owner;
    if (!p || !level)
        return SL_RESULT_PARAMETER_INVALID;
    *level = p->level;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult vol_GetMaxVolumeLevel(SLVolumeItf self, SLmillibel *level)
{
    if (level) *level = 0;   /* 0 mB = no attenuation */
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult vol_SetMute(SLVolumeItf self, SLboolean mute)
{
    Player *p = ((Iface<SLVolumeItf_, Player> *)self)->owner;
    if (!p)
        return SL_RESULT_PARAMETER_INVALID;
    p->muted = mute != SL_BOOLEAN_FALSE;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult vol_GetMute(SLVolumeItf self, SLboolean *mute)
{
    Player *p = ((Iface<SLVolumeItf_, Player> *)self)->owner;
    if (!p || !mute)
        return SL_RESULT_PARAMETER_INVALID;
    *mute = p->muted ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult vol_EnableStereoPosition(SLVolumeItf self, SLboolean enable)
{
    (void)self; (void)enable;
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static ABI_ATTR SLresult vol_IsEnableStereoPosition(SLVolumeItf self, SLboolean *enable)
{
    if (enable) *enable = SL_BOOLEAN_FALSE;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult vol_SetStereoPosition(SLVolumeItf self, SLpermille pos)
{
    (void)self; (void)pos;
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static ABI_ATTR SLresult vol_GetStereoPosition(SLVolumeItf self, SLpermille *pos)
{
    if (pos) *pos = 0;
    return SL_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ *
 * SLSeekItf
 *
 * Only the music player is ever seeked, and the game only ever uses it to say
 * "loop this track forever" right after starting playback. The state is kept
 * and reported back honestly; with no decoder behind the music player there is
 * no position to move.
 * ------------------------------------------------------------------ */
static ABI_ATTR SLresult seek_SetPosition(SLSeekItf self, SLmillisecond pos, SLuint32 mode)
{
    (void)self; (void)pos; (void)mode;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult seek_SetLoop(SLSeekItf self, SLboolean enable,
                                      SLmillisecond start, SLmillisecond end)
{
    (void)start; (void)end;
    Player *p = ((Iface<SLSeekItf_, Player> *)self)->owner;
    if (!p)
        return SL_RESULT_PARAMETER_INVALID;
    p->looping = enable != SL_BOOLEAN_FALSE;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult seek_GetLoop(SLSeekItf self, SLboolean *enable,
                                      SLmillisecond *start, SLmillisecond *end)
{
    Player *p = ((Iface<SLSeekItf_, Player> *)self)->owner;
    if (!p)
        return SL_RESULT_PARAMETER_INVALID;
    if (enable) *enable = p->looping ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
    if (start)  *start  = 0;
    if (end)    *end    = (SLmillisecond)0xFFFFFFFF;
    return SL_RESULT_SUCCESS;
}

static const SLSeekItf_ kSeekVt = {
    seek_SetPosition, seek_SetLoop, seek_GetLoop,
};

static const SLVolumeItf_ kVolumeVt = {
    vol_SetVolumeLevel, vol_GetVolumeLevel, vol_GetMaxVolumeLevel,
    vol_SetMute, vol_GetMute,
    vol_EnableStereoPosition, vol_IsEnableStereoPosition,
    vol_SetStereoPosition, vol_GetStereoPosition,
};

/* ------------------------------------------------------------------ *
 * The player object itself
 * ------------------------------------------------------------------ */
static ABI_ATTR SLresult player_Realize(SLObjectItf self, SLboolean async)
{
    (void)async;
    Player *p = (Player *)self;
    if (!p)
        return SL_RESULT_PARAMETER_INVALID;

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq     = p->freq;
    want.format   = (p->bits == 8) ? AUDIO_U8 : AUDIO_S16LSB;
    want.channels = (Uint8)p->channels;
    want.samples  = 1024;
    want.callback = NULL;            /* queue-driven, like the game's own model */

    if (g_audio_up && p->decodes) {
        /*
         * On the R36S the default device comes back "Device or resource busy":
         * something else - EmulationStation's menu music is the candidate -
         * holds it, and ALSA will not hand out a second exclusive handle. So
         * try the default first, then the devices SDL enumerates, and say
         * which one worked so the next log answers this instead of guessing.
         *
         * DEADSPACE_AUDIODEV overrides the whole search with one name.
         */
        const char *forced = getenv("DEADSPACE_AUDIODEV");

        if (forced && *forced) {
            p->device = SDL_OpenAudioDevice(forced, 0, &want, &have,
                                            SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
            if (!p->device)
                warning("OpenSL ES: DEADSPACE_AUDIODEV=%s failed: %s\n",
                        forced, SDL_GetError());
        } else {
            p->device = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                            SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);

            if (!p->device) {
                /*
                 * "The first device that opens" sends the game out the wrong hole.
                 *
                 * On some CFWs the handheld's own speaker answers "Device or
                 * resource busy" for a second or two after launch, because the
                 * frontend has not let go of the card yet - while HDMI is free
                 * and opens immediately. Taking the first one that opens leaves
                 * the player in silence, with the sound going out a port that
                 * has nothing plugged into it.
                 *
                 * First pass therefore skips HDMI and waits out a busy speaker;
                 * second pass accepts anything rather than leave the game mute.
                 *
                 * Seen on muOS and diagnosed by NextOs-Ports (sonic4ep2-nextos).
                 * This device is unaffected - its default opens - so
                 * DEADSPACE_NO_PREFER_SPEAKER restores the old behaviour if the
                 * heuristic ever guesses wrong somewhere else.
                 */
                const char *first_err = SDL_GetError();
                int n = SDL_GetNumAudioDevices(0);
                bool prefer_speaker = (getenv("DEADSPACE_NO_PREFER_SPEAKER") == NULL);

                for (int pass = 0; pass < (prefer_speaker ? 2 : 1) && !p->device; pass++) {
                    for (int i = 0; i < n && !p->device; i++) {
                        const char *name = SDL_GetAudioDeviceName(i, 0);
                        if (!name || !*name)
                            continue;

                        bool is_hdmi = (strcasestr(name, "hdmi") != NULL);
                        if (prefer_speaker && pass == 0 && is_hdmi) {
                            trace("OpenSL ES: skipping \"%s\" on the first pass (HDMI)",
                                  name);
                            continue;
                        }

                        /* Only the first pass waits - it is the one hoping the
                         * speaker frees up. */
                        int tries = (prefer_speaker && pass == 0) ? 5 : 1;
                        for (int t = 0; t < tries; t++) {
                            p->device = SDL_OpenAudioDevice(name, 0, &want, &have,
                                                            SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
                            if (p->device)
                                break;
                            const char *err = SDL_GetError();
                            if (!err || !strcasestr(err, "busy") || t + 1 >= tries)
                                break;
                            trace("OpenSL ES: \"%s\" busy, retry %d/%d", name, t + 1, tries);
                            SDL_Delay(400);
                        }

                        if (p->device)
                            trace("OpenSL ES: default was unavailable (%s), opened "
                                  "\"%s\" on pass %d", first_err, name, pass + 1);
                    }
                }

                if (!p->device)
                    warning("OpenSL ES: no audio device for %d Hz %d ch: %s "
                            "(%d alternative device(s) also refused)\n",
                            p->freq, p->channels, first_err, n);
            }
        }
    }

    p->realized      = true;
    p->last_drain_ms = SDL_GetTicks();
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult player_Resume(SLObjectItf self, SLboolean async)
{
    (void)self; (void)async;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult player_GetState(SLObjectItf self, SLuint32 *state)
{
    Player *p = (Player *)self;
    if (!p || !state)
        return SL_RESULT_PARAMETER_INVALID;
    *state = p->realized ? SL_OBJECT_STATE_REALIZED : SL_OBJECT_STATE_UNREALIZED;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult player_GetInterface(SLObjectItf self, const SLInterfaceID iid, void *iface)
{
    Player *p = (Player *)self;
    if (!p || !iid || !iface)
        return SL_RESULT_PARAMETER_INVALID;

    if (iid == SL_IID_PLAY)        { *(void **)iface = &p->play;   return SL_RESULT_SUCCESS; }
    if (iid == SL_IID_VOLUME)      { *(void **)iface = &p->volume; return SL_RESULT_SUCCESS; }
    if (iid == SL_IID_BUFFERQUEUE) { *(void **)iface = &p->queue;  return SL_RESULT_SUCCESS; }
    if (iid == SL_IID_SEEK)        { *(void **)iface = &p->seek;   return SL_RESULT_SUCCESS; }

    /* Playback rate, effect send, dynamic interface management: this port has
     * none of them, and the game only asks opportunistically - it checks the
     * result and skips the feature. */
    *(void **)iface = NULL;
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static ABI_ATTR SLresult player_RegisterCallback(SLObjectItf self, slObjectCallback cb, void *ctx)
{
    (void)self; (void)cb; (void)ctx;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR void player_AbortAsyncOperation(SLObjectItf self)
{
    (void)self;
}

static ABI_ATTR void player_Destroy(SLObjectItf self)
{
    Player *p = (Player *)self;
    if (!p)
        return;

    if (p->device)
        SDL_CloseAudioDevice(p->device);

    for (Player **it = &g_players; *it; it = &(*it)->next) {
        if (*it == p) {
            *it = p->next;
            break;
        }
    }
    delete p;
}

static ABI_ATTR SLresult player_SetPriority(SLObjectItf self, SLint32 priority, SLboolean preemptable)
{
    (void)self; (void)priority; (void)preemptable;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult player_GetPriority(SLObjectItf self, SLint32 *priority)
{
    if (priority) *priority = 0;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult player_SetLossOfControl(SLObjectItf self, SLint16 num,
                                                 SLInterfaceID *ids, SLboolean enabled)
{
    (void)self; (void)num; (void)ids; (void)enabled;
    return SL_RESULT_SUCCESS;
}

static const SLObjectItf_ kPlayerObjectVt = {
    player_Realize, player_Resume, player_GetState, player_GetInterface,
    player_RegisterCallback, player_AbortAsyncOperation, player_Destroy,
    player_SetPriority, player_GetPriority, player_SetLossOfControl,
};

/* ------------------------------------------------------------------ *
 * The output mix. It has no interfaces the game uses: on Android it exists
 * only to be named as the sink of every player.
 * ------------------------------------------------------------------ */
static ABI_ATTR SLresult mix_Realize(SLObjectItf self, SLboolean async)
{
    (void)async;
    OutputMix *m = (OutputMix *)self;
    if (!m)
        return SL_RESULT_PARAMETER_INVALID;
    m->realized = true;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult mix_Resume(SLObjectItf self, SLboolean async)
{
    (void)self; (void)async;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult mix_GetState(SLObjectItf self, SLuint32 *state)
{
    OutputMix *m = (OutputMix *)self;
    if (!m || !state)
        return SL_RESULT_PARAMETER_INVALID;
    *state = m->realized ? SL_OBJECT_STATE_REALIZED : SL_OBJECT_STATE_UNREALIZED;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult mix_GetInterface(SLObjectItf self, const SLInterfaceID iid, void *iface)
{
    (void)self; (void)iid;
    if (iface)
        *(void **)iface = NULL;
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static ABI_ATTR SLresult mix_RegisterCallback(SLObjectItf self, slObjectCallback cb, void *ctx)
{
    (void)self; (void)cb; (void)ctx;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR void mix_AbortAsyncOperation(SLObjectItf self) { (void)self; }

static ABI_ATTR void mix_Destroy(SLObjectItf self)
{
    delete (OutputMix *)self;
}

static ABI_ATTR SLresult mix_SetPriority(SLObjectItf self, SLint32 priority, SLboolean preemptable)
{
    (void)self; (void)priority; (void)preemptable;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult mix_GetPriority(SLObjectItf self, SLint32 *priority)
{
    if (priority) *priority = 0;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult mix_SetLossOfControl(SLObjectItf self, SLint16 num,
                                              SLInterfaceID *ids, SLboolean enabled)
{
    (void)self; (void)num; (void)ids; (void)enabled;
    return SL_RESULT_SUCCESS;
}

static const SLObjectItf_ kMixObjectVt = {
    mix_Realize, mix_Resume, mix_GetState, mix_GetInterface,
    mix_RegisterCallback, mix_AbortAsyncOperation, mix_Destroy,
    mix_SetPriority, mix_GetPriority, mix_SetLossOfControl,
};

/* ------------------------------------------------------------------ *
 * SLEngineItf
 * ------------------------------------------------------------------ */
static ABI_ATTR SLresult eng_CreateAudioPlayer(SLEngineItf self, SLObjectItf *player,
                                               SLDataSource *source, SLDataSink *sink,
                                               SLuint32 num_ifaces,
                                               const SLInterfaceID *ids,
                                               const SLboolean *required)
{
    (void)self; (void)sink; (void)num_ifaces; (void)ids; (void)required;

    if (!player || !source || !source->pFormat)
        return SL_RESULT_PARAMETER_INVALID;

    const SLDataFormat_PCM *pcm = (const SLDataFormat_PCM *)source->pFormat;

    /*
     * Two kinds of player, and only one of them is ours to fill.
     *
     * The sound effects arrive as SL_DATAFORMAT_PCM through a buffer queue:
     * the game decodes its own Ogg Vorbis and hands us finished samples, so
     * those play for real.
     *
     * The other kind arrives as SL_DATAFORMAT_MIME - a file the *platform* is
     * expected to decode, which on Android is Stagefright and here is nobody.
     * That player is created but produces nothing. Refusing instead is not an
     * option: the game does not survive it. xt::SoundSystem::playMusic calls
     * Channel::play() unconditionally, and Channel::play() dereferences the
     * SLPlayItf it never got:
     *
     *     ldr r0, [r0, #20]   ; channel->playItf, null if creation failed
     *     ldr r1, [r0]        ; <- SIGSEGV
     *
     * This used to announce "Music will be silent", and that claim went into a
     * published release before anyone checked it against the device. It was
     * wrong: music plays. The game does not depend on this player, and reaches
     * its soundtrack through the PCM path like everything else. A message about
     * an unused code path is not a statement about what the player hears, and
     * writing it as though it were cost a correction in public.
     */
    const bool is_pcm = (pcm->formatType == SL_DATAFORMAT_PCM);
    if (!is_pcm) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            warning("OpenSL ES: a player was asked for with a compressed "
                    "stream (format %u) for the platform to decode, and this "
                    "port has no decoder for that path, so it stays empty. "
                    "This is not the audio you hear: sound and music both go "
                    "through the PCM path and are unaffected.\n",
                    (unsigned)pcm->formatType);
        }
    }

    Player *p = new Player();
    p->vt = &kPlayerObjectVt;
    p->play   = { &kPlayVt,        p };
    p->volume = { &kVolumeVt,      p };
    p->queue  = { &kBufferQueueVt, p };
    p->seek   = { &kSeekVt,        p };

    p->decodes = is_pcm;

    if (is_pcm) {
        p->channels = (int)pcm->numChannels;
        /* samplesPerSec is milliHz in the specification, and the game does
         * obey that: it passes 22050000 for 22.05 kHz. */
        p->freq     = (int)(pcm->samplesPerSec / 1000);
        p->bits     = (int)pcm->bitsPerSample;
    } else {
        /* Nothing describes the stream until it is decoded; these only keep
         * the queue arithmetic well defined. */
        p->channels = 2;
        p->freq     = 44100;
        p->bits     = 16;
    }

    if (p->channels < 1) p->channels = 1;
    if (p->freq < 4000)  p->freq     = 44100;
    if (p->bits != 8)    p->bits     = 16;

    p->device        = 0;
    p->state         = SL_PLAYSTATE_STOPPED;
    p->level         = 0;
    p->muted         = false;
    p->realized      = false;
    p->looping       = false;
    p->callback      = NULL;
    p->callback_ctx  = NULL;
    p->last_drain_ms = SDL_GetTicks();
    p->carry_bytes   = 0.0;

    p->next   = g_players;
    g_players = p;

    *player = (SLObjectItf)p;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult eng_CreateOutputMix(SLEngineItf self, SLObjectItf *mix,
                                             SLuint32 num_ifaces, const SLInterfaceID *ids,
                                             const SLboolean *required)
{
    (void)self; (void)num_ifaces; (void)ids; (void)required;
    if (!mix)
        return SL_RESULT_PARAMETER_INVALID;

    OutputMix *m = new OutputMix();
    m->vt       = &kMixObjectVt;
    m->realized = false;
    *mix = (SLObjectItf)m;
    return SL_RESULT_SUCCESS;
}

/*
 * The rest of the engine.
 *
 * The game never asks for a MIDI player or a 3D listener, but the slots cannot
 * be left null: a null slot turns "the game called something unexpected" into
 * a jump to zero. Each one is a real function with that slot's own signature
 * which says the feature is unsupported - the answer a conformant engine
 * without the feature gives.
 */
template <typename... A>
static ABI_ATTR SLresult eng_unsupported(A...)
{
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static const SLEngineItf_ kEngineVt = {
    eng_unsupported<SLEngineItf, SLObjectItf *, SLuint32, SLuint32, const SLInterfaceID *, const SLboolean *>,
    eng_unsupported<SLEngineItf, SLObjectItf *, SLuint32, SLuint32, const SLInterfaceID *, const SLboolean *>,
    eng_CreateAudioPlayer,
    eng_unsupported<SLEngineItf, SLObjectItf *, SLDataSource *, SLDataSink *, SLuint32, const SLInterfaceID *, const SLboolean *>,
    eng_unsupported<SLEngineItf, SLObjectItf *, SLDataSource *, SLDataSource *, SLDataSink *, SLDataSink *, SLuint32, const SLInterfaceID *, const SLboolean *>,
    eng_unsupported<SLEngineItf, SLObjectItf *, SLuint32, const SLInterfaceID *, const SLboolean *>,
    eng_unsupported<SLEngineItf, SLObjectItf *, SLuint32, const SLInterfaceID *, const SLboolean *>,
    eng_CreateOutputMix,
    eng_unsupported<SLEngineItf, SLObjectItf *, SLDataSource *, SLuint32, const SLInterfaceID *, const SLboolean *>,
    eng_unsupported<SLEngineItf, SLObjectItf *, void *, SLuint32, SLuint32, const SLInterfaceID *, const SLboolean *>,
    eng_unsupported<SLEngineItf, SLuint32, SLuint32 *>,
    eng_unsupported<SLEngineItf, SLuint32, SLuint32, SLInterfaceID *>,
    eng_unsupported<SLEngineItf, SLuint32 *>,
    eng_unsupported<SLEngineItf, SLuint32, char *, SLint16 *>,
    eng_unsupported<SLEngineItf, const char *, SLboolean *>,
};

/* ------------------------------------------------------------------ *
 * The engine object
 * ------------------------------------------------------------------ */
static ABI_ATTR SLresult engine_Realize(SLObjectItf self, SLboolean async)
{
    (void)async;
    Engine *e = (Engine *)self;
    if (!e)
        return SL_RESULT_PARAMETER_INVALID;

    if (!g_audio_tried) {
        g_audio_tried = true;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
            g_audio_up = true;
            trace("audio: SDL driver '%s'", SDL_GetCurrentAudioDriver());
        } else {
            warning("OpenSL ES: no audio backend (%s); the game will run "
                    "without sound.\n", SDL_GetError());
        }
    }

    e->realized = true;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult engine_Resume(SLObjectItf self, SLboolean async)
{
    (void)self; (void)async;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult engine_GetState(SLObjectItf self, SLuint32 *state)
{
    Engine *e = (Engine *)self;
    if (!e || !state)
        return SL_RESULT_PARAMETER_INVALID;
    *state = e->realized ? SL_OBJECT_STATE_REALIZED : SL_OBJECT_STATE_UNREALIZED;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult engine_GetInterface(SLObjectItf self, const SLInterfaceID iid, void *iface)
{
    Engine *e = (Engine *)self;
    if (!e || !iid || !iface)
        return SL_RESULT_PARAMETER_INVALID;

    if (iid == SL_IID_ENGINE) {
        *(void **)iface = &e->engine;
        return SL_RESULT_SUCCESS;
    }

    *(void **)iface = NULL;
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static ABI_ATTR SLresult engine_RegisterCallback(SLObjectItf self, slObjectCallback cb, void *ctx)
{
    (void)self; (void)cb; (void)ctx;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR void engine_AbortAsyncOperation(SLObjectItf self) { (void)self; }

static ABI_ATTR void engine_Destroy(SLObjectItf self)
{
    delete (Engine *)self;
}

static ABI_ATTR SLresult engine_SetPriority(SLObjectItf self, SLint32 priority, SLboolean preemptable)
{
    (void)self; (void)priority; (void)preemptable;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult engine_GetPriority(SLObjectItf self, SLint32 *priority)
{
    if (priority) *priority = 0;
    return SL_RESULT_SUCCESS;
}

static ABI_ATTR SLresult engine_SetLossOfControl(SLObjectItf self, SLint16 num,
                                                 SLInterfaceID *ids, SLboolean enabled)
{
    (void)self; (void)num; (void)ids; (void)enabled;
    return SL_RESULT_SUCCESS;
}

static const SLObjectItf_ kEngineObjectVt = {
    engine_Realize, engine_Resume, engine_GetState, engine_GetInterface,
    engine_RegisterCallback, engine_AbortAsyncOperation, engine_Destroy,
    engine_SetPriority, engine_GetPriority, engine_SetLossOfControl,
};

extern "C" SLresult slCreateEngine(void **engine, SLuint32 numOptions, const void *options,
                                   SLuint32 numInterfaces, const SLInterfaceID *interfaceIds,
                                   const void *interfaceRequired)
{
    (void)numOptions; (void)options; (void)numInterfaces;
    (void)interfaceIds; (void)interfaceRequired;

    if (!engine)
        return SL_RESULT_PARAMETER_INVALID;

    Engine *e = new Engine();
    e->vt       = &kEngineObjectVt;
    e->engine   = { &kEngineVt, e };
    e->realized = false;

    *engine = e;
    return SL_RESULT_SUCCESS;
}

/*
 * Retire buffers the device has finished with and call the game back for each
 * one, because its mixer only ever refills from that callback.
 *
 * This runs on the game's own thread, from inside ALooper_pollAll, rather than
 * on SDL's audio thread: the callback re-enters the buffer queue and the
 * engine's mixer state, none of which is thread-safe here. Android delivers it
 * on a dedicated thread, but the game treats it as "the mixer tick", and
 * running it where the mixer already runs is the safe reading.
 */
extern "C" void android_opensles_pump(void)
{
    uint32_t now = SDL_GetTicks();

    for (Player *p = g_players; p; p = p->next) {
        if (p->pending.empty() || !p->callback)
            continue;

        uint32_t elapsed = now - p->last_drain_ms;
        p->last_drain_ms = now;

        SLuint32 done = 0;

        if (p->device) {
            /* The device is the authority: whatever it has not consumed is
             * still queued, so everything else has been played. */
            uint32_t left = SDL_GetQueuedAudioSize(p->device);
            uint32_t held = 0;
            for (SLuint32 sz : p->pending)
                held += sz;
            uint32_t played = (held > left) ? held - left : 0;
            while (done < p->pending.size() && p->pending[done] <= played) {
                played -= p->pending[done];
                done++;
            }
        } else if (p->state == SL_PLAYSTATE_PLAYING) {
            /* No device: pace the queue off the clock so the game's mixer
             * still advances at roughly the rate the audio would have. */
            p->carry_bytes += bytes_per_ms(p) * (double)elapsed;
            while (done < p->pending.size() && p->carry_bytes >= (double)p->pending[done]) {
                p->carry_bytes -= (double)p->pending[done];
                done++;
            }
        }

        for (SLuint32 i = 0; i < done; i++)
            p->pending.pop_front();

        /* One callback per tick, as Android does: the game enqueues exactly
         * one buffer per callback. */
        if (done > 0)
            p->callback((SLBufferQueueItf)&p->queue, p->callback_ctx);
    }
}

/*
 * The table the loader binds into libdeadspace.so for its libOpenSLES.so
 * imports.
 */
#include "so_util.h"
#include "thunk_gen.h"

/* The SL_IID_* entries are data, not code: the game's relocation has to land
 * on the storage that holds the pointer, so they are exported by address with
 * no thunk. */
#define SL_IID(name) { #name, (uintptr_t)&name }

DynLibFunction symtable_opensles[] = {
    THUNK_DIRECT(slCreateEngine),
    SL_IID(SL_IID_ENGINE),
    SL_IID(SL_IID_PLAY),
    SL_IID(SL_IID_SEEK),
    SL_IID(SL_IID_VOLUME),
    SL_IID(SL_IID_PLAYBACKRATE),
    SL_IID(SL_IID_EFFECTSEND),
    SL_IID(SL_IID_DYNAMICINTERFACEMANAGEMENT),
    SL_IID(SL_IID_BUFFERQUEUE),
    { NULL, 0 },
};
