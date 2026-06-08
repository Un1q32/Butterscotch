# GLES backend (OpenGL ES 1.1) + iOS / Android frontends

This directory holds the **OpenGL ES 1.1** path for Butterscotch and the
platform-specific GUI wrappers built on top of it.

```
src/gles/
  gles1_renderer.{h,c}     ← shared GLES 1.1 RendererVtable implementation
  ios/                     ← iOS UIKit wrapper (UIWindow + EAGLContext)
  android/                 ← Android GLSurfaceView / JNI wrapper (full port)
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

A full native port lives in `android/` (GLSurfaceView + JNI around the
same GLES 1.1 renderer, miniaudio→OpenSL ES audio). It targets Android
2.3 Gingerbread (API 10) and installs down to API 8.

```bash
cd src/gles/android/app/src/main
# Needs NDK r10e (last NDK with Gingerbread sysroots):
$NDK_R10E/ndk-build \
    NDK_PROJECT_PATH=. \
    APP_BUILD_SCRIPT=./jni/Android.mk \
    NDK_APPLICATION_MK=./jni/Application.mk \
    APP_ABI=armeabi-v7a
```

CI assembles + signs a full `.apk` on every push — see
`.github/workflows/android.yml`. Full details in `android/README.md`.
