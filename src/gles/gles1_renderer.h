#pragma once

// Butterscotch RendererVtable implementation against OpenGL ES 1.1.
//
// Designed for the original iPhone, iPod Touch 1G/2G (PowerVR MBX Lite),
// plus early Android devices. No shaders — uses the GLES 1.1 fixed
// function pipeline (glColor4f / glVertexPointer / glTexCoordPointer /
// glDrawArrays).
//
// IMPORTANT: A valid GLES 1.1 context must already be current on the
// calling thread before any vtable call. On iOS this is set up by
// `src/gles/ios` (EAGLContext + CAEAGLLayer); on Android by the
// EGLSurface owned by NativeActivity.
//
// This is intentionally a skeleton: at the moment everything beyond
// init / destroy / clearScreen / beginFrame / endFrame is a no-op so
// the runner can be brought up on-device and we can iterate from real
// hardware logs.

#include "../renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Allocate + initialize an empty GLES 1.1 renderer. Returns a Renderer*
// that can be passed to Runner_create. The renderer's vtable is fully
// populated (no nullptr entries) so Runner_step / draw paths are safe
// to call even though most draw functions are currently stubs.
Renderer* GLES1Renderer_create(void);

// Tell the renderer where data.win lives on disk so it can fopen its
// own FILE handle (independent of dataWin->lazyLoadFile) and lazily
// stream TXTR PNG blobs from that file. Must be called before
// Runner_create / Renderer_init (the vtable init reads this path).
void GLES1Renderer_setDataWinPath(Renderer* r, const char* path);

// Hint to the renderer that the host OS is under memory pressure
// (e.g. iOS UIApplication didReceiveMemoryWarning, Android onTrimMemory).
// The renderer aggressively evicts every resident atlas that wasn't
// touched during the current frame to give the OS pages back.  Safe
// to call from any thread that has the GL context current.
void GLES1Renderer_handleMemoryWarning(Renderer* r);

#ifdef __cplusplus
}
#endif
