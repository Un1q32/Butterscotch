#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <sys/farptr.h>
#include <sys/nearptr.h>

#include "common.h"
#include "input_recording.h"
#include "desktop/platformdefs.h"
#include "gettime.h"

static Runner *g_runner;
static int32_t fbWidth, fbHeight;

bool platformGetWindowSize(int32_t* outW, int32_t* outH) {
    if (!outW || !outH) return false;
    *outW = fbWidth;
    *outH = fbHeight;
    return true;
}

bool platformGetScaledWindowSize(int32_t* outW, int32_t* outH) {
    return platformGetWindowSize(outW, outH);
}

void platformGetMousePos(double *xPos, double *yPos) {
    *xPos = 0.0;
    *yPos = 0.0;
}

typedef struct {
    uint32_t physBase;
    int      width;
    int      height;
    int      pitch;  // bytes per scanline
} VesaMode;

static VesaMode       vesa;
static __dpmi_meminfo vesa_mapping;
static uint32_t       vesa_selector;

static uint16_t findVesaMode(int32_t reqW, int32_t reqH) {
    // Walk VESA mode list looking for 32bpp match at requested resolution,
    // falling back to closest available if not found.
    // Returns mode number or 0xFFFF on failure.
    __dpmi_regs r;
    static uint16_t modeList[256];
    uint16_t best = 0xFFFF;

    // Get VESA info block
    uint8_t vib[512];
    memset(vib, 0, sizeof(vib));
    vib[0]='V'; vib[1]='B'; vib[2]='E'; vib[3]='2'; // request VBE2
    dosmemput(vib, sizeof(vib), __tb);
    r.x.ax = 0x4F00;
    r.x.di = __tb & 0xF;
    r.x.es = __tb >> 4;
    __dpmi_int(0x10, &r);
    if (r.x.ax != 0x004F) return 0xFFFF;
    dosmemget(__tb, sizeof(vib), vib);

    // Mode list pointer is a far pointer at offset 14
    uint16_t modeOff = *(uint16_t*)(vib + 14);
    uint16_t modeSeg = *(uint16_t*)(vib + 16);
    uint32_t modeAddr = (modeSeg << 4) + modeOff;
    for (int i = 0; i < 256; i++) {
        modeList[i] = _farpeekw(_dos_ds, modeAddr + i * 2);
        if (modeList[i] == 0xFFFF) break;
    }

    // Check each mode
    uint8_t mib[256];
    for (int i = 0; modeList[i] != 0xFFFF && i < 256; i++) {
        memset(mib, 0, sizeof(mib));
        dosmemput(mib, sizeof(mib), __tb);
        r.x.ax = 0x4F01;
        r.x.cx = modeList[i];
        r.x.di = __tb & 0xF;
        r.x.es = __tb >> 4;
        __dpmi_int(0x10, &r);
        if (r.x.ax != 0x004F) continue;
        dosmemget(__tb, sizeof(mib), mib);

        uint16_t attrs  = *(uint16_t*)(mib + 0);
        uint16_t w      = *(uint16_t*)(mib + 18);
        uint16_t h      = *(uint16_t*)(mib + 20);
        uint8_t  bpp    = mib[25];
        uint8_t  memmdl = mib[27];

        // Must be: supported, graphics, linear fb, 32bpp, direct color
        if (!(attrs & 0x0001)) continue; // not supported
        if (!(attrs & 0x0010)) continue; // not graphics
        if (!(attrs & 0x0080)) continue; // no linear fb
        if (bpp != 32)         continue;
        if (memmdl != 6)       continue; // not direct color

        if (w == reqW && h == reqH) return modeList[i]; // exact match
        if (best == 0xFFFF)         best = modeList[i]; // fallback
    }
    return best;
}

