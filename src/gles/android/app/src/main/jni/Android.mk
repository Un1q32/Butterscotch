# Butterscotch Android — NDK Android.mk
# =============================================================================
#
# Builds libbutterscotch.so: the full Butterscotch C runtime (DataWin
# parser + VM + Runner) plus the shared GLES 1.1 renderer, the miniaudio
# audio system, and the JNI bridge (bs_jni.c). Loaded by NativeBridge on
# the Java side.
#
# This is the Android analogue of src/gles/ios/Makefile. The same set of
# src/*.c runtime files are compiled; the only platform-specific pieces
# that differ are:
#   * audio:   miniaudio (OpenSL|ES) instead of OpenAL.
#   * frontend: bs_jni.c + Java GLSurfaceView instead of main.m + UIKit.
#   * GL headers: <GLES/gl.h> (gles1_renderer.c picks this on non-Apple).

LOCAL_PATH := $(call my-dir)

# ndk-build prepends $(LOCAL_PATH)/ to every LOCAL_SRC_FILES entry, so
# source paths must be RELATIVE to jni/ (no $(LOCAL_PATH) prefix). Includes,
# by contrast, want absolute paths, so we keep both forms:
#   REL_ROOT  — relative repo root, for LOCAL_SRC_FILES
#   ABS_SRC   — absolute src/ + vendor/, for LOCAL_C_INCLUDES
#   (jni → main → src → app → android → gles → src → repo root = 7 up)
REL_ROOT   := ../../../../../../..
REL_SRC    := $(REL_ROOT)/src
REL_VENDOR := $(REL_ROOT)/vendor

ABS_SRC    := $(LOCAL_PATH)/$(REL_ROOT)/src
ABS_VENDOR := $(LOCAL_PATH)/$(REL_ROOT)/vendor

include $(CLEAR_VARS)

LOCAL_MODULE := butterscotch

# ---- Runtime sources --------------------------------------------------------
# The full src/*.c runtime EXCEPT:
#   * data_win_print.c  — only used by the desktop `--print` subcommand,
#     pulls in dprintf helpers we do not need (same omission as iOS).
# Renderer: the shared GLES 1.1 backend.
# Audio: miniaudio system (stb_vorbis.c is textually #included by it).
BS_RUNTIME_SRC := \
    $(REL_SRC)/binary_reader.c \
    $(REL_SRC)/data_win.c \
    $(REL_SRC)/debug_overlay.c \
    $(REL_SRC)/event_table.c \
    $(REL_SRC)/gml_array.c \
    $(REL_SRC)/gml_method.c \
    $(REL_SRC)/ini.c \
    $(REL_SRC)/input_recording.c \
    $(REL_SRC)/instance.c \
    $(REL_SRC)/int_int_hashmap.c \
    $(REL_SRC)/int_rvalue_hashmap.c \
    $(REL_SRC)/json_reader.c \
    $(REL_SRC)/json_writer.c \
    $(REL_SRC)/noop_audio_system.c \
    $(REL_SRC)/noop_file_system.c \
    $(REL_SRC)/overlay_file_system.c \
    $(REL_SRC)/profiler.c \
    $(REL_SRC)/runner.c \
    $(REL_SRC)/runner_gamepad.c \
    $(REL_SRC)/runner_keyboard.c \
    $(REL_SRC)/runner_mouse.c \
    $(REL_SRC)/spatial_grid.c \
    $(REL_SRC)/string_builder.c \
    $(REL_SRC)/vm.c \
    $(REL_SRC)/vm_builtins.c

BS_VENDOR_SRC := \
    $(REL_VENDOR)/md5/md5.c \
    $(REL_VENDOR)/sha1/sha1.c \
    $(REL_VENDOR)/base64/base64.c

BS_RENDERER_SRC := \
    $(REL_SRC)/gles/gles1_renderer.c

BS_AUDIO_SRC := \
    $(REL_SRC)/audio/miniaudio/ma_audio_system.c

