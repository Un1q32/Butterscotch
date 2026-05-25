// On Windows, include windows.h first so its headers are processed before stb_vorbis
// defines single-letter macros (L, C, R) that conflict with winnt.h struct field names.
#ifdef _WIN32
#include <windows.h>
#endif

#include "stb_vorbis.c"
#include "al_audio_system.h"
#include "data_win.h"
#include "utils.h"
#include "wave.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "stb_ds.h"

// ===[ Helpers ]===

static bool alSourceIsPlaying(ALuint source) {
    ALint state;
    alGetSourcei(source, AL_SOURCE_STATE, &state);
    return state == AL_PLAYING;
}

static bool alSourceHasStopped(ALuint source) {
    ALint state;
    alGetSourcei(source, AL_SOURCE_STATE, &state);
    return state == AL_STOPPED;
}

static bool alSourceIsLooping(ALuint source) {
    ALint state;
    alGetSourcei(source, AL_LOOPING, &state);
    return state != AL_FALSE;
}

// Source - https://stackoverflow.com/a/7995655
// Posted by Karl
// Retrieved 2026-05-05, License - CC BY-SA 3.0
static void alGetSourceLengthSec(ALuint buffer, float* out) {
    ALint sizeInBytes;
    ALint channels;
    ALint bits;

    alGetBufferi(buffer, AL_SIZE, &sizeInBytes);
    alGetBufferi(buffer, AL_CHANNELS, &channels);
    alGetBufferi(buffer, AL_BITS, &bits);

    int lengthInSamples = sizeInBytes * 8 / (channels * bits);
    ALint frequency;

    alGetBufferi(buffer, AL_FREQUENCY, &frequency);

    *out = (float)lengthInSamples / (float)frequency;
}

// Tears down whatever AL state is attached to a slot and marks it inactive.
// Pass the owning AlAudioSystem so buffer-cache ref counts are updated.
static void releaseInstanceEx(AlAudioSystem* ma, SoundInstance* inst) {
    if (!inst->active)
        return;

    alSourceStop(inst->alSource);

    if (inst->streaming) {
        ALint queued = 0;
        alGetSourcei(inst->alSource, AL_BUFFERS_QUEUED, &queued);
        repeat(queued, i) {
            ALuint b;
            alSourceUnqueueBuffers(inst->alSource, 1, &b);
        }
        alDeleteSources(1, &inst->alSource);
        alDeleteBuffers(AL_STREAM_BUFFER_COUNT, inst->streamBuffers);
        if (inst->vorbis != nullptr) {
            stb_vorbis_close((stb_vorbis*) inst->vorbis);
            inst->vorbis = nullptr;
        }
        free(inst->decodeScratch);
        inst->decodeScratch = nullptr;
        inst->streaming = false;
    } else {
        alSourcei(inst->alSource, AL_BUFFER, 0); // detach buffer from source
        alDeleteSources(1, &inst->alSource);
        if (inst->bufferCached && ma != NULL) {
            // Decrement ref count in the shared cache; buffer stays alive
            // for future reuse until evicted by memory pressure or LRU.
            if (inst->cacheSlot >= 0 && inst->cacheSlot < AL_BUFFER_CACHE_SIZE) {
                BufferCacheEntry* ce = &ma->bufferCache[inst->cacheSlot];
                if (ce->refCount > 0) ce->refCount--;
            }
        } else {
            alDeleteBuffers(1, &inst->alBuffer);
        }
    }

    inst->bufferCached = false;
    inst->cacheSlot = -1;
    inst->active = false;
}

// Decode the next chunk from inst->vorbis into inst->decodeScratch and upload it to "buf".
// Wraps around on EOF if inst->loop is set.
// Returns false when no more samples are available (decoder exhausted and not looping, or read failed).
static bool streamFillBuffer(SoundInstance* inst, ALuint buf) {
    stb_vorbis* v = (stb_vorbis*) inst->vorbis;
    int samples = stb_vorbis_get_samples_short_interleaved(v, inst->streamChannels, inst->decodeScratch, AL_STREAM_BUFFER_SAMPLES * inst->streamChannels);
    if (0 >= samples) {
        if (!inst->loop) return false;
        stb_vorbis_seek_start(v);
        samples = stb_vorbis_get_samples_short_interleaved(v, inst->streamChannels, inst->decodeScratch, AL_STREAM_BUFFER_SAMPLES * inst->streamChannels);
        if (0 >= samples) return false;
    }
    alBufferData(buf, inst->streamFormat, inst->decodeScratch, samples * inst->streamChannels * (ALsizei) sizeof(int16_t), inst->streamSampleRate);
    return true;
}

// ===[ Buffer Cache Helpers ]===

// Look up an existing cached buffer for a given sound index.
static int32_t cacheLookup(AlAudioSystem* ma, int32_t soundIndex) {
    repeat(AL_BUFFER_CACHE_SIZE, i) {
        if (ma->bufferCache[i].inUse && ma->bufferCache[i].soundIndex == soundIndex) {
            return (int32_t) i;
        }
    }
    return -1;
}

// Evict the least-recently-used cache entry that has refCount == 0.
// Returns the slot index freed, or -1 if nothing could be evicted.
static int32_t cacheEvictLRU(AlAudioSystem* ma) {
    int32_t best = -1;
    uint32_t bestFrame = UINT32_MAX;
    repeat(AL_BUFFER_CACHE_SIZE, i) {
        BufferCacheEntry* ce = &ma->bufferCache[i];
        if (!ce->inUse) continue;
        if (ce->refCount > 0) continue;
        if (ce->lastUsedFrame < bestFrame) {
            bestFrame = ce->lastUsedFrame;
            best = (int32_t) i;
        }
    }
    if (best >= 0) {
        BufferCacheEntry* ce = &ma->bufferCache[best];
        alDeleteBuffers(1, &ce->alBuffer);
        ma->cacheResidentBytes -= ce->pcmBytes;
        ce->inUse = false;
    }
    return best;
}

