#include <algorithm>
#include <stdint.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <vector>
#include <SDL2/SDL.h>

#include "platform.h"
#include "jni.h"
#include "jni_internals.h"
#include "trace.h"
#include "media_AudioTrack.h"

static int GetSDLFormat(int audioFormat)
{
    switch (audioFormat) {
    case ENCODING_PCM_16BIT: return AUDIO_S16;
    case ENCODING_PCM_8BIT: return AUDIO_U8;
    case ENCODING_PCM_FLOAT: return AUDIO_F32SYS;
    default: return AUDIO_U8;
    }
}

static int GetSDLFormatBytes(int audioFormat)
{
    switch (audioFormat) {
    case ENCODING_PCM_16BIT: return 2;
    case ENCODING_PCM_8BIT: return 1;
    case ENCODING_PCM_FLOAT: return 4;
    default: return 1;
    }
}

static SDL_AudioDeviceID OpenAudioOutput(const SDL_AudioSpec *desired,
                                         SDL_AudioSpec *obtained)
{
    const char *forced = getenv("DEADSPACE_AUDIODEV");
    if (forced && *forced) {
        SDL_AudioDeviceID device =
            SDL_OpenAudioDevice(forced, 0, desired, obtained, 0);
        if (!device)
            warning("AudioTrack: DEADSPACE_AUDIODEV=%s failed: %s\n",
                    forced, SDL_GetError());
        return device;
    }

    SDL_AudioDeviceID device =
        SDL_OpenAudioDevice(NULL, 0, desired, obtained, 0);
    if (device)
        return device;

    /*
     * Some PortMaster frontends keep the speaker PCM busy briefly after
     * launching a port, while an unused HDMI endpoint opens immediately.
     * Prefer a non-HDMI enumerated device and wait out "busy" before accepting
     * any remaining endpoint as a last resort.
     */
    const char *error = SDL_GetError();
    std::string default_error = error ? error : "unknown error";
    int count = SDL_GetNumAudioDevices(0);
    for (int pass = 0; pass < 2 && !device; pass++) {
        for (int i = 0; i < count && !device; i++) {
            const char *name = SDL_GetAudioDeviceName(i, 0);
            if (!name || !*name)
                continue;
            bool hdmi = strcasestr(name, "hdmi") != NULL;
            if (pass == 0 && hdmi)
                continue;

            int attempts = pass == 0 ? 5 : 1;
            for (int attempt = 0; attempt < attempts; attempt++) {
                device = SDL_OpenAudioDevice(name, 0, desired, obtained, 0);
                if (device)
                    break;
                const char *attempt_error = SDL_GetError();
                if (!attempt_error || !strcasestr(attempt_error, "busy") ||
                    attempt + 1 >= attempts)
                    break;
                SDL_Delay(400);
            }
            if (device)
                trace("AudioTrack: default output failed (%s); opened \"%s\" "
                      "on pass %d", default_error.c_str(), name, pass + 1);
        }
    }

    if (!device)
        warning("AudioTrack: no output accepted %d Hz/%u ch/0x%x: %s "
                "(%d enumerated device(s))\n",
                desired->freq, (unsigned int)desired->channels,
                (unsigned int)desired->format, default_error.c_str(), count);
    return device;
}

AudioTrack::AudioTrack(int streamType, int sampleRateInHz, int channelConfig, int audioFormat, int bufferSizeInBytes, int mode)
{
    SDL_zero(desired);
    SDL_zero(obtained);
    desired.freq = sampleRateInHz;
    desired.format = GetSDLFormat(audioFormat);
    desired.channels = (channelConfig == 4) ? 1 : 2;
    /*
     * bufferSizeInBytes is the engine's 1 MiB producer ring, not the hardware
     * period. Passing it through made SDL request a 262144-frame (six-second)
     * device buffer. Queue-driven playback only needs a conventional period.
     */
    desired.samples = 1024;
    desired.callback = NULL;
    playing = 0;
    deviceId = 0;
    needed_bytes = bufferSizeInBytes;
    this->mode = mode;

    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) &&
        SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        warning("AudioTrack: SDL audio subsystem failed to initialize: %s\n",
                SDL_GetError());
        return;
    }

    const char *driver = SDL_GetCurrentAudioDriver();
    int outputs = SDL_GetNumAudioDevices(0);
    trace("AudioTrack: SDL audio ready driver=%s outputs=%d request=%d Hz/%u "
          "ch/0x%x period=%u",
          driver ? driver : "(none)", outputs, desired.freq,
          (unsigned int)desired.channels, (unsigned int)desired.format,
          (unsigned int)desired.samples);
    for (int i = 0; i < outputs && i < 8; i++) {
        const char *name = SDL_GetAudioDeviceName(i, 0);
        trace("AudioTrack: output[%d]=%s", i, name ? name : "(null)");
    }

    deviceId = OpenAudioOutput(&desired, &obtained);
    if (deviceId) {
        trace("AudioTrack: opened device=%u obtained=%d Hz/%u ch/0x%x "
              "period=%u",
              (unsigned int)deviceId, obtained.freq,
              (unsigned int)obtained.channels, (unsigned int)obtained.format,
              (unsigned int)obtained.samples);
    }
}

