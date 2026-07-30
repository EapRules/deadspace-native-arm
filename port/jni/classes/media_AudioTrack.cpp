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

AudioTrack::AudioTrack(int streamType, int sampleRateInHz, int channelConfig, int audioFormat, int bufferSizeInBytes, int mode)
{    
    SDL_zero(desired);
    desired.freq = sampleRateInHz;
    desired.format = GetSDLFormat(audioFormat);
    desired.channels = (channelConfig == 4) ? 1 : 2;
    desired.samples = bufferSizeInBytes / (desired.channels * GetSDLFormatBytes(audioFormat));
    desired.callback = NULL;
    playing = 0;
    
    needed_bytes = bufferSizeInBytes;
    deviceId = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
    this->mode = mode;
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

    uintptr_t where = (uintptr_t)data->elements + (size_t)offsetInShorts * 2;
    int       bytes = sizeInShorts * 2;

    int ret = SDL_QueueAudio(track->deviceId, (void *)where, bytes);

    if (track->playing == 0)
        AudioTrack::play(env, obj, (jclass)&AudioTrack::clazz);

    /* Blocking, like the byte[] path: the engine's mixer thread expects write()
     * to be back-pressure, and returning immediately makes it produce as fast
     * as it can into a queue that only grows. */
    do {
        SDL_Delay(0);
    } while (SDL_GetQueuedAudioSize(track->deviceId));

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