// Maximum PCM size for a single buffer to be eligible for caching.
// Sounds larger than this are unlikely to repeat rapidly and would
// bloat the cache — let them use private (non-shared) buffers.
#ifndef AL_BUFFER_CACHE_MAX_ENTRY
#define AL_BUFFER_CACHE_MAX_ENTRY (128u * 1024u)
#endif

// Insert a buffer into the cache. Evicts LRU entries if over budget.
// Returns slot index, or -1 on failure (entry too large, over budget
// with nothing evictable, or no free slot).
static int32_t cacheInsert(AlAudioSystem* ma, int32_t soundIndex, ALuint buffer, uint32_t pcmBytes) {
    // Refuse to cache large sounds — they bloat the budget and rarely
    // benefit from sharing (long/music-like sounds).
    if (pcmBytes > AL_BUFFER_CACHE_MAX_ENTRY) return -1;

    // Make room if over budget
    while (ma->cacheResidentBytes + pcmBytes > AL_BUFFER_CACHE_BUDGET) {
        if (cacheEvictLRU(ma) < 0) {
            // Cannot evict anything (all entries have active refs).
            // Hard-enforce the budget: refuse to cache this sound.
            return -1;
        }
    }

    // Find a free slot
    int32_t slot = -1;
    repeat(AL_BUFFER_CACHE_SIZE, i) {
        if (!ma->bufferCache[i].inUse) { slot = (int32_t) i; break; }
    }
    if (slot < 0) {
        // All slots occupied — try one more eviction
        slot = cacheEvictLRU(ma);
    }
    if (slot < 0) return -1;

    BufferCacheEntry* ce = &ma->bufferCache[slot];
    ce->inUse = true;
    ce->soundIndex = soundIndex;
    ce->alBuffer = buffer;
    ce->pcmBytes = pcmBytes;
    ce->refCount = 0;
    ce->lastUsedFrame = ma->frameCounter;
    ma->cacheResidentBytes += pcmBytes;
    return slot;
}

static SoundInstance* findFreeSlot(AlAudioSystem* ma) {
    // First pass: find an inactive slot
    repeat(MAX_SOUND_INSTANCES, i) {
        if (!ma->instances[i].active) {
            return &ma->instances[i];
        }
    }

    // Second pass: evict the lowest-priority ended sound.
    // Streaming instances can briefly report AL_STOPPED during an underrun, so exclude them from eviction to keep music alive across SFX bursts.
    SoundInstance* best = nullptr;
    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->streaming)
            continue;

        if (!alSourceIsPlaying(inst->alSource)) {
            if (best == nullptr || best->priority > inst->priority) {
                best = inst;
            }
        }
    }

    if (best != nullptr) {
        releaseInstanceEx(ma, best);
    }

    return best;
}

static SoundInstance* findInstanceById(AlAudioSystem* ma, int32_t instanceId) {
    int32_t slotIndex = instanceId - SOUND_INSTANCE_ID_BASE;
    if (0 > slotIndex || slotIndex >= MAX_SOUND_INSTANCES) return nullptr;
    SoundInstance* inst = &ma->instances[slotIndex];
    if (!inst->active || inst->instanceId != instanceId) return nullptr;
    return inst;
}

// Helper: resolve external audio file path from Sound entry
static char* resolveExternalPath(AlAudioSystem* ma, Sound* sound) {
    const char* file = sound->file;
    if (file == nullptr || file[0] == '\0') return nullptr;

    // If the filename has no extension, append ".ogg"
    bool hasExtension = (strchr(file, '.') != nullptr);

    char filename[512];
    if (hasExtension) {
        snprintf(filename, sizeof(filename), "%s", file);
    } else {
        snprintf(filename, sizeof(filename), "%s.ogg", file);
    }

    return ma->fileSystem->vtable->resolvePath(ma->fileSystem, filename);
}

// ===[ Vtable Implementations ]===

static void maInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;
    arrput(ma->base.audioGroups, dataWin);
    ma->fileSystem = fileSystem;

    ma->alDevice = alcOpenDevice(nullptr);
    ma->alContext = alcCreateContext(ma->alDevice, nullptr);
    alcMakeContextCurrent(ma->alContext);
    if (ma->alDevice == nullptr || ma->alContext == nullptr) {
        fprintf(stderr, "Audio: Failed to initialize OpenAL engine (error %d)\n", alGetError());
        return;
    }

    memset(ma->instances, 0, sizeof(ma->instances));
    ma->nextInstanceCounter = 0;

    fprintf(stderr, "Audio: OpenAL engine initialized\n");
}

static void maDestroy(AudioSystem* audio) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    // Uninit all active sound instances (uses releaseInstanceEx so
    // ref counts are decremented before we nuke the cache).
    repeat(MAX_SOUND_INSTANCES, i) {
        releaseInstanceEx(ma, &ma->instances[i]);
    }

    // Destroy the buffer cache
    repeat(AL_BUFFER_CACHE_SIZE, i) {
        if (ma->bufferCache[i].inUse) {
            alDeleteBuffers(1, &ma->bufferCache[i].alBuffer);
            ma->bufferCache[i].inUse = false;
        }
    }
    ma->cacheResidentBytes = 0;

    // Free stream entries
    repeat(MAX_AUDIO_STREAMS, i) {
        if (ma->streams[i].active) {
            free(ma->streams[i].filePath);
        }
    }

    // Free loaded audio groups. The main data.win is owned by the caller, so skip index 0.
    if (arrlen(ma->base.audioGroups) > 1) {
        for (int32_t i = 1; i < (int32_t) arrlen(ma->base.audioGroups); i++) {
            DataWin_free(ma->base.audioGroups[i]);
        }
    }
    arrfree(ma->base.audioGroups);

    alcMakeContextCurrent(nullptr);
    alcDestroyContext(ma->alContext);
    alcCloseDevice(ma->alDevice);
    free(ma);
}

