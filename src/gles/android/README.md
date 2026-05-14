# Butterscotch Android — placeholder

This directory is reserved for the Android NDK / `NativeActivity` /
JNI wrapper around `src/gles/gles1_renderer.c`. It is **not yet
implemented**.

Planned target: Android 2.3 Gingerbread (API 9, `armeabi-v7a`, GLES
2.0+OpenSL ES). NDK r10e is the last NDK that ships GCC 4.8 with
proper API 8/9 sysroots; r12b is the last with API 9.

Files that will live here once we start:

- `main.c` — `android_native_app_glue` entry point: pumps input,
  manages the EGL context, calls `Runner_step()` each frame.
- `bs_jni.c` — JNI bridge for Java-side things we need (file picker,
  asset extraction from APK, vibration, etc.).
- `AndroidManifest.xml` — `NativeActivity` declaration, screen
  orientations, permissions.
- `jni/Android.mk` + `jni/Application.mk` — NDK build files.

The renderer code in `../gles1_renderer.c` is already platform-neutral
(uses `<GLES/gl.h>` on non-Apple builds), so dropping it into an
Android build is purely a packaging exercise.
