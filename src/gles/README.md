# GLES backend (OpenGL ES 1.1) + iOS / Android frontends

This directory holds the **OpenGL ES 1.1** path for Butterscotch and the
platform-specific GUI wrappers built on top of it.

```
src/gles/
  gles1_renderer.{h,c}     ← shared GLES 1.1 RendererVtable implementation
  ios/                     ← iOS UIKit wrapper (UIWindow + EAGLContext)
  android/                 ← Android NativeActivity / JNI wrapper (skeleton)
```

## Why GLES 1.1 specifically

The primary device target is the **iPod Touch 2G** (PowerVR MBX Lite, iPhone
OS 2.1.1–4.2.1). MBX Lite supports **OpenGL ES 1.1 only** — there is no
GLES 2.0 hardware on this generation. The same GLES 1.1 backend also runs
on early Android devices (API 5+) so the renderer code is shared.

For modern desktops, see `src/gl/` (GLES 3.0 / desktop GL) and
`src/gl_legacy/` (desktop fixed-function).

## iOS build (Theos, Linux host)

See `ios/Makefile` for the build. Quick start (assumes `THEOS` is set and
the iPhoneOS3.1.3 SDK is in `$THEOS/sdks/`):

```bash
cd src/gles/ios
make package         # produces a .deb
make ipa             # repackages into a sideloadable .ipa
```

## Android build

Skeleton only for now. Will use Android NDK r10e + `NativeActivity` on
API 9+ (Android 2.3 Gingerbread).