static void maUpdate(AudioSystem* audio, float deltaTime) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;
    ma->frameCounter++;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (!inst->active) continue;

        // Handle gain fading (for cases where we do manual fading)
        if (inst->fadeTimeRemaining > 0.0f) {
            inst->fadeTimeRemaining -= deltaTime;
            if (0.0f >= inst->fadeTimeRemaining) {
                inst->fadeTimeRemaining = 0.0f;
                inst->currentGain = inst->targetGain;
            } else {
                float t = 1.0f - (inst->fadeTimeRemaining / inst->fadeTotalTime);
                inst->currentGain = inst->startGain + (inst->targetGain - inst->startGain) * t;
            }
            alSourcef(inst->alSource, AL_GAIN, inst->currentGain);
        }

        if (inst->streaming) {
            // Recycle any buffers AL has finished with: count their samples toward the play position, then refill from the decoder and re-queue at the tail.
            ALint processed = 0;
            alGetSourcei(inst->alSource, AL_BUFFERS_PROCESSED, &processed);
            while (processed > 0) {
                ALuint buf;
                alSourceUnqueueBuffers(inst->alSource, 1, &buf);
                processed--;

                ALint sizeBytes = 0, bits = 0, channels = 0;
                alGetBufferi(buf, AL_SIZE, &sizeBytes);
                alGetBufferi(buf, AL_BITS, &bits);
                alGetBufferi(buf, AL_CHANNELS, &channels);
                if (bits > 0 && channels > 0) {
                    inst->playedSamples += (uint64_t) (sizeBytes * 8 / (bits * channels));
                }

                if (!inst->streamEnded) {
                    if (streamFillBuffer(inst, buf)) {
                        alSourceQueueBuffers(inst->alSource, 1, &buf);
                    } else {
                        inst->streamEnded = true;
                    }
                }
            }

            // Reap once the queue has fully drained on a non-looping track.
            ALint queued = 0;
            alGetSourcei(inst->alSource, AL_BUFFERS_QUEUED, &queued);
            if (inst->streamEnded && queued == 0) {
                releaseInstanceEx(ma, inst);
                continue;
            }

            // Underrun recovery: AL goes to AL_STOPPED if the queue runs dry.
            // Kick it back on as soon as we have buffers queued again.
            if (alSourceHasStopped(inst->alSource) && queued > 0) {
                alSourcePlay(inst->alSource);
            }
            continue;
        }

        // Clean up ended non-looping sounds
        if (alSourceHasStopped(inst->alSource) && !alSourceIsLooping(inst->alSource)) {
            releaseInstanceEx(ma, inst);
        }
    }
}