static void AudioClass_ctor1(JNIEnv *env, jobject obj, jclass clazz, int streamType, int sampleRateInHz, int channelConfig, int audioFormat, int bufferSizeInBytes, int mode)
{
    AudioTrack *track = new (obj) AudioTrack(streamType, sampleRateInHz, channelConfig, audioFormat, bufferSizeInBytes, mode);
}

int AudioTrack::getMinBufferSize(JNIEnv *env, jclass clazz, int sampleRateInHz, int channelConfig, int audioFormat)
{
    return 2048 * GetSDLFormatBytes(audioFormat);
}

void AudioTrack::play(JNIEnv *env, jobject obj, jclass clazz)
{
    AudioTrack *track = (AudioTrack*)obj;
    track->playing = 1;
    SDL_PauseAudioDevice(track->deviceId, 0);
}

void AudioTrack::stop(JNIEnv *env, jobject obj, jclass clazz)
{
    AudioTrack *track = (AudioTrack*)obj;
    track->playing = 0;
    SDL_PauseAudioDevice(track->deviceId, 1);
}

void AudioTrack::release(JNIEnv *env, jobject obj, jclass clazz)
{
    AudioTrack *track = (AudioTrack*)obj;
    SDL_ClearQueuedAudio(track->deviceId);
}

int AudioTrack::write(JNIEnv *env, jobject obj, jclass clazz, jbyteArray audioData, int offsetInBytes, int sizeInBytes)
{
    return AudioTrack::write(env, obj, clazz, audioData, offsetInBytes, sizeInBytes, WRITE_BLOCKING);
}


int AudioTrack::write(JNIEnv *env, jobject obj, jclass clazz, jbyteArray audioData, int offsetInBytes, int sizeInBytes, int writeMode)
{
    Class *clz = (Class*)clazz;
    AudioTrack *track = (AudioTrack*)obj;
    ArrayObject *data = (ArrayObject*)audioData;
    uintptr_t where = (uintptr_t)data->elements + offsetInBytes;

    int ret = SDL_QueueAudio(track->deviceId, (void*)where, sizeInBytes);

    if (track->playing == 0)
        AudioTrack::play(env, obj, clazz);

    if (writeMode == WRITE_BLOCKING) {
        do {
            SDL_Delay(0);
        } while (SDL_GetQueuedAudioSize(track->deviceId));
    }

    if (ret == 0)
        return sizeInBytes;
    else
        return 0;
}

/*
 * The 16-bit overload, which this engine is the first to ask for.
 *
 * AudioTrack.write has a byte[] form and a short[] form, and Android treats the
 * count argument differently in each: bytes for one, *samples* for the other.
 * Dead Space uses the short[] form - the run log said so the moment the audio
 * core came up:
 *
 *     Class AudioClass does not have method write([SII)I
 *
 * and a missing method here is not a silent no-op. GetMethodID returns NULL, the
 * engine writes into nothing, and the mixer either stalls waiting for the queue
 * to drain or spins feeding a device that never consumes.
 *
 * The conversion is the whole point of a separate entry rather than an alias:
 * SDL_QueueAudio counts bytes, so the sample count doubles. Registering this
 * signature against the byte[] implementation would have queued half the audio
 * and produced a stutter that sounds like a performance problem.
 */
