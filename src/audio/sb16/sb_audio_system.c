// This code was written ENTIRELY by claude (AI), with
// zero real manual modification from any humans. That
// means this code is in the public domain, it also means
// the code is probably terrible. Good luck to anyone
// who needs to read through this. This has been tested
// to work in DOSBox-X, and doesn't break on my laptop
// running FreeDOS without a compatible soundcard.

// Sound Blaster 16 audio backend for DJGPP/DOS
// Targets SB16 (DSP 4.x) with auto-detection via BLASTER env var.
// Audio is mixed in software to a double-buffered DMA ring buffer.
// OGG/embedded sounds are decoded to 16-bit stereo PCM at load time
// using stb_vorbis

#include "sb_audio_system.h"
#include "data_win.h"
#include "utils.h"

#include "stb_vorbis.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/farptr.h>
#include <sys/movedata.h>
#include <pc.h>

#include "stb_ds.h"

// ─── SB16 Constants ──────────────────────────────────────────────────────────

#define SB_SAMPLE_RATE      22050   // output sample rate (Hz)
#define SB_CHANNELS         2       // stereo
#define SB_BITS             16

// DMA buffer: two halves, we fill one while the card plays the other
#define SB_HALF_FRAMES      2048                                    // frames per half
#define SB_HALF_BYTES       (SB_HALF_FRAMES * SB_CHANNELS * 2)     // bytes per half
#define SB_BUF_BYTES        (SB_HALF_BYTES * 2)                    // total DMA buffer

// DMA channel and IRQ for SB16 defaults (overridden by BLASTER env)
#define SB_DEFAULT_PORT     0x220
#define SB_DEFAULT_IRQ      5
#define SB_DEFAULT_DMA8     1
#define SB_DEFAULT_DMA16    5

// DSP commands
#define SB_DSP_RESET        0x01
#define SB_DSP_SPEAKER_ON   0xD1
#define SB_DSP_SPEAKER_OFF  0xD3
#define SB_DSP_SET_RATE     0x41   // SB16: set output sample rate
#define SB_DSP_OUT_16BIT    0xB6   // 16-bit auto-init DMA output
#define SB_DSP_MODE_STEREO  0x30   // stereo + signed

// ─── Hardware State ───────────────────────────────────────────────────────────

static struct {
    uint16_t port;
    uint8_t  irq;
    uint8_t  dma8;
    uint8_t  dma16;
} sb;

// DMA buffer in DOS conventional memory (below 1MB)
static uint32_t           dma_phys;     // physical address of DMA buffer
static uint32_t           dma_selector;
static volatile int       dma_half;     // which half is being filled (0 or 1)
static volatile bool      sb_irq_fired;

// IRQ handler
static _go32_dpmi_seginfo old_irq_handler, new_irq_handler;

// Mix scratch buffer (one half worth of frames, float for precision)
static float mix_buf[SB_HALF_FRAMES * SB_CHANNELS];

// Global pointer back to the audio system for the IRQ handler
static SbAudioSystem *g_sb = NULL;

// ─── DSP I/O ─────────────────────────────────────────────────────────────────

static void dsp_write(uint8_t val) {
    for (int i = 0; i < 100000; i++) {
        if (!(inportb(sb.port + 0xC) & 0x80)) {
            outportb(sb.port + 0xC, val);
            return;
        }
    }
    // Timed out — no card present, ignore
}

static uint8_t dsp_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (inportb(sb.port + 0xE) & 0x80)
            return inportb(sb.port + 0xA);
    }
    return 0xFF; // timed out
}

static bool dsp_reset(void) {
    outportb(sb.port + 0x6, 1);
    for (volatile int i = 0; i < 100; i++) {}
    outportb(sb.port + 0x6, 0);
    for (int i = 0; i < 100000; i++) {
        if ((inportb(sb.port + 0xE) & 0x80) && inportb(sb.port + 0xA) == 0xAA)
            return true;
    }
    return false;
}

// ─── DMA Setup ───────────────────────────────────────────────────────────────

static void dma16_program(uint32_t phys, uint32_t length_bytes) {
    uint8_t  ch    = sb.dma16;
    // 16-bit DMA addresses are in words, not bytes
    uint32_t addr  = phys >> 1;
    uint8_t  page  = (phys >> 16) & 0xFF;
    uint16_t ofs   = addr & 0xFFFF;
    uint16_t count = (uint16_t)((length_bytes / 2) - 1); // counts words

    // Page ports for channels 0-7 (8-bit 0-3, 16-bit 4-7)
    static const uint8_t page_ports[8] = {
        0x87, 0x83, 0x81, 0x82,   // channels 0-3
        0x8F, 0x8B, 0x89, 0x8A    // channels 4-7
    };

    // Mask channel
    outportb(0xD4, 4 | (ch & 3));
    // Clear byte-pointer flip-flop
    outportb(0xD8, 0);
    // Mode: auto-init, increment, read, block (0x58 | channel)
    outportb(0xD6, 0x58 | (ch & 3));
    // Address low, high (word address)
    uint8_t addr_port = 0xC0 + (ch & 3) * 4;
    outportb(addr_port,  ofs & 0xFF);
    outportb(addr_port, (ofs >> 8) & 0xFF);
    // Page register
    outportb(page_ports[ch & 7], page);
    // Count low, high
    uint8_t cnt_port = 0xC2 + (ch & 3) * 4;
    outportb(cnt_port,  count & 0xFF);
    outportb(cnt_port, (count >> 8) & 0xFF);
    // Unmask channel
    outportb(0xD4, ch & 3);
}

