# Butterscotch Android

A native Android port of Butterscotch (a GameMaker: Studio runtime
reimplementation), targeting **Android 2.3 Gingerbread (API 10)** on the
HTC Sensation XE class of device — but it installs and runs on anything
from **Android 2.2 Froyo (API 8)** upward.

It reuses the exact same C runtime, VM, and OpenGL ES 1.1 renderer as the
iOS port (`../ios`); only the platform shell differs:

| Layer        | iOS (`../ios`)              | Android (here)                       |
|--------------|-----------------------------|--------------------------------------|
| Frontend     | UIKit + `main.m`            | `GLSurfaceView` + Java Activities    |
| Audio        | OpenAL (system framework)   | miniaudio → OpenSL ES                 |
| GL headers   | `<OpenGLES/ES1/gl.h>`       | `<GLES/gl.h>`                         |
| Touch UI     | `BSPadButton` overlay       | `TouchOverlayView` (Canvas-drawn)     |
| Build        | Theos Makefile → `.ipa`     | NDK r10e + SDK build-tools → `.apk`   |

The renderer (`../gles1_renderer.c`) is platform-neutral: on non-Apple
builds it includes `<GLES/gl.h>` and uses the `GL_*_OES` FBO entry
points, which is exactly what Android's GLES 1.1 driver exposes.

## Layout

```
android/
├── README.md
├── build.gradle / settings.gradle / gradle.properties   # optional Gradle path
└── app/
    ├── build.gradle
    └── src/main/
        ├── AndroidManifest.xml          # minSdk 8, target 10, landscape
        ├── java/com/mrpowergamerbr/butterscotch/
        │   ├── NativeBridge.java        # JNI declarations (mirrors bs_jni.c)
        │   ├── GameRenderer.java        # GLSurfaceView.Renderer → nativeStep()
        │   ├── GameGLSurfaceView.java   # ES 1.x context, RGB565 config
        │   ├── TouchOverlayView.java    # on-screen D-pad + Z/X/C/Shift/Esc
        │   ├── GameActivity.java        # hosts the running game
        │   └── GamePickerActivity.java  # scans storage for data.win games
        ├── jni/
        │   ├── Android.mk               # builds libbutterscotch.so
        │   ├── Application.mk            # ABI/platform/toolchain
        │   ├── bs_jni.c                  # the runtime bring-up + frame loop
        │   ├── stb_impl.c                # stb single-TU impls
        │   ├── compat_math.c             # log2/exp2 for old bionic libm
        │   └── opensl_compat.h           # OpenSL ES constants for API 9 headers
        └── res/                          # icons, strings, theme
```

## Building

CI builds it automatically on every push — see
`.github/workflows/android.yml`. The artifact is a debug-signed
`Butterscotch-<hash>-armeabi-v7a-gingerbread.apk`.

To build locally you need **NDK r10e** (the last NDK with Gingerbread
sysroots) and the Android SDK build-tools.

### 1. Native library

```sh
cd app/src/main
$NDK_R10E/ndk-build \
    NDK_PROJECT_PATH=. \
    APP_BUILD_SCRIPT=./jni/Android.mk \
    NDK_APPLICATION_MK=./jni/Application.mk \
    APP_ABI=armeabi-v7a
# → libs/armeabi-v7a/libbutterscotch.so
```

This compiles the full `src/*.c` runtime (minus `data_win_print.c`),
the GLES 1.1 renderer, the miniaudio audio system, and the JNI bridge.

### 2. APK (no Gradle required)

The app uses **zero** third-party/support libraries — only framework
APIs — so the APK can be assembled straight from the SDK build-tools
(`aapt` → `javac` → `d8` → `zipalign` → `apksigner`). The CI workflow
does exactly this; read it for the precise commands. A Gradle path
(`build.gradle` etc.) is also provided for IDE users, wired to the same
`jni/Android.mk` via prebuilt jniLibs.

## Why these versions

- **NDK r10e / GCC 4.8** — newest toolchain that still ships API 8/9
  sysroots. r17+ dropped everything below API 16, so they cannot produce
  a Gingerbread-compatible binary.
- **miniaudio → OpenSL ES** — Android has no system OpenAL (unlike iOS),
  and AAudio needs API 26. OpenSL ES is present since API 9. The
  `opensl_compat.h` shim back-fills a few `SL_ANDROID_*` constants that
  only landed in the unified API 14 headers but are referenced by
  miniaudio's switch statements.
- **`compat_math.c`** — `log2`/`log2f`/`exp2`/`exp2f` were added to
  bionic's libm in API 18; on Gingerbread we provide our own
  `log(x)/M_LN2`-style fallbacks.
- **GLES 1.1** — the renderer is fixed-function (`glOrthof`, no shaders),
  which is universally available and a perfect match for 2D GameMaker
  output.

## Controls

The on-screen overlay mirrors the iOS layout, restyled for Android:

- **D-pad** (left): arrow keys (←↑→↓).
- **Z** = Enter/confirm · **X** = Cancel/menu · **C** = extra action.
- **Shift** (hold to run) and **Esc** small buttons.

All keys feed the same GML virtual-key codes the runtime expects (see
`src/runner_keyboard.h`).

## Installing games

`GamePickerActivity` scans for any folder containing a `data.win` under:

- `<external storage>/Butterscotch/<game>/data.win`
- the app-private files dir (`<filesDir>/games/<game>/data.win`)

Drop a GameMaker `data.win` (plus its loose assets, if any) into a
sub-folder there and it shows up in the picker. Saves are redirected to
the app-private `saves/<game>/` dir so they survive but stay sandboxed.
