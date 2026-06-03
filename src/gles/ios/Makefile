# Butterscotch iOS — Theos Makefile
#
# Targets iPod Touch 2G (armv6) running jailbroken iPhone OS 2.0+.
# Build on Linux with the standard Theos Linux toolchain:
#
#   sudo bash -c "$(curl -fsSL https://raw.githubusercontent.com/theos/theos/master/bin/install-theos)"
#
# Drop the iPhoneOS3.1.3 SDK into $THEOS/sdks/ (e.g. from the
# qianjigui/iOS-sdks repo, file iPhoneOS3.1.3.sdk.tbz2). Then:
#
#   cd src/gles/ios
#   make package           # builds the app + a .deb
#   make ipa               # rewraps the .deb's .app bundle into a .ipa
#
# The .ipa lands in ./packages/Butterscotch.ipa and is the artifact you
# sideload via AppSync/Filza onto a jailbroken iPod Touch 2G.

# -----------------------------------------------------------------------------
# Toolchain target. Format: <platform>:<compiler>:<SDK ver>:<deployment ver>
# armv6 is mandatory for iPod Touch 2G — armv7 binaries refuse to launch.
# -----------------------------------------------------------------------------
TARGET := iphone:clang:3.1.3:2.0
ARCHS  := armv6

# -----------------------------------------------------------------------------
# Theos preamble
# -----------------------------------------------------------------------------
INSTALL_TARGET_PROCESSES = Butterscotch
include $(THEOS)/makefiles/common.mk

APPLICATION_NAME = Butterscotch