/*
 * Per-write signal metrics plus a running digest of every sample the mixer has
 * produced.
 *
 * The digest is what makes the VFP short-vector expansion testable. qemu-arm
 * still implements FPSCR LEN/STRIDE, so the same mixer can be run twice - once
 * with the trampolines installed and once with DEADSPACE_NO_VFP_PATCH=1 - and
 * the two digests must agree exactly. That turns "the scalar expansion is
 * arithmetically equivalent" from a claim about hand-decoded opcodes into a
 * comparison the harness can make, on a machine that has no ARMv8 handheld
 * attached.
 *
 * Reported at a fixed set of write indices rather than a time interval,
 * because the comparison only means something if both runs report after the
 * same number of samples.
 */
static void report_pcm(const int16_t *samples, int count)
{
    static unsigned int write_index  = 0;
    static uint64_t     digest       = 1469598103934665603ULL;  /* FNV-1a */
    static uint64_t     total_frames = 0;
    static bool         saw_signal   = false;

    unsigned int nonzero = 0;
    unsigned int peak = 0;
    uint64_t square_sum = 0;

    for (int i = 0; i < count; i++) {
        int sample = samples[i];
        unsigned int magnitude =
            sample < 0 ? (unsigned int)(-(int64_t)sample)
                       : (unsigned int)sample;
        if (magnitude)
            nonzero++;
        peak = std::max(peak, magnitude);
        square_sum += (uint64_t)magnitude * magnitude;

        digest = (digest ^ (uint16_t)sample) * 1099511628211ULL;
    }

    write_index++;
    total_frames += (uint64_t)count;

    bool first_signal = nonzero && !saw_signal;
    saw_signal = saw_signal || nonzero;

    /* Powers of two keep the log bounded over a long run while still giving
     * several comparison points early, where a divergence would appear. */
    bool milestone = (write_index & (write_index - 1)) == 0;
    if (write_index <= 4 || first_signal || milestone) {
        trace("AudioTrack: PCM write=%u samples=%d nonzero=%u peak=%u "
              "mean_square=%llu total=%llu digest=%016llx",
              write_index, count, nonzero, peak,
              (unsigned long long)(count ?
                  square_sum / (unsigned int)count : 0),
              (unsigned long long)total_frames,
              (unsigned long long)digest);
    }
}

int AudioTrack::write_shorts(JNIEnv *env, jobject obj,
                             jshortArray audioData, int offsetInShorts,
                             int sizeInShorts)
{
    AudioTrack  *track = (AudioTrack *)obj;
    ArrayObject *data  = (ArrayObject *)audioData;

    /* Defensive, and instrumented, because the first run of this path arrived
     * with a jshortArray of 0x83469cb5 - not a pointer this loader ever handed
     * out. Whether the engine passes something unexpected or the dispatcher
     * reads the wrong slot is not answerable by reading code, so both are
     * printed once. */
    static bool announced = false;
    if (!announced) {
        announced = true;
        trace("AudioTrack.write(short[]): track=%p data=%p offset=%d count=%d",
              (void *)track, (void *)data, offsetInShorts, sizeInShorts);
    }

    if (!track || !data)
        return 0;

    /* An ArrayObject this loader allocated always has a non-null elements
     * pointer and an element_size of 2 for a short[]. Anything else is not one
     * of ours, and dereferencing it is how this crashed. */
    if (data->element_size != (jsize)sizeof(short) || !data->elements) {
        warning("AudioTrack.write(short[]): array at %p is not a short[] this "
                "loader allocated (element_size=%d, elements=%p) - dropping the "
                "write rather than following the pointer.\n",
                (void *)data, (int)data->element_size, data->elements);
        return 0;
    }

    if (offsetInShorts < 0 || sizeInShorts < 0 ||
        (size_t)offsetInShorts + (size_t)sizeInShorts >
            (size_t)data->count) {
        warning("AudioTrack.write(short[]): range offset=%d count=%d exceeds "
                "array length=%d - dropping write\n",
                offsetInShorts, sizeInShorts, (int)data->count);
        return 0;
    }

    uintptr_t where = (uintptr_t)data->elements + (size_t)offsetInShorts * 2;
    int       bytes = sizeInShorts * 2;

    /*
     * Measure the PCM *before* the output device is consulted, and deliberately
     * so.
     *
     * The build container has no ALSA endpoint at all, so on the verification
     * path deviceId is always zero. Reporting after an early return meant the
     * one run that executes on every commit produced no signal telemetry
     * whatsoever - and signal telemetry is the only evidence available for the
     * VFP short-vector expansion, which cannot be exercised any other way
     * without the physical console.
     *
     * Silence and "no endpoint" are different failures and the log has to keep
     * them apart. No samples or proprietary assets leave the process.
     */
    report_pcm((const int16_t *)where, sizeInShorts);

    if (!track->deviceId) {
        static bool warned_no_device = false;
        if (!warned_no_device) {
            warned_no_device = true;
            warning("AudioTrack.write(short[]): no SDL output device; PCM "
                    "cannot be played\n");
        }
        return 0;
    }

    int ret = SDL_QueueAudio(track->deviceId, (void *)where, bytes);
    if (ret != 0)
        warning("AudioTrack.write(short[]): SDL_QueueAudio failed: %s\n",
                SDL_GetError());

    if (track->playing == 0)
        AudioTrack::play(env, obj, (jclass)&AudioTrack::clazz);

    /*
     * Android's blocking write waits for buffer space, not for the speaker to
     * drain completely. Keep two SDL periods queued: waiting for zero between
     * every 512-frame game block created an audible scheduling gap.
     */
    unsigned int bytes_per_frame =
        (SDL_AUDIO_BITSIZE(track->obtained.format) / 8) *
        track->obtained.channels;
    unsigned int queue_high_water =
        track->obtained.samples * bytes_per_frame * 2;
    while (SDL_GetQueuedAudioSize(track->deviceId) > queue_high_water)
        SDL_Delay(1);

    return ret == 0 ? sizeInShorts : 0;
}