// ─── Mixing ──────────────────────────────────────────────────────────────────

static void mix_half(SbAudioSystem *ma, int half) {
    // Clear mix buffer
    memset(mix_buf, 0, sizeof(mix_buf));

    for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++) {
        SbSoundInstance *inst = &ma->instances[i];
        if (!inst->active || !inst->samples || inst->numFrames == 0) continue;

        float gain = inst->currentGain;

        for (int f = 0; f < SB_HALF_FRAMES; f++) {
            if (inst->cursor >= inst->numFrames) {
                if (inst->loop) {
                    inst->cursor = 0;
                    inst->pitchAccum = 0.0f;
                } else {
                    inst->active = false;
                    break;
                }
            }

            uint32_t pos   = inst->cursor;
            float    Left  = inst->samples[pos * 2 + 0] * gain;
            float    Right = inst->samples[pos * 2 + 1] * gain;

            mix_buf[f * 2 + 0] += Left;
            mix_buf[f * 2 + 1] += Right;

            // Advance cursor with pitch
            inst->pitchAccum += inst->pitch;
            uint32_t advance = (uint32_t)inst->pitchAccum;
            inst->pitchAccum -= advance;
            inst->cursor += advance;
        }
    }

    // Convert float mix to int16, clamp, write to DMA buffer via selector
    uint32_t dma_offset = half * SB_HALF_BYTES;
    for (int s = 0; s < SB_HALF_FRAMES * SB_CHANNELS; s++) {
        float v = mix_buf[s];
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        int16_t sample = (int16_t)v;
        _farpokew(dma_selector, dma_offset + s * 2, (uint16_t)sample);
    }
}

// ─── IRQ Handler ─────────────────────────────────────────────────────────────

static void sb_irq_handler(void) {
    // Acknowledge SB16 16-bit IRQ
    inportb(sb.port + 0xF);
    // Acknowledge PIC
    if (sb.irq >= 8) outportb(0xA0, 0x20);
    outportb(0x20, 0x20);

    // Fill the half that just finished playing
    if (g_sb) mix_half(g_sb, dma_half);
    dma_half ^= 1;
}

static void install_irq(void) {
    int vec = (sb.irq < 8) ? (sb.irq + 8) : (sb.irq - 8 + 0x70);
    _go32_dpmi_get_protected_mode_interrupt_vector(vec, &old_irq_handler);
    new_irq_handler.pm_offset   = (int)sb_irq_handler;
    new_irq_handler.pm_selector = _go32_my_cs();
    _go32_dpmi_allocate_iret_wrapper(&new_irq_handler);
    _go32_dpmi_set_protected_mode_interrupt_vector(vec, &new_irq_handler);

    // Unmask IRQ on PIC
    if (sb.irq < 8) {
        outportb(0x21, inportb(0x21) & ~(1 << sb.irq));
    } else {
        outportb(0xA1, inportb(0xA1) & ~(1 << (sb.irq - 8)));
        outportb(0x21, inportb(0x21) & ~(1 << 2)); // cascade
    }
}

static void uninstall_irq(void) {
    int vec = (sb.irq < 8) ? (sb.irq + 8) : (sb.irq - 8 + 0x70);
    _go32_dpmi_set_protected_mode_interrupt_vector(vec, &old_irq_handler);
    _go32_dpmi_free_iret_wrapper(&new_irq_handler);
}

// ─── BLASTER Env Parsing ─────────────────────────────────────────────────────

static void parse_blaster_env(void) {
    sb.port  = SB_DEFAULT_PORT;
    sb.irq   = SB_DEFAULT_IRQ;
    sb.dma8  = SB_DEFAULT_DMA8;
    sb.dma16 = SB_DEFAULT_DMA16;

    const char *blaster = getenv("BLASTER");
    if (!blaster) {
        fprintf(stderr, "Audio: BLASTER env not set, using defaults\n");
        return;
    }

    const char *p = blaster;
    while (*p) {
        while (*p == ' ') p++;
        char key = *p++;
        int  val = (int)strtol(p, (char**)&p, 16);
        switch (key) {
            case 'A': case 'a': sb.port  = (uint16_t)val; break;
            case 'I': case 'i': sb.irq   = (uint8_t)val;  break;
            case 'D': case 'd': sb.dma8  = (uint8_t)val;  break;
            case 'H': case 'h': sb.dma16 = (uint8_t)val;  break;
        }
    }
    fprintf(stderr, "Audio: BLASTER port=0x%X IRQ=%d DMA8=%d DMA16=%d\n",
            sb.port, sb.irq, sb.dma8, sb.dma16);
}

// ─── PCM Decoding ─────────────────────────────────────────────────────────────