# -----------------------------------------------------------------------------
# Sources
#
# Includes the full Butterscotch C runtime so DataWin parsing, the VM
# and the Runner are all linked in. We omit the platform-specific
# main.c entries (src/glfw, src/ps2, src/ps3, src/web) — main.m below
# replaces them — plus data_win_print.c which is only used by the
# `--print` debug subcommand on desktop and pulls in dprintf-style
# helpers we do not need on iOS.
# -----------------------------------------------------------------------------
BS_RUNTIME_FILES = $(wildcard ../../*.c) ../../../vendor/md5/md5.c ../../../vendor/sha1/sha1.c ../../../vendor/base64/base64.c

# OpenAL-based audio system, used in place of NoopAudioSystem on iOS.
# OpenAL is built into iOS as a system framework since iOS 2.0 and gives
# us multi-voice PCM mixing for free; stb_vorbis is bundled and is
# textually included by al_audio_system.c so no separate compile entry.
BS_AUDIO_FILES = \
    ../../audio/openal/al_audio_system.c \
    ../../audio/openal/wave.c

BS_RENDERER_FILES = \
    ../gles1_renderer.c

Butterscotch_FILES = \
    main.m \
    stb_impl.c \
    $(BS_RENDERER_FILES) \
    $(BS_RUNTIME_FILES) \
    $(BS_AUDIO_FILES)

# -----------------------------------------------------------------------------
# Frameworks
# -----------------------------------------------------------------------------
Butterscotch_FRAMEWORKS = UIKit Foundation CoreGraphics QuartzCore OpenGLES OpenAL AudioToolbox

# -----------------------------------------------------------------------------
# Compiler flags
#   - -fno-objc-arc: iOS 3 SDK is pre-ARC; we use manual retain/release.
#   - -ffixed-r9: iOS 2.x ABI reserves R9 as a platform register. LLVM 10
#     doesn't know this (fix only landed in LLVM 20, Oct 2025), so we
#     manually tell the compiler to never use R9.  Harmless on iOS 3.x+.
#   - -DBS_PLATFORM_IOS: marker for future src/*.c conditionals.
#   - -DBS_GLES1=1: marker so gles1_renderer.c picks the iOS GLES1 path.
#   - -Wno-deprecated-declarations: iOS 3.1.3 deployment is flagged
#     deprecated by clang; that's fine, we know.
# -----------------------------------------------------------------------------
# MAX_SOUND_INSTANCES/AL_STREAM_BUFFER_SAMPLES are dialed down from the
# upstream defaults (128 / 4096) because the iPod Touch 2G has 128 MB
# total RAM and the iOS OpenAL driver caps practical source count well
# below 128.  32 simultaneous voices is plenty for Undertale (worst case
# is ~8 SFX + 1 music track) and the 2048-sample decode chunk halves
# the OGG streaming buffer footprint without causing audible underruns.
# AL_BUFFER_CACHE_SIZE/BUDGET caps the shared buffer cache so we never
# hold more than ~2 MB of decoded PCM across all cached sounds — this
# prevents OOM during the boss fight's SFX burst.
# Inject the build's git commit as the app version (shown in Settings →
# About). Falls back to "unknown" outside a git checkout; a trailing "+"
# marks a dirty working tree.
BS_GIT_VERSION := $(shell git rev-parse --short=10 HEAD 2>/dev/null || echo unknown)
BS_GIT_DIRTY   := $(shell git diff --quiet HEAD 2>/dev/null || echo +)

Butterscotch_CFLAGS  = -fno-objc-arc \
                       -ffixed-r9 \
                       -DBS_PLATFORM_IOS=1 \
                       -DBS_GLES1=1 \
                       -DENABLE_WAD16=1 \
                       -DMAX_SOUND_INSTANCES=32 \
                       -DMAX_AUDIO_STREAMS=8 \
                       -DAL_STREAM_BUFFER_SAMPLES=2048 \
                       -DAL_BUFFER_CACHE_SIZE=32 \
                       -DAL_BUFFER_CACHE_BUDGET='(2u * 1024u * 1024u)' \
                       -DAL_BUFFER_CACHE_MAX_ENTRY='(64u * 1024u)' \
                       -DBS_GIT_VERSION='"$(BS_GIT_VERSION)$(BS_GIT_DIRTY)"' \
                       -I$(THEOS_PROJECT_DIR)/../../ \
                       -I$(THEOS_PROJECT_DIR)/../../../vendor/stb/ds \
                       -I$(THEOS_PROJECT_DIR)/../../../vendor/stb/image \
                       -I$(THEOS_PROJECT_DIR)/../../../vendor/stb/vorbis \
                       -I$(THEOS_PROJECT_DIR)/../../../vendor/md5 \
                       -I$(THEOS_PROJECT_DIR)/../../../vendor/sha1 \
                       -I$(THEOS_PROJECT_DIR)/../../../vendor/base64 \
                       -I$(THEOS_PROJECT_DIR)/../../../vendor/miniaudio \
                       -I$(THEOS_PROJECT_DIR)/../../audio/openal \
                       -Wno-deprecated-declarations \
                       -Wno-unused-parameter \
                       -Wno-unused-variable \
                       -Wno-unused-function \
                       -Wno-return-type \
                       -Wno-incompatible-pointer-types-discards-qualifiers \
                       -Wno-pointer-sign \
                       -Wno-sign-compare \
                       -Wno-format \
                       -Wno-implicit-function-declaration \
                       -Wno-error

Butterscotch_CCFLAGS = $(Butterscotch_CFLAGS)
Butterscotch_LDFLAGS = -Wl,-no_implicit_dylibs

include $(THEOS_MAKE_PATH)/application.mk

# -----------------------------------------------------------------------------
# .ipa packaging
#
# Theos produces a .deb (Cydia/dpkg). Most jailbroken users still
# install via .deb directly, but a .ipa is friendlier for AltStore /
# AppSync / direct SSH copy. This rule scoops the .app bundle out of
# the .deb and wraps it into a Payload/ zip = .ipa.
# -----------------------------------------------------------------------------
IPA_OUT := $(THEOS_PROJECT_DIR)/packages/$(APPLICATION_NAME).ipa
APP_PATH := $(THEOS_OBJ_DIR)/$(APPLICATION_NAME).app

.PHONY: ipa
ipa: package
	@echo "==> Packaging .ipa"
	@rm -rf $(THEOS_PROJECT_DIR)/.ipa-staging
	@mkdir -p $(THEOS_PROJECT_DIR)/.ipa-staging/Payload
	@cp -R $(APP_PATH) $(THEOS_PROJECT_DIR)/.ipa-staging/Payload/
	@cd $(THEOS_PROJECT_DIR)/.ipa-staging && zip -qry $(IPA_OUT) Payload
	@rm -rf $(THEOS_PROJECT_DIR)/.ipa-staging
	@echo "==> $(IPA_OUT)"