static int32_t maPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    // Check if this is a stream index (created by audio_create_stream)
    bool isStream = (soundIndex >= AUDIO_STREAM_INDEX_BASE);
    Sound* sound = nullptr;
    char* streamPath = nullptr;

    if (isStream) {
        int32_t streamSlot = soundIndex - AUDIO_STREAM_INDEX_BASE;
        if (0 > streamSlot || streamSlot >= MAX_AUDIO_STREAMS || !ma->streams[streamSlot].active) {
            fprintf(stderr, "Audio: Invalid stream index %d\n", soundIndex);
            return -1;
        }
        streamPath = ma->streams[streamSlot].filePath;
    } else {
        DataWin* dw = ma->base.audioGroups[0]; // Audio Group 0 should always be data.win
        if (0 > soundIndex || (uint32_t) soundIndex >= dw->sond.count) {
            fprintf(stderr, "Audio: Invalid sound index %d\n", soundIndex);
            return -1;
        }
        sound = &dw->sond.sounds[soundIndex];
    }

    SoundInstance* slot = findFreeSlot(ma);
    if (slot == nullptr) {
        fprintf(stderr, "Audio: No free sound slots for sound %d\n", soundIndex);
        return -1;
    }

    int32_t slotIndex = (int32_t) (slot - ma->instances);

    slot->streaming = false;
    slot->vorbis = nullptr;
    slot->decodeScratch = nullptr;
    slot->streamEnded = false;
    slot->playedSamples = 0;
    slot->bufferCached = false;
    slot->cacheSlot = -1;

    if (isStream) {
        // Streaming path: open the decoder, queue a few small buffers, and let maUpdate() top them up.
        // This avoids the multi-hundred-millisecond hang of decoding a whole song into PCM on the main thread.
        int err = 0;
        stb_vorbis* v = stb_vorbis_open_filename(streamPath, &err, nullptr);
        if (v == nullptr) {
            fprintf(stderr, "Audio: Failed to open stream '%s' (stb_vorbis err %d)\n", streamPath, err);
            return -1;
        }
        stb_vorbis_info info = stb_vorbis_get_info(v);

        slot->streaming = true;
        slot->loop = loop;
        slot->vorbis = v;
        slot->streamChannels = info.channels;
        slot->streamSampleRate = (int) info.sample_rate;
        slot->streamFormat = (info.channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
        slot->streamLengthSeconds = stb_vorbis_stream_length_in_seconds(v);
        slot->decodeScratch = safeMalloc(AL_STREAM_BUFFER_SAMPLES * info.channels * sizeof(int16_t));

        alGenSources(1, &slot->alSource);
        alGenBuffers(AL_STREAM_BUFFER_COUNT, slot->streamBuffers);

        int primed = 0;
        for (int i = 0; AL_STREAM_BUFFER_COUNT > i; i++) {
            if (!streamFillBuffer(slot, slot->streamBuffers[i])) break;
            alSourceQueueBuffers(slot->alSource, 1, &slot->streamBuffers[i]);
            primed++;
        }

        if (primed == 0) {
            // Empty file or decode failure: tear everything down cleanly.
            alDeleteSources(1, &slot->alSource);
            alDeleteBuffers(AL_STREAM_BUFFER_COUNT, slot->streamBuffers);
            stb_vorbis_close(v);
            free(slot->decodeScratch);
            slot->streaming = false;
            slot->vorbis = nullptr;
            slot->decodeScratch = nullptr;
            return -1;
        }
    } else {
        // === Buffer cache fast path ===
        // If we already decoded this sound, reuse the shared buffer.
        int32_t cachedSlot = cacheLookup(ma, soundIndex);
        if (cachedSlot >= 0) {
            BufferCacheEntry* ce = &ma->bufferCache[cachedSlot];
            alGenSources(1, &slot->alSource);
            alSourcei(slot->alSource, AL_BUFFER, (ALint) ce->alBuffer);
            slot->alBuffer = ce->alBuffer;
            slot->bufferCached = true;
            slot->cacheSlot = cachedSlot;
            ce->refCount++;
            ce->lastUsedFrame = ma->frameCounter;
            goto apply_props;
        }

        alGenSources(1, &slot->alSource);
        alGenBuffers(1, &slot->alBuffer);
        bool isEmbedded = (sound->flags & 0x01) != 0;

        // GMS sound flags are ambiguous between "embedded WAV" and
        // "embedded OGG", and the AUDO blob can hold either. So we
        // resolve the actual buffer first, then sniff the magic bytes
        // and dispatch to the right decoder. External sounds (non-
        // embedded) take the file path and feed it through stb_vorbis;
        // those are always .ogg in Undertale.
        const uint8_t* embeddedBytes = nullptr;
        uint32_t embeddedSize = 0;
        uint8_t* embeddedOwned = nullptr; // freed at end of this block if non-null
        bool loadedOk = false;
        if (isEmbedded) {
            DataWin* group = nullptr;
            if (sound->audioGroup >= 0
                && (int32_t) arrlen(ma->base.audioGroups) > sound->audioGroup) {
                group = ma->base.audioGroups[sound->audioGroup];
            }
            if (group == nullptr) {
                fprintf(stderr, "Audio: audio group %d for '%s' not loaded\n",
                        sound->audioGroup, sound->name);
                alDeleteBuffers(1, &slot->alBuffer);
                alDeleteSources(1, &slot->alSource);
                return -1;
            }
            if (0 > sound->audioFile
                || (uint32_t) sound->audioFile >= group->audo.count) {
                fprintf(stderr, "Audio: invalid audio file %d for '%s'\n",
                        sound->audioFile, sound->name);
                alDeleteBuffers(1, &slot->alBuffer);
                alDeleteSources(1, &slot->alSource);
                return -1;
            }
            AudioEntry* entry = &group->audo.entries[sound->audioFile];
            embeddedSize = entry->dataSize;
            if (entry->data != nullptr) {
                embeddedBytes = (const uint8_t*) entry->data;
            } else if (entry->dataOffset > 0 && entry->dataSize > 0
                       && group->lazyLoadFile != nullptr) {
                // The iOS port parses data.win with skipLoadingAudoBlobs=1
                // to keep RAM usage low on the iPod Touch 2G (Undertale's
                // AUDO is ~50 MB of WAV/OGG that would otherwise sit in
                // memory forever).  Lazy-load this specific blob now via
                // the lazy-load FILE* and free it after decoding.
                embeddedOwned = (uint8_t*) malloc(entry->dataSize);
                if (embeddedOwned == nullptr) {
                    fprintf(stderr, "Audio: malloc %u bytes failed for '%s'\n",
                            (unsigned) entry->dataSize, sound->name);
                    alDeleteBuffers(1, &slot->alBuffer);
                    alDeleteSources(1, &slot->alSource);
                    return -1;
                }
                if (fseek(group->lazyLoadFile, (long) entry->dataOffset, SEEK_SET) != 0) {
                    fprintf(stderr, "Audio: fseek to offset %u failed for '%s'\n",
                            (unsigned) entry->dataOffset, sound->name);
                    free(embeddedOwned);
                    alDeleteBuffers(1, &slot->alBuffer);
                    alDeleteSources(1, &slot->alSource);
                    return -1;
                }
                size_t got = fread(embeddedOwned, 1, entry->dataSize, group->lazyLoadFile);
                if (got != entry->dataSize) {
                    fprintf(stderr, "Audio: short read %zu/%u for '%s'\n",
                            got, (unsigned) entry->dataSize, sound->name);
                    free(embeddedOwned);
                    alDeleteBuffers(1, &slot->alBuffer);
                    alDeleteSources(1, &slot->alSource);
                    return -1;
                }
                embeddedBytes = embeddedOwned;
            }
            if (embeddedBytes == nullptr || embeddedSize < 12) {
                fprintf(stderr, "Audio: '%s' embedded blob missing or too small (data=%p size=%u offset=%u lazyFile=%p)\n",
                        sound->name, (void*) entry->data, (unsigned) entry->dataSize,
                        (unsigned) entry->dataOffset, (void*) group->lazyLoadFile);
                if (embeddedOwned != nullptr) free(embeddedOwned);
                alDeleteBuffers(1, &slot->alBuffer);
                alDeleteSources(1, &slot->alSource);
                return -1;
            }
        }

        // Magic-byte sniff: RIFF/WAVE → uncompressed PCM, OggS → Ogg
        // Vorbis. We accept both for embedded and for external, so
        // Undertale's mix of embedded .wav SFX and external .ogg
        // music both go through the right decoder.
        bool isWAV = false;
        bool isOGG = false;
        if (isEmbedded) {
            isWAV = (memcmp(embeddedBytes, "RIFF", 4) == 0)
                 && (memcmp(embeddedBytes + 8, "WAVE", 4) == 0);
            isOGG = (memcmp(embeddedBytes, "OggS", 4) == 0);
        }

        if (isEmbedded && isWAV) {
            // Inline RIFF/WAVE parser: WAV_ParseFileData() in
            // src/audio/openal/wave.c assumes a fixed layout (fmt size
            // == 16, no extra chunks, data follows immediately), but
            // Undertale's embedded SFX are produced by GameMaker with
            // arbitrary chunk orderings and 18-byte / 40-byte fmt
            // headers — feeding those through the rigid parser
            // mis-measures the PCM length and gives OpenAL a buffer
            // pointing past the heap, which is the EXC_BAD_ACCESS the
            // user is hitting in alBufferData on iPod Touch 2G.
            // Walk the chunks properly instead.
            int parseErr = 0;
            uint16_t fmtAudioFormat = 0;
            uint16_t fmtChannels = 0;
            uint32_t fmtSampleRate = 0;
            uint16_t fmtBitsPerSample = 0;
            const uint8_t* pcmData = nullptr;
            uint32_t pcmSize = 0;
            uint32_t cursor = 12; // skip RIFF + size + WAVE
            while (cursor + 8 <= embeddedSize) {
                const uint8_t* h = embeddedBytes + cursor;
                uint32_t chunkSize = (uint32_t)h[4]
                                   | ((uint32_t)h[5] << 8)
                                   | ((uint32_t)h[6] << 16)
                                   | ((uint32_t)h[7] << 24);
                if (cursor + 8 + chunkSize > embeddedSize) { parseErr = 1; break; }
                if (memcmp(h, "fmt ", 4) == 0 && chunkSize >= 16) {
                    fmtAudioFormat   = (uint16_t)(h[8]  | (h[9]  << 8));
                    fmtChannels      = (uint16_t)(h[10] | (h[11] << 8));
                    fmtSampleRate    = (uint32_t)h[12] | ((uint32_t)h[13] << 8)
                                     | ((uint32_t)h[14] << 16) | ((uint32_t)h[15] << 24);
                    fmtBitsPerSample = (uint16_t)(h[22] | (h[23] << 8));
                } else if (memcmp(h, "data", 4) == 0) {
                    pcmData = h + 8;
                    pcmSize = chunkSize;
                    break;
                }
                // chunks are word-aligned; size is rounded up to even
                uint32_t advance = 8 + chunkSize + (chunkSize & 1u);
                cursor += advance;
            }

            if (parseErr || pcmData == nullptr || pcmSize == 0
                || fmtChannels == 0 || fmtSampleRate == 0
                || (fmtBitsPerSample != 8 && fmtBitsPerSample != 16)) {
                fprintf(stderr, "Audio: '%s' WAV parse failed (fmtCh=%u sr=%u bps=%u dataSz=%u)\n",
                        sound->name, fmtChannels, (unsigned) fmtSampleRate,
                        fmtBitsPerSample, (unsigned) pcmSize);
                if (embeddedOwned != nullptr) free(embeddedOwned);
                alDeleteBuffers(1, &slot->alBuffer);
                alDeleteSources(1, &slot->alSource);
                return -1;
            }
            // GameMaker WAVs are always PCM; refuse anything else.
            if (fmtAudioFormat != 1) {
                fprintf(stderr, "Audio: '%s' WAV audio_format=%u (not PCM), skipping\n",
                        sound->name, fmtAudioFormat);
                if (embeddedOwned != nullptr) free(embeddedOwned);
                alDeleteBuffers(1, &slot->alBuffer);
                alDeleteSources(1, &slot->alSource);
                return -1;
            }

            ALenum format = (fmtChannels == 1)
                ? (fmtBitsPerSample == 8 ? AL_FORMAT_MONO8   : AL_FORMAT_MONO16)
                : (fmtBitsPerSample == 8 ? AL_FORMAT_STEREO8 : AL_FORMAT_STEREO16);
            alBufferData(slot->alBuffer, format, pcmData,
                         (ALsizei) pcmSize, (ALsizei) fmtSampleRate);
            alSourcei(slot->alSource, AL_BUFFER, slot->alBuffer);
            loadedOk = true;
        } else if (isEmbedded && isOGG) {
            int channels = 0, sample_rate = 0;
            short* data = NULL;
            int len = stb_vorbis_decode_memory(embeddedBytes, (int) embeddedSize, &channels, &sample_rate, &data);
            if (len <= 0 || data == NULL) {
                fprintf(stderr, "Audio: stb_vorbis_decode_memory failed for '%s'\n", sound->name);
                if (data != NULL) free(data);
                if (embeddedOwned != nullptr) free(embeddedOwned);
                alDeleteBuffers(1, &slot->alBuffer);
                alDeleteSources(1, &slot->alSource);
                return -1;
            }
            alBufferData(slot->alBuffer,
                         (channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16,
                         (void*) data,
                         len * channels * (int) sizeof(int16_t),
                         sample_rate);
            alSourcei(slot->alSource, AL_BUFFER, slot->alBuffer);
            free(data);
            loadedOk = true;
        } else if (!isEmbedded) {
            // External audio file (always Ogg Vorbis in Undertale).
            char* path = resolveExternalPath(ma, sound);
            if (path == nullptr) {
                fprintf(stderr, "Audio: Could not resolve path for '%s'\n", sound->name);
                alDeleteBuffers(1, &slot->alBuffer);
                alDeleteSources(1, &slot->alSource);
                return -1;
            }
            int channels = 0, sample_rate = 0;
            short* data = NULL;
            int len = stb_vorbis_decode_filename(path, &channels, &sample_rate, &data);
            if (len <= 0 || data == NULL) {
                fprintf(stderr, "Audio: stb_vorbis_decode_filename failed for '%s' (path '%s')\n", sound->name, path);
                free(path);
                if (data != NULL) free(data);
                alDeleteBuffers(1, &slot->alBuffer);
                alDeleteSources(1, &slot->alSource);
                return -1;
            }
            alBufferData(slot->alBuffer,
                         (channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16,
                         (void*) data,
                         len * channels * (int) sizeof(int16_t),
                         sample_rate);
            alSourcei(slot->alSource, AL_BUFFER, slot->alBuffer);
            free(data);
            free(path);
            loadedOk = true;
        } else {
            fprintf(stderr, "Audio: '%s' embedded blob has unknown magic %02x %02x %02x %02x\n",
                    sound->name,
                    embeddedBytes[0], embeddedBytes[1], embeddedBytes[2], embeddedBytes[3]);
            if (embeddedOwned != nullptr) free(embeddedOwned);
            alDeleteBuffers(1, &slot->alBuffer);
            alDeleteSources(1, &slot->alSource);
            return -1;
        }

        // The decoded PCM is owned by OpenAL now (alBufferData copied
        // it), so the raw WAV/OGG bytes we lazy-loaded are no longer
        // needed.  Free the temporary lazy buffer if we allocated one.
        if (embeddedOwned != nullptr) free(embeddedOwned);

        if (!loadedOk) {
            alDeleteBuffers(1, &slot->alBuffer);
            alDeleteSources(1, &slot->alSource);
            return -1;
        }

        // Insert the freshly-decoded buffer into the shared cache so
        // future plays of the same sound reuse it instead of decoding
        // again. Query OpenAL for the actual buffer size in bytes.
        ALint bufSizeBytes = 0;
        alGetBufferi(slot->alBuffer, AL_SIZE, &bufSizeBytes);
        int32_t newCacheSlot = cacheInsert(ma, soundIndex, slot->alBuffer, (uint32_t) bufSizeBytes);
        if (newCacheSlot >= 0) {
            slot->bufferCached = true;
            slot->cacheSlot = newCacheSlot;
            ma->bufferCache[newCacheSlot].refCount++;
        }
    }

apply_props:;
    // Apply properties
    float volume = isStream ? 1.0f : sound->volume;
    float pitch = isStream ? 1.0f : sound->pitch;
    alSourcef(slot->alSource, AL_GAIN, volume);

    if (pitch != 1.0f) {
        alSourcef(slot->alSource, AL_PITCH, pitch != 0.0f ? pitch : 1.0f);
    }
    // AL_LOOPING on a streaming source only loops the currently-playing buffer, not the whole queue,
    // so streaming looping is handled by streamFillBuffer calling stb_vorbis_seek_start when the decoder runs out.
    if (!isStream)
        alSourcei(slot->alSource, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);

    // Set up instance tracking
    slot->active = true;
    slot->soundIndex = soundIndex;
    slot->instanceId = SOUND_INSTANCE_ID_BASE + slotIndex;
    slot->currentGain = volume;
    slot->targetGain = volume;
    slot->fadeTimeRemaining = 0.0f;
    slot->fadeTotalTime = 0.0f;
    slot->startGain = volume;
    slot->priority = priority;

    // Track unique IDs for disambiguation
    ma->nextInstanceCounter++;

    alSourcePlay(slot->alSource);

    return slot->instanceId;
}

static void maStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) releaseInstanceEx(ma, inst);
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                releaseInstanceEx(ma, inst);
            }
        }
    }
}