// Resample interleaved stereo int16 from srcRate to SB_SAMPLE_RATE.
// Returns new buffer (caller owns), sets *outFrames.
static int16_t *resample_stereo(int16_t *src, uint32_t srcFrames, int srcRate, uint32_t *outFrames) {
    if (srcRate == SB_SAMPLE_RATE) {
        *outFrames = srcFrames;
        return src;
    }
    double ratio = (double)srcRate / SB_SAMPLE_RATE;
    *outFrames = (uint32_t)(srcFrames / ratio);
    int16_t *out = (int16_t*)safeMalloc(*outFrames * 2 * sizeof(int16_t));
    for (uint32_t o = 0; o < *outFrames; o++) {
        double srcPos = o * ratio;
        uint32_t s0 = (uint32_t)srcPos;
        uint32_t s1 = s0 + 1 < srcFrames ? s0 + 1 : s0;
        float t = (float)(srcPos - s0);
        for (int c = 0; c < 2; c++) {
            float v = src[s0 * 2 + c] * (1.0f - t) + src[s1 * 2 + c] * t;
            out[o * 2 + c] = (int16_t)v;
        }
    }
    free(src);
    return out;
}

// Expand mono to stereo in place (reallocates).
static int16_t *mono_to_stereo(int16_t *mono, uint32_t frames) {
    int16_t *stereo = (int16_t*)safeMalloc(frames * 2 * sizeof(int16_t));
    for (uint32_t i = 0; i < frames; i++) {
        stereo[i * 2 + 0] = mono[i];
        stereo[i * 2 + 1] = mono[i];
    }
    free(mono);
    return stereo;
}

// Decode WAV from memory into 16-bit stereo PCM at SB_SAMPLE_RATE.
static int16_t *decode_wav_memory(const uint8_t *data, uint32_t dataSize, uint32_t *numFrames) {
    if (dataSize < 44) return NULL;

    // Minimal RIFF/WAV parser
    #define RL16(p) ((uint16_t)((p)[0] | ((p)[1] << 8)))
    #define RL32(p) ((uint32_t)((p)[0] | ((p)[1] << 8) | ((p)[2] << 16) | ((p)[3] << 24)))

    if (RL32(data) != 0x46464952) return NULL;      // "RIFF"
    if (RL32(data + 8) != 0x45564157) return NULL;  // "WAVE"

    uint16_t audioFmt = 0, channels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0;
    const uint8_t *pcmData = NULL;
    uint32_t pcmBytes = 0;

    // Walk chunks
    uint32_t pos = 12;
    while (pos + 8 <= dataSize) {
        uint32_t chunkId   = RL32(data + pos);
        uint32_t chunkSize = RL32(data + pos + 4);
        pos += 8;
        if (pos + chunkSize > dataSize) break;

        if (chunkId == 0x20746D66) { // "fmt "
            if (chunkSize < 16) return NULL;
            audioFmt     = RL16(data + pos);      // 1=PCM, 3=float
            channels     = RL16(data + pos + 2);
            sampleRate   = RL32(data + pos + 4);
            bitsPerSample = RL16(data + pos + 14);
        } else if (chunkId == 0x61746164) { // "data"
            pcmData  = data + pos;
            pcmBytes = chunkSize;
        }
        pos += (chunkSize + 1) & ~1; // chunks are word-aligned
    }

    #undef RL16
    #undef RL32

    if (!pcmData || channels == 0 || sampleRate == 0) return NULL;
    if (audioFmt != 1) {
        fprintf(stderr, "Audio: unsupported WAV format %d (only PCM supported)\n", audioFmt);
        return NULL;
    }

    uint32_t bytesPerSample = bitsPerSample / 8;
    uint32_t frames = pcmBytes / (bytesPerSample * channels);

    // Convert to 16-bit
    int16_t *buf = (int16_t*)safeMalloc(frames * channels * sizeof(int16_t));
    for (uint32_t i = 0; i < frames * channels; i++) {
        if (bitsPerSample == 8) {
            // 8-bit WAV is unsigned
            buf[i] = ((int16_t)pcmData[i] - 128) << 8;
        } else if (bitsPerSample == 16) {
            buf[i] = (int16_t)(pcmData[i * 2] | (pcmData[i * 2 + 1] << 8));
        } else if (bitsPerSample == 24) {
            int32_t s = pcmData[i*3] | (pcmData[i*3+1] << 8) | (pcmData[i*3+2] << 16);
            if (s & 0x800000) s |= 0xFF000000; // sign extend
            buf[i] = (int16_t)(s >> 8);
        } else {
            free(buf);
            fprintf(stderr, "Audio: unsupported WAV bit depth %d\n", bitsPerSample);
            return NULL;
        }
    }

    // Mono to stereo
    if (channels == 1) {
        buf = mono_to_stereo(buf, frames);
    }

    // Resample
    buf = resample_stereo(buf, frames, (int)sampleRate, numFrames);
    return buf;
}