LOCAL_SRC_FILES := \
    bs_jni.c \
    compat_math.c \
    stb_impl.c \
    $(BS_RENDERER_SRC) \
    $(BS_RUNTIME_SRC) \
    $(BS_VENDOR_SRC) \
    $(BS_AUDIO_SRC)

# ---- Include paths (absolute; mirror the iOS Makefile) ----------------------
LOCAL_C_INCLUDES := \
    $(ABS_SRC) \
    $(ABS_SRC)/audio/miniaudio \
    $(ABS_VENDOR)/stb/ds \
    $(ABS_VENDOR)/stb/image \
    $(ABS_VENDOR)/stb/vorbis \
    $(ABS_VENDOR)/md5 \
    $(ABS_VENDOR)/sha1 \
    $(ABS_VENDOR)/base64 \
    $(ABS_VENDOR)/miniaudio

# ---- Defines ----------------------------------------------------------------
# BS_GLES1=1            : gles1_renderer.c picks the GLES1 path.
# ENABLE_WAD16=1        : enable WAD/bytecode v16 (Undertale v1.08).
# MAX_SOUND_INSTANCES   : Sensation XE has 768 MB RAM (vs the iPod Touch
#                         2G's 128 MB) so we can afford the upstream-ish
#                         64 voices; still capped to keep OpenSL happy.
# USE_MINIAUDIO         : ma_audio_system selects its real backend.
# MA_NO_AAUDIO          : AAudio needs API 26 — force the OpenSL|ES path.
# MA_NO_WASAPI etc.     : disable desktop backends; only OpenSL on Android.
# BS_GIT_VERSION        : commit hash, surfaced via NativeBridge.version().
BS_GIT_VERSION := $(shell cd $(LOCAL_PATH)/$(REL_ROOT) && git rev-parse --short=10 HEAD 2>/dev/null || echo unknown)
BS_GIT_DIRTY   := $(shell cd $(LOCAL_PATH)/$(REL_ROOT) && git diff --quiet HEAD 2>/dev/null || echo +)

LOCAL_CFLAGS := \
    -std=gnu99 \
    -include $(LOCAL_PATH)/opensl_compat.h \
    -DBS_PLATFORM_ANDROID=1 \
    -DBS_NEED_MATH_COMPAT=1 \
    -DBS_GLES1=1 \
    -DENABLE_WAD16=1 \
    -DUSE_MINIAUDIO=1 \
    -DMAX_SOUND_INSTANCES=64 \
    -DMAX_AUDIO_STREAMS=8 \
    -DMA_NO_AAUDIO \
    -DMA_NO_WASAPI -DMA_NO_DSOUND -DMA_NO_WINMM \
    -DMA_NO_ALSA -DMA_NO_PULSEAUDIO -DMA_NO_JACK \
    -DMA_NO_COREAUDIO -DMA_NO_SNDIO -DMA_NO_AUDIO4 -DMA_NO_OSS \
    -DBS_GIT_VERSION='"$(BS_GIT_VERSION)$(BS_GIT_DIRTY)"' \
    -Wno-unused-parameter \
    -Wno-unused-variable \
    -Wno-unused-function \
    -Wno-pointer-sign \
    -Wno-sign-compare \
    -Wno-format \
    -Wno-implicit-function-declaration

# ---- Link libraries ---------------------------------------------------------
# GLESv1_CM : OpenGL ES 1.1 (Common-Lite) — present on every Android since 1.0.
# EGL       : context/surface management (Java owns it, but we link defensively).
# OpenSLES  : OpenSL|ES — present since API 9 (Gingerbread); miniaudio runtime-
#             links it via dlopen, but we link explicitly so it resolves on
#             devices where dlopen("libOpenSLES.so") is flaky (see MA docs).
# log       : __android_log_print.
# android   : AAsset / native window glue (future asset-from-APK use).
LOCAL_LDLIBS := -lm -lGLESv1_CM -lEGL -lOpenSLES -llog -landroid

include $(BUILD_SHARED_LIBRARY)