const FieldId mediaAudioTrackClassFields[] = {
    {NULL},
};

const ManagedMethod mediaAudioTrackClassMethods[] = {
    REGISTER_INIT_METHOD(AudioTrack, AudioClass_ctor1, "(IIIIII)V"),
    REGISTER_STATIC_METHOD(AudioTrack, getMinBufferSize, "(III)I"),
    REGISTER_NONVIRTUAL(AudioTrack, play   , "()V"),
    REGISTER_NONVIRTUAL(AudioTrack, stop   , "()V"),
    REGISTER_NONVIRTUAL(AudioTrack, release, "()V"),
    REGISTER_NONVIRTUAL(AudioTrack, write, "([BII)I", int, jbyteArray, int, int),
    REGISTER_NONVIRTUAL(AudioTrack, write, "([BIII)I", int, jbyteArray, int, int, int),
    /* Registered by hand rather than through REGISTER_NONVIRTUAL: that macro
     * takes the Java method name from the C++ identifier, which would publish
     * this as "write_shorts" and leave the engine's lookup of "write([SII)I"
     * failing exactly as before. The C++ name has to differ - it is an overload
     * the deduction macro cannot disambiguate - so the Java name is given
     * explicitly. */
    /* Register, not RegisterNonVirtual, and the distinction is load-bearing.
     *
     * RegisterNonVirtual builds a dispatcher taking (JNIEnv*, jobject, jclass,
     * va_list). iface_CallMethodV - which is what the engine reaches through
     * CallIntMethod, the ordinary way to call an instance method - casts
     * addr_variadic to (JNIEnv*, jobject, va_list) and passes three arguments.
     * The four-argument dispatcher then reads one register too far and takes
     * whatever is there as its va_list.
     *
     * That does not fail cleanly. It arrived as a jshortArray of 0x83469cb5
     * with a count of -599784200 - audio sample data being read as arguments -
     * and faulted deep inside this function on a pointer that was never a
     * pointer. Only CallNonvirtual* supplies the jclass, and nothing calls
     * AudioTrack.write that way. */
    ManagedMethod::Register<&AudioTrack::write_shorts>(
        AudioTrack::clazz, "write", "([SII)I"),
    NULL
};

Class AudioTrack::clazz = {
    .classpath = "android/media/AudioTrack",
    .classname = "AudioClass",
    .managed_methods = mediaAudioTrackClassMethods,
    .fields = mediaAudioTrackClassFields,
    .instance_size = sizeof(AudioTrack)
};

static const int registered = ClassRegistry::register_class(AudioTrack::clazz);