// Decode OGG from memory into 16-bit stereo PCM at SB_SAMPLE_RATE.
static int16_t *decode_ogg_memory(const uint8_t *data, uint32_t dataSize, uint32_t *numFrames) {
    int channels, sampleRate;
    short *decoded = NULL;
    int frames = stb_vorbis_decode_memory(data, (int)dataSize,
                                          &channels, &sampleRate, &decoded);
    if (frames <= 0 || !decoded) return NULL;

    int16_t *stereo;
    if (channels == 1) {
        stereo = mono_to_stereo((int16_t*)decoded, (uint32_t)frames);
    } else {
        stereo = (int16_t*)decoded;
    }

    stereo = resample_stereo(stereo, (uint32_t)frames, sampleRate, numFrames);
    return stereo;
}

// Auto-detect format and decode from memory.
static int16_t *decode_audio_memory(const uint8_t *data, uint32_t dataSize, uint32_t *numFrames) {
    if (dataSize >= 4 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F')
        return decode_wav_memory(data, dataSize, numFrames);
    return decode_ogg_memory(data, dataSize, numFrames);
}

// Decode from file (auto-detect format).
static int16_t *decode_audio_file(const char *path, uint32_t *numFrames) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t*)safeMalloc(sz);
    fread(buf, 1, sz, f);
    fclose(f);
    int16_t *pcm = decode_audio_memory(buf, (uint32_t)sz, numFrames);
    free(buf);
    return pcm;
}

// ─── Asset Loading ────────────────────────────────────────────────────────────

static SbSoundAsset *get_asset(SbAudioSystem *ma, DataWin *dw, int32_t soundIndex) {
    if ((uint32_t)soundIndex >= ma->assetCount) return NULL;
    SbSoundAsset *asset = &ma->assets[soundIndex];
    if (asset->loaded) return asset;

    Sound *sound = &dw->sond.sounds[soundIndex];
    bool isEmbedded   = (sound->flags & 0x01) != 0;
    bool isCompressed = (sound->flags & 0x02) != 0;

    fprintf(stderr, "Audio: get_asset soundIndex=%d name='%s' flags=0x%X embedded=%d compressed=%d audioGroup=%d audioFile=%d\n",
            soundIndex, sound->name, sound->flags, isEmbedded, isCompressed, sound->audioGroup, sound->audioFile);

    if (isEmbedded || isCompressed) {
        if (sound->audioFile < 0 ||
            (uint32_t)sound->audioFile >= ma->base.audioGroups[sound->audioGroup]->audo.count) {
            fprintf(stderr, "Audio: invalid audioFile index for '%s'\n", sound->name);
            return NULL;
        }
        AudioEntry *entry = &ma->base.audioGroups[sound->audioGroup]->audo.entries[sound->audioFile];
        fprintf(stderr, "Audio: entry data=%p dataSize=%u magic=%02X %02X %02X %02X\n",
                (void*)entry->data, entry->dataSize,
                entry->dataSize > 0 ? entry->data[0] : 0,
                entry->dataSize > 1 ? entry->data[1] : 0,
                entry->dataSize > 2 ? entry->data[2] : 0,
                entry->dataSize > 3 ? entry->data[3] : 0);
        asset->samples = decode_audio_memory(entry->data, entry->dataSize, &asset->numFrames);
    } else {
        // External file
        const char *file = sound->file;
        bool hasExt = (strchr(file, '.') != NULL);
        char filename[512];
        if (hasExt) snprintf(filename, sizeof(filename), "%s", file);
        else         snprintf(filename, sizeof(filename), "%s.ogg", file);
        char *path = ma->fileSystem->vtable->resolvePath(ma->fileSystem, filename);
        if (!path) {
            fprintf(stderr, "Audio: could not resolve path for '%s'\n", sound->name);
            return NULL;
        }
        asset->samples = decode_audio_file(path, &asset->numFrames);
        free(path);
    }

    if (!asset->samples) {
        fprintf(stderr, "Audio: failed to decode '%s'\n", sound->name);
        return NULL;
    }

    asset->loaded = true;
    return asset;
}

// ─── Instance Helpers ─────────────────────────────────────────────────────────

static SbSoundInstance *find_free_slot(SbAudioSystem *ma) {
    // First pass: inactive slot
    for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++) {
        if (!ma->instances[i].active) return &ma->instances[i];
    }
    // Second pass: evict lowest-priority ended sound
    SbSoundInstance *best = NULL;
    for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++) {
        SbSoundInstance *inst = &ma->instances[i];
        if (inst->cursor >= inst->numFrames && !inst->loop) {
            if (!best || best->priority > inst->priority) best = inst;
        }
    }
    if (best) {
        if (best->ownssamples) free(best->samples);
        best->active = false;
    }
    return best;
}

static SbSoundInstance *find_by_id(SbAudioSystem *ma, int32_t instanceId) {
    int32_t slot = instanceId - SB_SOUND_INSTANCE_ID_BASE;
    if (slot < 0 || slot >= SB_MAX_SOUND_INSTANCES) return NULL;
    SbSoundInstance *inst = &ma->instances[slot];
    if (!inst->active || inst->instanceId != instanceId) return NULL;
    return inst;
}

static bool sb_initialized = false;

// ─── Vtable Implementations ───────────────────────────────────────────────────

