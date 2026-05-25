#pragma once

#include "common.h"
#include "audio_system.h"
#ifdef __APPLE__
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#else
#include <AL/al.h>
#include <AL/alc.h>
#endif

// Allow platform builds to shrink these — iOS 3.1.3 on the iPod Touch 2G
// only has 128 MB of RAM and the OpenAL driver tends to refuse to hand
// out more than ~32 sources, so we keep these overridable.
#ifndef MAX_SOUND_INSTANCES
#define MAX_SOUND_INSTANCES 128
#endif
#define SOUND_INSTANCE_ID_BASE 100000
#ifndef MAX_AUDIO_STREAMS
#define MAX_AUDIO_STREAMS 32
#endif
// This is the index space that the native runner uses
#define AUDIO_STREAM_INDEX_BASE 300000

#define AL_STREAM_BUFFER_COUNT 4
#ifndef AL_STREAM_BUFFER_SAMPLES
#define AL_STREAM_BUFFER_SAMPLES 4096
#endif

struct stb_vorbis;

typedef struct {
    bool active;
    int32_t soundIndex; // SOND resource that spawned this
    int32_t instanceId; // unique ID returned to GML
    ALuint alSource; // OpenAL source object
    ALuint alBuffer; // OpenAL buffer object (only valid when streaming == false)
    float targetGain;
    float currentGain;
    float fadeTimeRemaining;
    float fadeTotalTime;
    float startGain;
    int32_t priority;

    // Streaming state (only valid when streaming == true)
    bool streaming;
    bool loop;
    bool streamEnded; // decoder produced no more samples; waiting for queue to drain
    struct stb_vorbis* vorbis;
    ALuint streamBuffers[AL_STREAM_BUFFER_COUNT];
    int16_t* decodeScratch; // sized for AL_STREAM_BUFFER_SAMPLES * streamChannels shorts
    int streamChannels;
    int streamSampleRate;
    ALenum streamFormat;
    float streamLengthSeconds;
    uint64_t playedSamples; // cumulative per-channel samples that have left the queue
} SoundInstance;

typedef struct {
    bool active;
    char* filePath; // resolved file path (owned, freed on destroy)
} AudioStreamEntry;

typedef struct {
    AudioSystem base;
    ALCdevice* alDevice;
    ALCcontext* alContext;
    SoundInstance instances[MAX_SOUND_INSTANCES];
    int32_t nextInstanceCounter;
    FileSystem* fileSystem;
    AudioStreamEntry streams[MAX_AUDIO_STREAMS];
} AlAudioSystem;

AlAudioSystem* AlAudioSystem_create(void);
