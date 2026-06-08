// OpenSL|ES compatibility shim for building against the Android 2.3
// (API 9/10, NDK r10e) platform headers.
//
// miniaudio references a handful of OpenSL|ES Android-extension
// constants that only appear in the unified headers shipped with
// later platforms (android-14+):
//
//   * SL_ANDROID_STREAM_*                      — playback stream type
//       (present in android-9's OpenSLES_AndroidConfiguration.h, but
//        miniaudio only #includes OpenSLES_Android.h, so we pull the
//        configuration header in here too).
//   * SL_ANDROID_RECORDING_PRESET_*            — capture presets, only
//       defined from android-14 onward.
//
// Butterscotch is playback-only: it never opens a capture device, so
// the recording-preset values are referenced by miniaudio's switch
// statements but never actually used at runtime on our target. We
// define them (with their canonical android-14 values) purely so the
// translation unit compiles against the Gingerbread platform headers.
//
// This header is force-included (-include) ahead of miniaudio.h for the
// audio translation unit via jni/Android.mk.

// Bionic on API 9 defines fileno() as a macro (__sfileno). miniaudio
// declares its own `int fileno(FILE*)` prototype unless one of the POSIX
// feature macros is set, and that prototype collides with the macro,
// producing "expected ')' before '*'". Declaring _POSIX_SOURCE makes
// miniaudio defer to bionic's fileno macro. Must be set before any
// system header is pulled in — this shim is force-included first.
#ifndef _POSIX_SOURCE
#define _POSIX_SOURCE 1
#endif

#pragma once

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>

// --- Playback stream types (android-9 has these; belt-and-suspenders) ---
#ifndef SL_ANDROID_STREAM_VOICE
#define SL_ANDROID_STREAM_VOICE        ((SLint32) 0x00000000)
#endif
#ifndef SL_ANDROID_STREAM_SYSTEM
#define SL_ANDROID_STREAM_SYSTEM       ((SLint32) 0x00000001)
#endif
#ifndef SL_ANDROID_STREAM_RING
#define SL_ANDROID_STREAM_RING         ((SLint32) 0x00000002)
#endif
#ifndef SL_ANDROID_STREAM_MEDIA
#define SL_ANDROID_STREAM_MEDIA        ((SLint32) 0x00000003)
#endif
#ifndef SL_ANDROID_STREAM_ALARM
#define SL_ANDROID_STREAM_ALARM        ((SLint32) 0x00000004)
#endif
#ifndef SL_ANDROID_STREAM_NOTIFICATION
#define SL_ANDROID_STREAM_NOTIFICATION ((SLint32) 0x00000005)
#endif

// --- Recording presets (android-14+ only; unused, playback-only app) ---
#ifndef SL_ANDROID_RECORDING_PRESET_NONE
#define SL_ANDROID_RECORDING_PRESET_NONE                 ((SLuint32) 0x00000000)
#endif
#ifndef SL_ANDROID_RECORDING_PRESET_GENERIC
#define SL_ANDROID_RECORDING_PRESET_GENERIC              ((SLuint32) 0x00000001)
#endif
#ifndef SL_ANDROID_RECORDING_PRESET_CAMCORDER
#define SL_ANDROID_RECORDING_PRESET_CAMCORDER            ((SLuint32) 0x00000002)
#endif
#ifndef SL_ANDROID_RECORDING_PRESET_VOICE_RECOGNITION
#define SL_ANDROID_RECORDING_PRESET_VOICE_RECOGNITION    ((SLuint32) 0x00000003)
#endif
#ifndef SL_ANDROID_RECORDING_PRESET_VOICE_COMMUNICATION
#define SL_ANDROID_RECORDING_PRESET_VOICE_COMMUNICATION  ((SLuint32) 0x00000004)
#endif
#ifndef SL_ANDROID_RECORDING_PRESET_UNPROCESSED
#define SL_ANDROID_RECORDING_PRESET_UNPROCESSED          ((SLuint32) 0x00000005)
#endif