static void sbInit(AudioSystem *audio, DataWin *dataWin, FileSystem *fileSystem) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;
    arrput(ma->base.audioGroups, dataWin);
    ma->fileSystem = fileSystem;

    // Allocate asset table
    ma->assetCount = dataWin->sond.count;
    ma->assets     = (SbSoundAsset*)safeCalloc(ma->assetCount, sizeof(SbSoundAsset));

    parse_blaster_env();

    if (!dsp_reset()) {
        fprintf(stderr, "Audio: DSP reset failed — no Sound Blaster at port 0x%X, running silent\n", sb.port);
        return;
    }

    // Read DSP version
    dsp_write(0xE1);
    uint8_t major = dsp_read();
    uint8_t minor = dsp_read();
    fprintf(stderr, "Audio: SB DSP version %d.%d\n", major, minor);
    if (major < 4) {
        fprintf(stderr, "Audio: Need SB16 (DSP 4.x), got %d.%d\n", major, minor);
        return;
    }

    // Allocate DMA buffer in conventional memory (below 1MB).
    // Must not cross a 64K physical boundary.
    // Allocate 2x size so we can find an aligned region within.
    {
        int segment, selector;
        uint32_t allocParas = (SB_BUF_BYTES * 2 + 15) / 16;
        segment = __dpmi_allocate_dos_memory(allocParas, &selector);
        if (segment == -1) {
            fprintf(stderr, "Audio: Failed to allocate DMA buffer\n");
            return;
        }
        uint32_t base = (uint32_t)segment * 16;
        // Use base if it doesn't cross a 64K boundary, else round up
        uint32_t aligned;
        if ((base & 0xFFFF0000) == ((base + SB_BUF_BYTES - 1) & 0xFFFF0000)) {
            aligned = base;
        } else {
            aligned = (base + 0xFFFF) & ~0xFFFF;
        }
        dma_phys     = aligned;
        dma_selector = selector;
        // Point selector at the aligned region
        __dpmi_set_segment_base_address(selector, aligned);
        __dpmi_set_segment_limit(selector, SB_BUF_BYTES - 1);
    }

    // Zero the DMA buffer
    for (uint32_t i = 0; i < SB_BUF_BYTES; i += 2)
        _farpokew(dma_selector, i, 0);

    // Pre-fill both halves with silence
    dma_half = 0;

    // Install IRQ
    g_sb = ma;
    install_irq();

    // Enable speaker
    dsp_write(SB_DSP_SPEAKER_ON);

    // Set sample rate
    dsp_write(SB_DSP_SET_RATE);
    dsp_write((SB_SAMPLE_RATE >> 8) & 0xFF);
    dsp_write( SB_SAMPLE_RATE       & 0xFF);

    // Program DMA
    dma16_program(dma_phys, SB_BUF_BYTES);

    // Start auto-init 16-bit stereo signed DMA playback
    dsp_write(SB_DSP_OUT_16BIT);
    dsp_write(SB_DSP_MODE_STEREO);
    uint16_t count = SB_HALF_FRAMES * SB_CHANNELS - 1;
    dsp_write( count       & 0xFF);
    dsp_write((count >> 8) & 0xFF);

    fprintf(stderr, "Audio: SB16 initialized, %d Hz stereo 16-bit\n", SB_SAMPLE_RATE);
    sb_initialized = true;
}

static void sbDestroy(AudioSystem *audio) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;

    if (sb_initialized) {
        // Stop DMA
        dsp_write(SB_DSP_SPEAKER_OFF);
        dsp_reset();
        uninstall_irq();
        g_sb = NULL;
        __dpmi_free_dos_memory(dma_selector);
        sb_initialized = false;
    }

    // Free instances
    for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++) {
        if (ma->instances[i].active && ma->instances[i].ownssamples)
            free(ma->instances[i].samples);
    }

    // Free assets
    for (uint32_t i = 0; i < ma->assetCount; i++) {
        if (ma->assets[i].loaded) free(ma->assets[i].samples);
    }
    free(ma->assets);

    // Free streams
    for (int i = 0; i < SB_MAX_AUDIO_STREAMS; i++) {
        if (ma->streams[i].active) {
            free(ma->streams[i].filePath);
            if (ma->streams[i].samples) free(ma->streams[i].samples);
        }
    }

    // Free audio groups (skip 0, owned by caller)
    if (arrlen(ma->base.audioGroups) > 1) {
        for (int32_t i = 1; i < (int32_t)arrlen(ma->base.audioGroups); i++)
            DataWin_free(ma->base.audioGroups[i]);
    }
    arrfree(ma->base.audioGroups);

    free(ma);
}

static void sbUpdate(AudioSystem *audio, float deltaTime) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;

    for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++) {
        SbSoundInstance *inst = &ma->instances[i];
        if (!inst->active) continue;

        // Gain fading
        if (inst->fadeTimeRemaining > 0.0f) {
            inst->fadeTimeRemaining -= deltaTime;
            if (inst->fadeTimeRemaining <= 0.0f) {
                inst->fadeTimeRemaining = 0.0f;
                inst->currentGain = inst->targetGain;
            } else {
                float t = 1.0f - (inst->fadeTimeRemaining / inst->fadeTotalTime);
                inst->currentGain = inst->startGain + (inst->targetGain - inst->startGain) * t;
            }
        }
    }
}

