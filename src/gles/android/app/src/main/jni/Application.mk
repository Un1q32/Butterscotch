# Butterscotch Android — NDK Application.mk
#
# Target device: HTC Sensation XE (MSM8260 Snapdragon S3 / Scorpion,
# Adreno 220) running Android 2.3.4 Gingerbread = API level 10.
#
# Build with the LAST NDK that still ships Gingerbread sysroots:
#   * NDK r10e — GCC 4.8, API 3..21 sysroots (recommended).
#   * NDK r12b — last with a real API 9 sysroot + clang 3.8.
# Newer NDKs (r17+) dropped everything below API 16, so they CANNOT
# produce a Gingerbster-compatible binary.

# Adreno 220 is an armv7-a part with VFPv3. armeabi-v7a is the right ABI.
# (armeabi — plain armv5 — would also run but loses the FPU; not needed
# for a device this capable.)
APP_ABI := armeabi-v7a

# Android 2.3 Gingerbster.
APP_PLATFORM := android-9

# GCC 4.8 is the newest compiler that targets the r10e Gingerbster
# sysroot cleanly. (If you build with r12b instead, switch this to
# clang3.8 — but GCC 4.8 is the safe default for r10e.)
APP_STL := gnustl_static
NDK_TOOLCHAIN_VERSION := 4.8

# The runtime is C11-ish (uses _Generic-free C). -O2 keeps the VM fast
# on a 1.5 GHz Scorpion; size is not the constraint here (unlike the
# 128 MB iPod Touch 2G).
APP_CFLAGS   := -O2 -fno-strict-aliasing
APP_OPTIM    := release