static bool vesaInit(int32_t reqW, int32_t reqH) {
    // Init VESA
    __dpmi_regs r;
    uint8_t mib[256];

    uint16_t mode = findVesaMode(reqW, reqH);
    if (mode == 0xFFFF) {
        fprintf(stderr, "Fatal: no suitable VESA mode found\n");
        return false;
    }

    // Get mode info to read actual w/h/pitch/physBase
    memset(mib, 0, sizeof(mib));
    dosmemput(mib, sizeof(mib), __tb);
    r.x.ax = 0x4F01;
    r.x.cx = mode;
    r.x.di = __tb & 0xF;
    r.x.es = __tb >> 4;
    __dpmi_int(0x10, &r);
    dosmemget(__tb, sizeof(mib), mib);

    vesa.width    = *(uint16_t*)(mib + 18);
    vesa.height   = *(uint16_t*)(mib + 20);
    vesa.pitch    = *(uint16_t*)(mib + 16);
    vesa.physBase = *(uint32_t*)(mib + 40);

    if (vesa.width != reqW || vesa.height != reqH) {
        fprintf(stderr, "Warning: %dx%d unavailable, falling back to %dx%d\n",
                reqW, reqH, vesa.width, vesa.height);
    }
    fbWidth = vesa.width;
    fbHeight = vesa.height;

    // Set the mode (linear fb flag = 0x4000)
    r.x.ax = 0x4F02;
    r.x.bx = mode | 0x4000;
    __dpmi_int(0x10, &r);
    if (r.x.ax != 0x004F) {
        fprintf(stderr, "Fatal: could not set VESA mode\n");
        return false;
    }

    // Map physical framebuffer into flat address space
    vesa_mapping.address = vesa.physBase;
    vesa_mapping.size    = vesa.pitch * vesa.height;
    __dpmi_physical_address_mapping(&vesa_mapping);
    vesa_selector = __dpmi_allocate_ldt_descriptors(1);
    __dpmi_set_segment_base_address(vesa_selector, vesa_mapping.address);
    __dpmi_set_segment_limit(vesa_selector, vesa_mapping.size - 1);
    return true;
}

static volatile bool key_state[256];
static          bool prev_key_state[256];
static volatile bool next_extended;
static          bool caps_lock = false;

static _go32_dpmi_seginfo old_kb_handler, new_kb_handler;

static void key_handler(void) {
    unsigned char scan = inportb(0x60);

    if (scan == 0xE0) {
        next_extended = true;
        outportb(0x20, 0x20);
        return;
    }

    unsigned char code    = scan & 0x7F;
    bool          pressed = !(scan & 0x80);

    if (next_extended) {
        code |= 0x80;
        next_extended = false;
    }

    if (code == 0x3A && pressed)
        caps_lock = !caps_lock;

    key_state[code] = pressed;
    outportb(0x20, 0x20);
}

static int32_t dosScanToGml(unsigned char scan) {
    // Letters
    static const char scan_to_alpha[] = {
        // 0x10
        'Q','W','E','R','T','Y','U','I','O','P',
        // 0x1E
        'A','S','D','F','G','H','J','K','L',
        // 0x2C
        'Z','X','C','V','B','N','M'
    };
    if (scan >= 0x10 && scan <= 0x19) return scan_to_alpha[scan - 0x10];
    if (scan >= 0x1E && scan <= 0x26) return scan_to_alpha[scan - 0x1E + 10];
    if (scan >= 0x2C && scan <= 0x32) return scan_to_alpha[scan - 0x2C + 19];

    // Number row
    if (scan >= 0x02 && scan <= 0x0A) return '0' + (scan - 0x01);
    if (scan == 0x0B) return '0';

    switch (scan) {
        // Basic keys
        case 0x01: return VK_ESCAPE;
        case 0x0E: return VK_BACKSPACE;
        case 0x0F: return VK_TAB;
        case 0x1C: return VK_ENTER;
        case 0x39: return VK_SPACE;

        // Modifiers (non-extended)
        case 0x2A: return VK_SHIFT;    // left shift
        case 0x36: return VK_SHIFT;    // right shift
        case 0x1D: return VK_CONTROL;  // left ctrl
        case 0x38: return VK_ALT;      // left alt

        // F-keys
        case 0x3B: return VK_F1;
        case 0x3C: return VK_F2;
        case 0x3D: return VK_F3;
        case 0x3E: return VK_F4;
        case 0x3F: return VK_F5;
        case 0x40: return VK_F6;
        case 0x41: return VK_F7;
        case 0x42: return VK_F8;
        case 0x43: return VK_F9;
        case 0x44: return VK_F10;
        case 0x57: return VK_F11;
        case 0x58: return VK_F12;

        // Numpad arrows (Num Lock off) — same codes as extended arrows,
        // non-extended prefix so they land here naturally
        case 0x48: return VK_UP;
        case 0x50: return VK_DOWN;
        case 0x4B: return VK_LEFT;
        case 0x4D: return VK_RIGHT;
        case 0x52: return VK_INSERT;
        case 0x53: return VK_DELETE;
        case 0x47: return VK_HOME;
        case 0x4F: return VK_END;
        case 0x49: return VK_PAGEUP;
        case 0x51: return VK_PAGEDOWN;

        // Extended keys (0xE0 prefix → high bit set by handler)
        case 0x9D: return VK_CONTROL;  // right ctrl  (0x1D | 0x80)
        case 0xB8: return VK_ALT;      // right alt   (0x38 | 0x80)
        case 0xC8: return VK_UP;       // (0x48 | 0x80)
        case 0xD0: return VK_DOWN;     // (0x50 | 0x80)
        case 0xCB: return VK_LEFT;     // (0x4B | 0x80)
        case 0xCD: return VK_RIGHT;    // (0x4D | 0x80)
        case 0xD2: return VK_INSERT;   // (0x52 | 0x80)
        case 0xD3: return VK_DELETE;   // (0x53 | 0x80)
        case 0xC7: return VK_HOME;     // (0x47 | 0x80)
        case 0xCF: return VK_END;      // (0x4F | 0x80)
        case 0xC9: return VK_PAGEUP;   // (0x49 | 0x80)
        case 0xD1: return VK_PAGEDOWN; // (0x51 | 0x80)

        default: return -1;
    }
}