static void maStopAll(AudioSystem* audio) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        releaseInstanceEx(ma, &ma->instances[i]);
    }
}

static bool maIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst == nullptr)
            return false;

        // Streaming sources can flip to AL_STOPPED for a frame during underrun, so trust the active flag instead (cleared by maUpdate when fully drained).
        if (inst->streaming)
            return inst->active;

        return alSourceIsPlaying(inst->alSource);
    } else {
        // Check if any instance of this sound resource is playing
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (!inst->active || inst->soundIndex != soundOrInstance) continue;
            if (inst->streaming) return true;
            if (alSourceIsPlaying(inst->alSource)) return true;
        }
        return false;
    }
}

static void maPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            alSourcePause(inst->alSource);
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                alSourcePause(inst->alSource);
            }
        }
    }
}

static void maResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            alSourcePlay(inst->alSource);
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                alSourcePlay(inst->alSource);
            }
        }
    }
}

static void maPauseAll(AudioSystem* audio) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active && alSourceIsPlaying(inst->alSource)) {
            alSourcePause(inst->alSource);
        }
    }
}

static void maResumeAll(AudioSystem* audio) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active) {
            alSourcePlay(inst->alSource);
        }
    }
}

static void maSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            if (timeMs == 0) {
                inst->currentGain = gain;
                inst->targetGain = gain;
                inst->fadeTimeRemaining = 0.0f;
                alSourcef(inst->alSource, AL_GAIN, gain);
            } else {
                inst->startGain = inst->currentGain;
                inst->targetGain = gain;
                inst->fadeTotalTime = (float) timeMs / 1000.0f;
                inst->fadeTimeRemaining = inst->fadeTotalTime;
            }
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                if (timeMs == 0) {
                    inst->currentGain = gain;
                    inst->targetGain = gain;
                    inst->fadeTimeRemaining = 0.0f;
                    alSourcef(inst->alSource, AL_GAIN, gain);
                } else {
                    inst->startGain = inst->currentGain;
                    inst->targetGain = gain;
                    inst->fadeTotalTime = (float) timeMs / 1000.0f;
                    inst->fadeTimeRemaining = inst->fadeTotalTime;
                }
            }
        }
    }
}

