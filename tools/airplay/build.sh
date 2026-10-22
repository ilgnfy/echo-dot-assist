#!/bin/bash
# Builds the two programs of AirPlay 2 for the Echo Dot 3 as static glibc ARM binaries (see docs/airplay.md for why glibc), into
# devices/donut/prebuilt/airplay/.  Runs on Linux (Debian: gcc-arm-linux-gnueabihf, autoconf, automake, libtool, pkg-config, git, curl).
#   tools/airplay/build.sh [WORKDIR]        default ~/airplay; first tools/airplay/cross-libs.sh with GNU=1 builds the libraries
set -eu
W=${1:-$HOME/airplay}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/../../devices/donut/prebuilt/airplay
H=arm-linux-gnueabihf
P=$W/cross/prefix-gnu
[ -f "$P/lib/libcrypto.a" ] || GNU=1 "$HERE/cross-libs.sh" "$W/cross"
CF="-Os -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard"
mkdir -p "$OUT"

# the clock: nqptp 1.2.9 (bionic would need shims for pthread cancellation and shm_open; glibc does not)
[ -d "$W/nqptp" ] || git clone -q --branch 1.2.9 https://github.com/mikebrady/nqptp.git "$W/nqptp"
( cd "$W/nqptp" && { make distclean >/dev/null 2>&1 || true; } && autoreconf -fi >/dev/null 2>&1 &&
  CFLAGS="$CF" LDFLAGS=-static ./configure --host=$H >/dev/null && make -j"$(nproc)" >/dev/null && $H-strip nqptp && cp nqptp "$OUT/" )

# the receiver: shairport-sync at the commit the patch was made against, AirPlay 2, its audio to a pipe, mDNS as avahi service files
[ -d "$W/shairport-sync" ] || git clone -q https://github.com/mikebrady/shairport-sync.git "$W/shairport-sync"
( cd "$W/shairport-sync" && git checkout -q -f b991d91 && git apply "$HERE"/patches/*.patch &&
  { make distclean >/dev/null 2>&1 || true; } && autoreconf -fi >/dev/null 2>&1 &&
  PKG_CONFIG_PATH=$P/lib/pkgconfig PKG_CONFIG_LIBDIR=$P/lib/pkgconfig CFLAGS="$CF -I$P/include" LDFLAGS="-static -L$P/lib" LIBS=-lm \
    ./configure --host=$H --with-ssl=openssl --with-pipe --with-ffmpeg --with-airplay-2 --with-external-mdns --without-libdaemon >/dev/null &&
  # libgcrypt wants libgpg-error after it; libcrypto and libgcrypt both define gf_mul (Classic McEliece, Ed448), which nothing here calls
  make -j"$(nproc)" LDFLAGS="-static -Wl,--allow-multiple-definition -L$P/lib" LIBS="$(grep ^LIBS Makefile | cut -d= -f2-) -lgpg-error -lm -lpthread -ldl" >/dev/null &&
  $H-strip shairport-sync && cp shairport-sync "$OUT/" )
ls -la "$OUT"