static int32_t sbPlaySound(AudioSystem *audio, int32_t soundIndex, int32_t priority, bool loop) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;

    int16_t  *samples   = NULL;
    uint32_t  numFrames = 0;
    bool      ownsSamples = false;

    bool isStream = (soundIndex >= SB_AUDIO_STREAM_INDEX_BASE);

    if (isStream) {
        int32_t slot = soundIndex - SB_AUDIO_STREAM_INDEX_BASE;
        if (slot < 0 || slot >= SB_MAX_AUDIO_STREAMS || !ma->streams[slot].active) {
            fprintf(stderr, "Audio: invalid stream index %d\n", soundIndex);
            return -1;
        }
        SbStreamEntry *se = &ma->streams[slot];
        if (!se->samples) {
            se->samples = decode_audio_file(se->filePath, &se->numFrames);
            if (!se->samples) {
                fprintf(stderr, "Audio: failed to decode stream '%s'\n", se->filePath);
                return -1;
            }
        }
        samples    = se->samples;
        numFrames  = se->numFrames;
        ownsSamples = false;
    } else {
        DataWin *dw = ma->base.audioGroups[0];
        if (soundIndex < 0 || (uint32_t)soundIndex >= dw->sond.count) {
            fprintf(stderr, "Audio: invalid sound index %d\n", soundIndex);
            return -1;
        }
        SbSoundAsset *asset = get_asset(ma, dw, soundIndex);
        if (!asset) {
            fprintf(stderr, "Audio: get_asset returned NULL for soundIndex=%d\n", soundIndex);
            return -1;
        }
        samples    = asset->samples;
        numFrames  = asset->numFrames;
        ownsSamples = false;
    }

    SbSoundInstance *slot = find_free_slot(ma);
    if (!slot) {
        fprintf(stderr, "Audio: no free sound slots\n");
        return -1;
    }

    int32_t slotIndex = (int32_t)(slot - ma->instances);
    float volume = 1.0f; // streams and sounds default to 1.0; game sets gain separately

    if (!isStream) {
        DataWin *dw = ma->base.audioGroups[0];
        Sound *sound = &dw->sond.sounds[soundIndex];
        volume = sound->volume;
        if (volume <= 0.0f) volume = 1.0f; // treat 0 as unset, default to full volume
    }

    // Fill slot fully before marking active — IRQ handler checks active first
    fprintf(stderr, "Audio: activating slot %d soundIndex=%d samples=%p numFrames=%u volume=%.3f loop=%d\n",
            slotIndex, soundIndex, (void*)samples, numFrames, volume, loop);
    slot->soundIndex       = soundIndex;
    slot->instanceId       = SB_SOUND_INSTANCE_ID_BASE + slotIndex;
    slot->samples          = samples;
    slot->numFrames        = numFrames;
    slot->cursor           = 0;
    slot->loop             = loop;
    slot->ownssamples      = ownsSamples;
    slot->gain             = volume;
    slot->pitch            = 1.0f;
    slot->pitchAccum       = 0.0f;
    slot->currentGain      = volume;
    slot->targetGain       = volume;
    slot->startGain        = volume;
    slot->fadeTimeRemaining = 0.0f;
    slot->fadeTotalTime    = 0.0f;
    slot->priority         = priority;
    // Set active last so IRQ never sees a partially-initialized slot
    __asm__ volatile ("cli");
    slot->active = true;
    __asm__ volatile ("sti");

    ma->nextInstanceCounter++;
    return slot->instanceId;
}

static void stop_instance(SbSoundInstance *inst) {
    inst->active = false;
    if (inst->ownssamples) {
        free(inst->samples);
        inst->samples = NULL;
    }
}

static void sbStopSound(AudioSystem *audio, int32_t soundOrInstance) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;
    if (soundOrInstance >= SB_SOUND_INSTANCE_ID_BASE) {
        SbSoundInstance *inst = find_by_id(ma, soundOrInstance);
        if (inst) stop_instance(inst);
    } else {
        for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++) {
            SbSoundInstance *inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance)
                stop_instance(inst);
        }
    }
}

static void sbStopAll(AudioSystem *audio) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;
    for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++) {
        if (ma->instances[i].active) stop_instance(&ma->instances[i]);
    }
}

static bool sbIsPlaying(AudioSystem *audio, int32_t soundOrInstance) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;
    if (soundOrInstance >= SB_SOUND_INSTANCE_ID_BASE) {
        SbSoundInstance *inst = find_by_id(ma, soundOrInstance);
        return inst != NULL && inst->active;
    }
    for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++) {
        SbSoundInstance *inst = &ma->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) return true;
    }
    return false;
}

static void sbPauseSound(AudioSystem *audio, int32_t soundOrInstance) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;
    if (soundOrInstance >= SB_SOUND_INSTANCE_ID_BASE) {
        SbSoundInstance *inst = find_by_id(ma, soundOrInstance);
        if (inst) inst->active = false;
    } else {
        for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++) {
            SbSoundInstance *inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance)
                inst->active = false;
        }
    }
}