static unsigned int scanToChar(unsigned char scan) {
    bool shift = key_state[0x2A] || key_state[0x36];
    bool caps  = caps_lock;

    static const char normal[128] = {
        [0x02]='1', [0x03]='2', [0x04]='3', [0x05]='4', [0x06]='5',
        [0x07]='6', [0x08]='7', [0x09]='8', [0x0A]='9', [0x0B]='0',
        [0x0C]='-', [0x0D]='=',
        [0x10]='q', [0x11]='w', [0x12]='e', [0x13]='r', [0x14]='t',
        [0x15]='y', [0x16]='u', [0x17]='i', [0x18]='o', [0x19]='p',
        [0x1A]='[', [0x1B]=']',
        [0x1E]='a', [0x1F]='s', [0x20]='d', [0x21]='f', [0x22]='g',
        [0x23]='h', [0x24]='j', [0x25]='k', [0x26]='l',
        [0x27]=';', [0x28]='\'', [0x29]='`',
        [0x2B]='\\',
        [0x2C]='z', [0x2D]='x', [0x2E]='c', [0x2F]='v', [0x30]='b',
        [0x31]='n', [0x32]='m',
        [0x33]=',', [0x34]='.', [0x35]='/',
        [0x39]=' ',
        [0x47]='7', [0x48]='8', [0x49]='9',
        [0x4B]='4', [0x4C]='5', [0x4D]='6',
        [0x4F]='1', [0x50]='2', [0x51]='3',
        [0x52]='0', [0x53]='.',
    };

    static const char shifted[128] = {
        [0x02]='!', [0x03]='@', [0x04]='#', [0x05]='$', [0x06]='%',
        [0x07]='^', [0x08]='&', [0x09]='*', [0x0A]='(', [0x0B]=')',
        [0x0C]='_', [0x0D]='+',
        [0x10]='Q', [0x11]='W', [0x12]='E', [0x13]='R', [0x14]='T',
        [0x15]='Y', [0x16]='U', [0x17]='I', [0x18]='O', [0x19]='P',
        [0x1A]='{', [0x1B]='}',
        [0x1E]='A', [0x1F]='S', [0x20]='D', [0x21]='F', [0x22]='G',
        [0x23]='H', [0x24]='J', [0x25]='K', [0x26]='L',
        [0x27]=':', [0x28]='"', [0x29]='~',
        [0x2B]='|',
        [0x2C]='Z', [0x2D]='X', [0x2E]='C', [0x2F]='V', [0x30]='B',
        [0x31]='N', [0x32]='M',
        [0x33]='<', [0x34]='>', [0x35]='?',
        [0x39]=' ',
    };

    if (scan >= 128) return 0;

    char c = shift ? shifted[scan] : normal[scan];
    if (!c) return 0;

    if (caps && c >= 'a' && c <= 'z') c -= 32;
    if (caps && shift && c >= 'A' && c <= 'Z') c += 32;

    return (unsigned int)c;
}

static void vesaShutdown(void) {
    __dpmi_free_physical_address_mapping(&vesa_mapping);
    __dpmi_free_ldt_descriptor(vesa_selector);
}

void platformSetWindowSize(int32_t width, int32_t height) {
    vesaShutdown();
    if (!vesaInit(width, height))
        exit(1);
}

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless) {
    if (!vesaInit(reqW, reqH))
        return false;

    // install keyboard handler
    _go32_dpmi_get_protected_mode_interrupt_vector(9, &old_kb_handler);
    new_kb_handler.pm_offset   = (int)key_handler;
    new_kb_handler.pm_selector = _go32_my_cs();
    _go32_dpmi_allocate_iret_wrapper(&new_kb_handler);
    _go32_dpmi_set_protected_mode_interrupt_vector(9, &new_kb_handler);

    // Redirect stdio, not doing this causes messages to be
    // printed to the framebuffer while the game is running.
    // It also lets us actually read the output instead of
    // it just being lost forever.
    freopen("bslog.txt", "w", stderr);
    setbuf(stderr, NULL);
    return true;
}

