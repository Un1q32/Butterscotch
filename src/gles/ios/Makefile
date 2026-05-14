# Butterscotch iOS — Theos Makefile
#
# Targets iPod Touch 2G (armv6) running jailbroken iPhone OS 3.1.3+.
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
TARGET := iphone:clang:3.1.3:3.1.3
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
# At this milestone the shell is intentionally NOT linked against the
# full Butterscotch C runtime. The renderer skeleton is included so the
# linker pulls in the GLES1 vtable symbol, and the UI in main.m is
# self-sufficient for proving the toolchain + on-device GL pipeline.
# Once that works on the iPod Touch 2G we extend BS_RUNTIME_FILES with
# the rest of src/*.c.
# -----------------------------------------------------------------------------
BS_RENDERER_FILES = \
    ../gles1_renderer.c

Butterscotch_FILES = \
    main.m \
    $(BS_RENDERER_FILES)

# -----------------------------------------------------------------------------
# Frameworks
# -----------------------------------------------------------------------------
Butterscotch_FRAMEWORKS = UIKit Foundation CoreGraphics QuartzCore OpenGLES

# -----------------------------------------------------------------------------
# Compiler flags
#   - -fno-objc-arc: iOS 3 SDK is pre-ARC; we use manual retain/release.
#   - -DBS_PLATFORM_IOS: marker for future src/*.c conditionals.
#   - -DBS_GLES1=1: marker so gles1_renderer.c picks the iOS GLES1 path.
#   - -Wno-deprecated-declarations: iOS 3.1.3 deployment is flagged
#     deprecated by clang; that's fine, we know.
# -----------------------------------------------------------------------------
Butterscotch_CFLAGS  = -fno-objc-arc \
                       -DBS_PLATFORM_IOS=1 \
                       -DBS_GLES1=1 \
                       -DENABLE_BC16=1 \
                       -I$(THEOS_PROJECT_DIR)/../../ \
                       -I$(THEOS_PROJECT_DIR)/../../../vendor/stb/ds \
                       -I$(THEOS_PROJECT_DIR)/../../../vendor/stb/image \
                       -I$(THEOS_PROJECT_DIR)/../../../vendor/stb/vorbis \
                       -I$(THEOS_PROJECT_DIR)/../../../vendor/md5 \
                       -I$(THEOS_PROJECT_DIR)/../../../vendor/miniaudio \
                       -Wno-deprecated-declarations \
                       -Wno-unused-parameter \
                       -Wno-unused-variable \
                       -Wno-unused-function

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