static float maGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) return inst->currentGain;
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                return inst->currentGain;
            }
        }
    }
    return 0.0f;
}

static void maSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            alSourcef(inst->alSource, AL_PITCH, pitch);
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                alSourcef(inst->alSource, AL_PITCH, pitch);
            }
        }
    }
}

static float maGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    float pitch = 1.0f;
    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) alGetSourcef(inst->alSource, AL_PITCH, &pitch);
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                alGetSourcef(inst->alSource, AL_PITCH, &pitch);
            }
        }
    }
    return pitch;
}

// For streaming instances AL_SEC_OFFSET resets per buffer in the queue, so we combine the dequeued-sample tally with the offset into the currently-playing buffer to report a position over the whole track.
static float streamCursorSeconds(SoundInstance* inst) {
    if (0 >= inst->streamSampleRate)
        return 0.0f;
    
    ALint sampleOffset = 0;
    alGetSourcei(inst->alSource, AL_SAMPLE_OFFSET, &sampleOffset);
    uint64_t total = inst->playedSamples + (uint64_t) sampleOffset;
    return (float) total / (float) inst->streamSampleRate;
}

static float maGetTrackPosition(AudioSystem* audio, int32_t soundOrInstance) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            if (inst->streaming) return streamCursorSeconds(inst);
            float cursor;
            alGetSourcef(inst->alSource, AL_SEC_OFFSET, &cursor);
            return cursor;
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                if (inst->streaming) return streamCursorSeconds(inst);
                float cursor;
                alGetSourcef(inst->alSource, AL_SEC_OFFSET, &cursor);
                return cursor;
            }
        }
    }
    return 0.0f;
}

