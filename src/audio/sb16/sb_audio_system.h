#pragma once
#include "common.h"
#include "audio_system.h"

#define SB_MAX_SOUND_INSTANCES  128
#define SB_SOUND_INSTANCE_ID_BASE 100000
#define SB_MAX_AUDIO_STREAMS    32
#define SB_AUDIO_STREAM_INDEX_BASE 300000

// Decoded PCM sound asset (loaded once, shared across instances)
typedef struct {
    int16_t  *samples;      // interleaved stereo 16-bit PCM at SB_SAMPLE_RATE
    uint32_t  numFrames;    // number of sample frames (per-channel)
    bool      loaded;
} SbSoundAsset;

typedef struct {
    bool     active;
    int32_t  soundIndex;    // SOND resource index or stream index
    int32_t  instanceId;
    int16_t *samples;       // pointer into asset (not owned) or stream buffer (owned)
    uint32_t numFrames;
    uint32_t cursor;        // current frame position
    bool     loop;
    bool     ownssamples;   // true for streams
    float    gain;
    float    pitch;         // pitch != 1.0 handled via resampling cursor advance
    float    pitchAccum;    // sub-frame accumulator for pitch
    float    targetGain;
    float    currentGain;
    float    fadeTimeRemaining;
    float    fadeTotalTime;
    float    startGain;
    int32_t  priority;
} SbSoundInstance;

typedef struct {
    bool   active;
    char  *filePath;
    // Decoded PCM for file-based streams (loaded on first play)
    int16_t  *samples;
    uint32_t  numFrames;
} SbStreamEntry;

typedef struct {
    AudioSystem      base;
    FileSystem      *fileSystem;

    // Per-SOND decoded assets (lazily loaded)
    SbSoundAsset    *assets;    // array indexed by sound index, length = dw->sond.count
    uint32_t         assetCount;

    SbSoundInstance  instances[SB_MAX_SOUND_INSTANCES];
    int32_t          nextInstanceCounter;

    SbStreamEntry    streams[SB_MAX_AUDIO_STREAMS];
} SbAudioSystem;

SbAudioSystem *SbAudioSystem_create(void);