static void sbResumeSound(AudioSystem *audio, int32_t soundOrInstance) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;
    if (soundOrInstance >= SB_SOUND_INSTANCE_ID_BASE) {
        SbSoundInstance *inst = find_by_id(ma, soundOrInstance);
        if (inst) inst->active = true;
    } else {
        for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++) {
            SbSoundInstance *inst = &ma->instances[i];
            if (inst->soundIndex == soundOrInstance && inst->samples)
                inst->active = true;
        }
    }
}

static void sbPauseAll(AudioSystem *audio) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;
    for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++)
        if (ma->instances[i].active) ma->instances[i].active = false;
}

static void sbResumeAll(AudioSystem *audio) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;
    for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++)
        if (ma->instances[i].samples) ma->instances[i].active = true;
}

static void sbSetSoundGain(AudioSystem *audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;
    if (soundOrInstance >= SB_SOUND_INSTANCE_ID_BASE) {
        SbSoundInstance *inst = find_by_id(ma, soundOrInstance);
        if (!inst) return;
        if (timeMs == 0) {
            inst->currentGain = gain; inst->targetGain = gain; inst->fadeTimeRemaining = 0.0f;
        } else {
            inst->startGain = inst->currentGain;
            inst->targetGain = gain;
            inst->fadeTotalTime = (float)timeMs / 1000.0f;
            inst->fadeTimeRemaining = inst->fadeTotalTime;
        }
    } else {
        for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++) {
            SbSoundInstance *inst = &ma->instances[i];
            if (!inst->active || inst->soundIndex != soundOrInstance) continue;
            if (timeMs == 0) {
                inst->currentGain = gain; inst->targetGain = gain; inst->fadeTimeRemaining = 0.0f;
            } else {
                inst->startGain = inst->currentGain;
                inst->targetGain = gain;
                inst->fadeTotalTime = (float)timeMs / 1000.0f;
                inst->fadeTimeRemaining = inst->fadeTotalTime;
            }
        }
    }
}

static float sbGetSoundGain(AudioSystem *audio, int32_t soundOrInstance) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;
    if (soundOrInstance >= SB_SOUND_INSTANCE_ID_BASE) {
        SbSoundInstance *inst = find_by_id(ma, soundOrInstance);
        return inst ? inst->currentGain : 0.0f;
    }
    for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++) {
        SbSoundInstance *inst = &ma->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) return inst->currentGain;
    }
    return 0.0f;
}

static void sbSetSoundPitch(AudioSystem *audio, int32_t soundOrInstance, float pitch) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;
    if (soundOrInstance >= SB_SOUND_INSTANCE_ID_BASE) {
        SbSoundInstance *inst = find_by_id(ma, soundOrInstance);
        if (inst) inst->pitch = pitch;
    } else {
        for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++) {
            SbSoundInstance *inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) inst->pitch = pitch;
        }
    }
}

static float sbGetSoundPitch(AudioSystem *audio, int32_t soundOrInstance) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;
    if (soundOrInstance >= SB_SOUND_INSTANCE_ID_BASE) {
        SbSoundInstance *inst = find_by_id(ma, soundOrInstance);
        return inst ? inst->pitch : 1.0f;
    }
    for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++) {
        SbSoundInstance *inst = &ma->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) return inst->pitch;
    }
    return 1.0f;
}

static float sbGetTrackPosition(AudioSystem *audio, int32_t soundOrInstance) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;
    if (soundOrInstance >= SB_SOUND_INSTANCE_ID_BASE) {
        SbSoundInstance *inst = find_by_id(ma, soundOrInstance);
        return inst ? (float)inst->cursor / SB_SAMPLE_RATE : 0.0f;
    }
    for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++) {
        SbSoundInstance *inst = &ma->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance)
            return (float)inst->cursor / SB_SAMPLE_RATE;
    }
    return 0.0f;
}

static void sbSetTrackPosition(AudioSystem *audio, int32_t soundOrInstance, float positionSeconds) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;
    uint32_t frame = (uint32_t)(positionSeconds * SB_SAMPLE_RATE);
    if (soundOrInstance >= SB_SOUND_INSTANCE_ID_BASE) {
        SbSoundInstance *inst = find_by_id(ma, soundOrInstance);
        if (inst) inst->cursor = frame < inst->numFrames ? frame : 0;
    } else {
        for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++) {
            SbSoundInstance *inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance)
                inst->cursor = frame < inst->numFrames ? frame : 0;
        }
    }
}

static float sbGetSoundLength(AudioSystem *audio, int32_t soundOrInstance) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;
    if (soundOrInstance >= SB_SOUND_INSTANCE_ID_BASE) {
        SbSoundInstance *inst = find_by_id(ma, soundOrInstance);
        return inst ? (float)inst->numFrames / SB_SAMPLE_RATE : 0.0f;
    }
    // Check active instances first
    for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++) {
        SbSoundInstance *inst = &ma->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance)
            return (float)inst->numFrames / SB_SAMPLE_RATE;
    }
    // Load asset if needed
    if (soundOrInstance < 0 || (uint32_t)soundOrInstance >= ma->assetCount) return 0.0f;
    DataWin *dw = ma->base.audioGroups[0];
    SbSoundAsset *asset = get_asset(ma, dw, soundOrInstance);
    return asset ? (float)asset->numFrames / SB_SAMPLE_RATE : 0.0f;
}