void platformExit(void) {
    // uninstall keyboard handler
    _go32_dpmi_set_protected_mode_interrupt_vector(9, &old_kb_handler);
    _go32_dpmi_free_iret_wrapper(&new_kb_handler);

    vesaShutdown();

    // Return to text mode
    __dpmi_regs r;
    r.x.ax = 0x0003;
    __dpmi_int(0x10, &r);
}

void platformInitFunctions(Runner *runner) {
    g_runner = runner;
}

static uint32_t* nextFb = NULL;
static int nextFbH;

static const unsigned char fps_digits[10][5] = {
    {0x6,0x9,0x9,0x9,0x6}, /* 0 */
    {0x2,0x6,0x2,0x2,0x7}, /* 1 */
    {0x6,0x9,0x2,0x4,0xF}, /* 2 */
    {0xE,0x1,0x6,0x1,0xE}, /* 3 */
    {0x9,0x9,0xF,0x1,0x1}, /* 4 */
    {0xF,0x8,0xE,0x1,0xE}, /* 5 */
    {0x6,0x8,0xE,0x9,0x6}, /* 6 */
    {0xF,0x1,0x2,0x4,0x4}, /* 7 */
    {0x6,0x9,0x6,0x9,0x6}, /* 8 */
    {0x6,0x9,0x7,0x1,0x6}, /* 9 */
};

static void FPS_draw(void) {
    static uclock_t last  = 0;
    static int      count = 0;
    static int      fps   = 0;

    uclock_t now = uclock();
    if (++count, now - last >= UCLOCKS_PER_SEC) {
        fps   = count;
        count = 0;
        last  = now;
    }

    int digits[4], ndig = 0, val = fps > 0 ? fps : 0;
    do { digits[ndig++] = val % 10; val /= 10; } while (val && ndig < 4);

    const int PITCH = vesa.pitch / 4;  /* pixels per row */
    const int SCALE = 2;
    const int GW    = 4;
    const int GH    = 5;
    const int GAP   = 1;
    const int PAD   = 2;
    const int DX    = 4;
    const int DY    = 4;

    int sw = (ndig * (GW + GAP) - GAP) * SCALE;
    int sh = GH * SCALE;

    /* black background */
    int r, c;
    for (r = DY - PAD; r < DY + sh + PAD; r++)
        for (c = DX - PAD; c < DX + sw + PAD; c++)
            nextFb[r * PITCH + c] = 0x00000000;

    /* draw digits */
    int d, gx = DX;
    for (d = ndig - 1; d >= 0; d--) {
        const unsigned char *g = fps_digits[digits[d]];
        int row, col, sy, sx;
        for (row = 0; row < GH; row++)
            for (col = 0; col < GW; col++) {
                uint32_t color = (g[row] & (0x8 >> col)) ? 0x00FFFFFF : 0x00000000;
                for (sy = 0; sy < SCALE; sy++)
                    for (sx = 0; sx < SCALE; sx++)
                        nextFb[(DY + row * SCALE + sy) * PITCH + gx + col * SCALE + sx] = color;
            }
        gx += (GW + GAP) * SCALE;
    }
}

void platformSwapBuffers(void) {
    if (!nextFb) return;

    FPS_draw();

    const int height = nextFbH < vesa.height ? nextFbH : vesa.height;

    movedata(_my_ds(), (unsigned)nextFb,
            vesa_selector, 0,
            height * vesa.pitch);

    nextFb = NULL;
}

void Runner_setNextFrame(uint32_t* framebuffer, int width, int height) {
    nextFb  = framebuffer;
    nextFbH = height;
}

double platformGetTime(void) {
    return (double)uclock() / UCLOCKS_PER_SEC;
}

bool platformHandleEvents(void) {
    // During playback, suppress real keyboard input
    if (InputRecording_isPlaybackActive(globalInputRecording)) return false;

    for (int i = 0; i < 256; ++i) {
        bool now  = key_state[i];
        bool prev = prev_key_state[i];

        if (now && !prev) {
            RunnerKeyboard_onKeyDown(g_runner->keyboard, dosScanToGml(i));
            unsigned int codepoint = scanToChar(i);
            if (codepoint)
                RunnerKeyboard_onCharacter(g_runner->keyboard, codepoint);
        } else if (!now && prev)
            RunnerKeyboard_onKeyUp(g_runner->keyboard, dosScanToGml(i));

        prev_key_state[i] = now;
    }

    return false;
}

void platformSleepUntil(uint64_t time) {
    while (nowNanos() < time)
        ;
}

void platformSetWindowTitle(const char* title) {
    (void)title;
}
