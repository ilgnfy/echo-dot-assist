# Echo Dot 3rd gen 2018 (donut), Fire OS 6574.1: armv7, Android 7.1 bionic (API 24 NDK target), Amazon mixer + Pryon.
TARGET  := armv7a-linux-androideabi24
# Backends (src/hassmic/audio.h, wake.h) and the stock libraries they link against, from $(STOCK).
AUDIO   := src/hassmic/audio_mixer.c
WAKE    := src/hassmic/wake_pryon.c
LIBS    := libmixerAPI.so libpryon.so libopus.so libz.so

# Wi-Fi motion: the driver's own RX_STAT is the last frame from anyone on the channel; a kernel module of our own reads
# every frame from the access point (src/kmod/).  donut's kernel is 4.4.22 arm64 and carries no config of its own.
KMOD    := hassmic_rcpi4m
KVER    := 4.4.22
KARCH   := arm64
KCROSS  := $(CURDIR)/toolchain/aarch64-linux-android-4.9/bin/aarch64-linux-android-
KSRC_URL    := https://cdn.kernel.org/pub/linux/kernel/v4.x/linux-4.4.22.tar.xz
KSRC_SHA256 := 629d5661dc3b554fb0214b4635d80bb20f7977c6e814810945de31cfa86d1a0f
KCC_URL     := https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/+archive/961622e926a1b21382dba4dd9fe0e5fb3ee5ab7c.tar.gz
KCC_DIGEST  := 6465e57d85ac645a5ebe5fe0537dc3f868f85ea6bbd8d7e1d0de06b349168067
KFRAG   := devices/donut/kconfig
KCFLAGS := -fno-pic                 # Android's GCC builds PIC by default; modules use -mcmodel=large, which refuses it

# scripts/debloat.sh (the guided setup's "Remove Amazon's services" step) takes perfmonitord away; the mixer still calls three
# functions of its client library, which would hang it for 20 s each.  Build the stand-in (src/stubs/perf_stub.c).
PERF_STUB := 1