static void sbSetMasterGain(AudioSystem *audio, float gain) {
    // Set mixer master volume via SB16 mixer
    (void)audio;
    uint8_t vol = (uint8_t)(gain * 255.0f);
    outportb(sb.port + 0x4, 0x22); // master volume register
    outportb(sb.port + 0x5, vol);
}

static void sbSetChannelCount(MAYBE_UNUSED AudioSystem *audio, MAYBE_UNUSED int32_t count) {
    // no-op: channel count is fixed at SB_MAX_SOUND_INSTANCES
}

static void sbGroupLoad(AudioSystem *audio, int32_t groupIndex) {
    if (groupIndex > 0) {
        int sz = snprintf(NULL, 0, "audiogroup%d.dat", groupIndex);
        char buf[sz + 1];
        snprintf(buf, sizeof(buf), "audiogroup%d.dat", groupIndex);
        DataWin *audioGroup = DataWin_parse(
            ((SbAudioSystem*)audio)->fileSystem->vtable->resolvePath(
                ((SbAudioSystem*)audio)->fileSystem, buf),
            (DataWinParserOptions){ .parseAudo = true });
        arrput(audio->audioGroups, audioGroup);
    }
}

static bool sbGroupIsLoaded(MAYBE_UNUSED AudioSystem *audio, MAYBE_UNUSED int32_t groupIndex) {
    return (arrlen(audio->audioGroups) > groupIndex);
}

static int32_t sbCreateStream(AudioSystem *audio, const char *filename) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;
    int32_t freeSlot = -1;
    for (int i = 0; i < SB_MAX_AUDIO_STREAMS; i++) {
        if (!ma->streams[i].active) { freeSlot = i; break; }
    }
    if (freeSlot < 0) {
        fprintf(stderr, "Audio: no free stream slots for '%s'\n", filename);
        return -1;
    }
    char *resolved = ma->fileSystem->vtable->resolvePath(ma->fileSystem, filename);
    if (!resolved) {
        fprintf(stderr, "Audio: could not resolve stream path '%s'\n", filename);
        return -1;
    }
    ma->streams[freeSlot].active   = true;
    ma->streams[freeSlot].filePath = resolved;
    ma->streams[freeSlot].samples  = NULL;
    ma->streams[freeSlot].numFrames = 0;
    return SB_AUDIO_STREAM_INDEX_BASE + freeSlot;
}

static bool sbDestroyStream(AudioSystem *audio, int32_t streamIndex) {
    SbAudioSystem *ma = (SbAudioSystem*)audio;
    int32_t slot = streamIndex - SB_AUDIO_STREAM_INDEX_BASE;
    if (slot < 0 || slot >= SB_MAX_AUDIO_STREAMS) return false;
    SbStreamEntry *entry = &ma->streams[slot];
    if (!entry->active) return false;
    // Stop all instances using this stream
    for (int i = 0; i < SB_MAX_SOUND_INSTANCES; i++) {
        SbSoundInstance *inst = &ma->instances[i];
        if (inst->active && inst->soundIndex == streamIndex)
            stop_instance(inst);
    }
    free(entry->filePath);
    if (entry->samples) free(entry->samples);
    entry->filePath  = NULL;
    entry->samples   = NULL;
    entry->active    = false;
    return true;
}

// ─── Vtable ───────────────────────────────────────────────────────────────────

static AudioSystemVtable sbAudioSystemVtable = {
    .init             = sbInit,
    .destroy          = sbDestroy,
    .update           = sbUpdate,
    .playSound        = sbPlaySound,
    .stopSound        = sbStopSound,
    .stopAll          = sbStopAll,
    .isPlaying        = sbIsPlaying,
    .pauseSound       = sbPauseSound,
    .resumeSound      = sbResumeSound,
    .pauseAll         = sbPauseAll,
    .resumeAll        = sbResumeAll,
    .setSoundGain     = sbSetSoundGain,
    .getSoundGain     = sbGetSoundGain,
    .setSoundPitch    = sbSetSoundPitch,
    .getSoundPitch    = sbGetSoundPitch,
    .getTrackPosition = sbGetTrackPosition,
    .setTrackPosition = sbSetTrackPosition,
    .getSoundLength   = sbGetSoundLength,
    .setMasterGain    = sbSetMasterGain,
    .setChannelCount  = sbSetChannelCount,
    .groupLoad        = sbGroupLoad,
    .groupIsLoaded    = sbGroupIsLoaded,
    .createStream     = sbCreateStream,
    .destroyStream    = sbDestroyStream,
};

// ─── Lifecycle ────────────────────────────────────────────────────────────────

SbAudioSystem *SbAudioSystem_create(void) {
    SbAudioSystem *ma = safeCalloc(1, sizeof(SbAudioSystem));
    ma->base.vtable = &sbAudioSystemVtable;
    return ma;
}
