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

// Buffer cache: reuse decoded PCM across multiple sources playing the
// same sound, avoiding redundant decode + OpenAL copies.  The cache
// size is kept deliberately small for the iPod Touch 2G (128 MB RAM).
#ifndef AL_BUFFER_CACHE_SIZE
#define AL_BUFFER_CACHE_SIZE 48
#endif
// Total PCM bytes the cache is allowed to hold. Evict LRU entries
// once this threshold is exceeded.
#ifndef AL_BUFFER_CACHE_BUDGET
#define AL_BUFFER_CACHE_BUDGET (4u * 1024u * 1024u)
#endif

struct stb_vorbis;

typedef struct {
    bool inUse;           // slot occupied
    int32_t soundIndex;   // SOND index this buffer was decoded from
    ALuint alBuffer;      // OpenAL buffer name (shared across sources)
    uint32_t pcmBytes;    // uncompressed PCM size inside OpenAL
    uint32_t refCount;    // number of active SoundInstances referencing this
    uint32_t lastUsedFrame; // frame counter at last play (for LRU eviction)
} BufferCacheEntry;

typedef struct {
    bool active;
    int32_t soundIndex; // SOND resource that spawned this
    int32_t instanceId; // unique ID returned to GML
    ALuint alSource; // OpenAL source object
    ALuint alBuffer; // OpenAL buffer object (only valid when streaming == false && !bufferCached)
    bool bufferCached; // true when alBuffer belongs to the shared cache
    int32_t cacheSlot;  // index into bufferCache[] when bufferCached==true
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

    // Shared buffer cache
    BufferCacheEntry bufferCache[AL_BUFFER_CACHE_SIZE];
    uint32_t cacheResidentBytes; // sum of pcmBytes across all inUse entries
    uint32_t frameCounter;       // incremented each maUpdate() call
} AlAudioSystem;

AlAudioSystem* AlAudioSystem_create(void);

// Call from the iOS memory-warning handler (or equivalent) to free
// cached audio buffers that have no active sources referencing them.
void AlAudioSystem_handleMemoryWarning(AlAudioSystem* ma);