static void maSetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            alSourcef(inst->alSource, AL_SEC_OFFSET, positionSeconds);
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
            alSourcef(inst->alSource, AL_SEC_OFFSET, positionSeconds);
            }
        }
    }
}

// Total length of a loaded sound. Works on both SOND index and active instance ids.
static float maGetSoundLength(AudioSystem* audio, int32_t soundOrInstance) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    SoundInstance* match = nullptr;
    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        match = findInstanceById(ma, soundOrInstance);
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                match = inst;
                break;
            }
        }
    }
    if (match != nullptr) {
        if (match->streaming) return match->streamLengthSeconds;
        float seconds = 0.0f;
        alGetSourceLengthSec(match->alBuffer, &seconds);
        return seconds;
    }

    // No active instance: GMS audio_sound_length(soundIndex) must still return the asset's duration.
    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE || soundOrInstance >= AUDIO_STREAM_INDEX_BASE)
        return 0.0f;

    DataWin* dw = ma->base.audioGroups[0];
    if (dw == nullptr || 0 > soundOrInstance || (uint32_t) soundOrInstance >= dw->sond.count)
        return 0.0f;

    Sound* sound = &dw->sond.sounds[soundOrInstance];

    bool isEmbedded = (sound->flags & 0x01) != 0;
    bool isCompressed = (sound->flags & 0x02) != 0;
    if (isEmbedded || isCompressed) {
        if (0 > sound->audioFile || (uint32_t) sound->audioFile >= ma->base.audioGroups[sound->audioGroup]->audo.count) return 0.0f;
        AudioEntry* entry = &ma->base.audioGroups[sound->audioGroup]->audo.entries[sound->audioFile];
        WAVFile wav = WAV_ParseFileData(entry->data);
        float seconds = 0.0f;
        if (wav.header.byte_rate > 0) seconds = (float) wav.header.data_size / (float) wav.header.byte_rate;
        if (wav.data != nullptr) free(wav.data);
        return seconds;
    }

    char* path = resolveExternalPath(ma, sound);
    if (path == nullptr) return 0.0f;
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_filename(path, &err, nullptr);
    free(path);
    if (v == nullptr) return 0.0f;
    float seconds = stb_vorbis_stream_length_in_seconds(v);
    stb_vorbis_close(v);
    return seconds;
}

static void maSetMasterGain(AudioSystem* audio, float gain) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;
    alListenerf(AL_GAIN, gain);
}

static void maSetChannelCount(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t count) {
    // miniaudio handles channel management internally, this is a no-op
}

static void maGroupLoad(AudioSystem* audio, int32_t groupIndex) {
    if (groupIndex <= 0) return;
    // Already loaded?  Don't append a duplicate entry — the play path
    // indexes audioGroups by groupIndex directly, so a second arrput
    // would shift everything and corrupt the lookups.
    if ((int32_t) arrlen(audio->audioGroups) > groupIndex) return;

    AlAudioSystem* ma = (AlAudioSystem*) audio;
    char buf[32];
    snprintf(buf, sizeof(buf), "audiogroup%d.dat", groupIndex);

    // DataWin_parse calls exit(1) on fopen failure, so refuse to call
    // it if the file isn't present.  Pad with an empty group instead.
    bool exists = ma->fileSystem->vtable->fileExists(ma->fileSystem, buf);
    if (!exists) {
        fprintf(stderr, "Audio: audiogroup%d.dat not found, registering empty placeholder\n", groupIndex);
        DataWin* empty = (DataWin*) safeCalloc(1, sizeof(DataWin));
        while ((int32_t) arrlen(audio->audioGroups) < groupIndex) {
            arrput(audio->audioGroups, empty);
        }
        arrput(audio->audioGroups, empty);
        return;
    }

    char* path = ma->fileSystem->vtable->resolvePath(ma->fileSystem, buf);
    DataWin* audioGroup = DataWin_parse(path, (DataWinParserOptions) {
        .parseAudo = true,
    });
    free(path);
    arrput(audio->audioGroups, audioGroup);
}

static bool maGroupIsLoaded(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t groupIndex) {
    return (arrlen(audio->audioGroups) > groupIndex);
}

// ===[ Audio Streams ]===

static int32_t maCreateStream(AudioSystem* audio, const char* filename) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    // Find a free stream slot
    int32_t freeSlot = -1;
    repeat(MAX_AUDIO_STREAMS, i) {
        if (!ma->streams[i].active) {
            freeSlot = (int32_t) i;
            break;
        }
    }

    if (0 > freeSlot) {
        fprintf(stderr, "Audio: No free stream slots for '%s'\n", filename);
        return -1;
    }

    char* resolved = ma->fileSystem->vtable->resolvePath(ma->fileSystem, filename);
    if (resolved == nullptr) {
        fprintf(stderr, "Audio: Could not resolve path for stream '%s'\n", filename);
        return -1;
    }

    ma->streams[freeSlot].active = true;
    ma->streams[freeSlot].filePath = resolved;

    int32_t streamIndex = AUDIO_STREAM_INDEX_BASE + freeSlot;
    fprintf(stderr, "Audio: Created stream %d for '%s' -> '%s'\n", streamIndex, filename, resolved);
    return streamIndex;
}

static bool maDestroyStream(AudioSystem* audio, int32_t streamIndex) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    int32_t slotIndex = streamIndex - AUDIO_STREAM_INDEX_BASE;
    if (0 > slotIndex || slotIndex >= MAX_AUDIO_STREAMS) {
        fprintf(stderr, "Audio: Invalid stream index %d for destroy\n", streamIndex);
        return false;
    }

    AudioStreamEntry* entry = &ma->streams[slotIndex];
    if (!entry->active) return false;

    // Stop all sound instances that were playing this stream
    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active && inst->soundIndex == streamIndex) {
            releaseInstanceEx(ma, inst);
        }
    }

    free(entry->filePath);
    entry->filePath = nullptr;
    entry->active = false;
    fprintf(stderr, "Audio: Destroyed stream %d\n", streamIndex);
    return true;
}

// ===[ Vtable ]===

static AudioSystemVtable AlAudioSystemVtable = {
    .init = maInit,
    .destroy = maDestroy,
    .update = maUpdate,
    .playSound = maPlaySound,
    .stopSound = maStopSound,
    .stopAll = maStopAll,
    .isPlaying = maIsPlaying,
    .pauseSound = maPauseSound,
    .resumeSound = maResumeSound,
    .pauseAll = maPauseAll,
    .resumeAll = maResumeAll,
    .setSoundGain = maSetSoundGain,
    .getSoundGain = maGetSoundGain,
    .setSoundPitch = maSetSoundPitch,
    .getSoundPitch = maGetSoundPitch,
    .getTrackPosition = maGetTrackPosition,
    .setTrackPosition = maSetTrackPosition,
    .getSoundLength = maGetSoundLength,
    .setMasterGain = maSetMasterGain,
    .setChannelCount = maSetChannelCount,
    .groupLoad = maGroupLoad,
    .groupIsLoaded = maGroupIsLoaded,
    .createStream = maCreateStream,
    .destroyStream = maDestroyStream,
};

// ===[ Memory Pressure ]===

void AlAudioSystem_handleMemoryWarning(AlAudioSystem* ma) {
    if (ma == NULL) return;

    uint32_t stoppedInstances = 0;
    uint32_t evicted = 0;
    uint32_t freedBytes = 0;

    // Phase 1: Force-stop ALL non-streaming sound instances.
    // On the iPod Touch 2G this is the last chance before jetsam kills
    // us, so we sacrifice SFX to stay alive. Streaming music survives
    // (it uses tiny ring buffers, not large cached PCM).
    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (!inst->active) continue;
        if (inst->streaming) continue;
        releaseInstanceEx(ma, inst);
        stoppedInstances++;
    }

    // Phase 2: Now that all non-streaming refs are gone, nuke the
    // entire buffer cache.  Every entry should have refCount==0 after
    // phase 1 (streaming instances don't use the cache).
    repeat(AL_BUFFER_CACHE_SIZE, i) {
        BufferCacheEntry* ce = &ma->bufferCache[i];
        if (!ce->inUse) continue;
        alDeleteBuffers(1, &ce->alBuffer);
        freedBytes += ce->pcmBytes;
        ce->inUse = false;
        evicted++;
    }
    ma->cacheResidentBytes = 0;

    fprintf(stderr, "[audio] memory warning: stopped %u instances, evicted %u cached buffers "
            "(%u bytes freed)\n",
            stoppedInstances, evicted, freedBytes);
}

// ===[ Lifecycle ]===

AlAudioSystem* AlAudioSystem_create(void) {
    AlAudioSystem* ma = safeCalloc(1, sizeof(AlAudioSystem));
    ma->base.vtable = &AlAudioSystemVtable;
    return ma;
}
