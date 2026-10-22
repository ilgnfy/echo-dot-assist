#!/bin/bash
# Cross-builds the libraries shairport-sync (AirPlay 2) needs for the Echo Dot 3, static, with the project's NDK (r21e, armv7, API 24, bionic):
# OpenSSL, libsodium, libgpg-error + libgcrypt, libplist, libconfig, popt, libuuid (util-linux) and a minimal FFmpeg (the AAC and ALAC decoders and the
# resampler: the firmware's FFmpeg has no libavformat or libswresample, and its version would have to match the headers).  Runs on Linux.
#   tools/airplay/cross-libs.sh [WORKDIR]        default ~/airplay/cross; the result is WORKDIR/prefix (headers, .a, .pc)
# GNU=1 builds against glibc with Debian's arm-linux-gnueabihf-gcc instead, for a fully static binary: bionic has no pthread_cancel, which
# shairport-sync needs, and a static glibc binary runs on the Echo's kernel without a loader (prefix WORKDIR/prefix-gnu).
# docs/airplay.md says what each piece is for and why it is the way it is.
set -eu
W=${1:-$HOME/airplay/cross}
NDK=${NDK:-$(cd "$(dirname "$0")/../.." && pwd)/toolchain/android-ndk-r21e}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64
API=24
if [ "${GNU:-0}" = 1 ]; then
    HOST=arm-linux-gnueabihf; P=$W/prefix-gnu; B=$W/build-gnu
    export CC=$HOST-gcc CXX=$HOST-g++ AR=$HOST-ar RANLIB=$HOST-ranlib STRIP=$HOST-strip NM=$HOST-nm
    export CFLAGS="-Os -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard -fPIC -I$P/include" CXXFLAGS="-Os -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard -fPIC" CPPFLAGS="-I$P/include" LDFLAGS="-L$P/lib"
    SSLT="linux-generic32 no-asm"; FFOS="--target-os=linux --cross-prefix=$HOST-"; FFCC=""
else
    TRIPLE=armv7a-linux-androideabi$API
    HOST=arm-linux-androideabi; P=$W/prefix; B=$W/build
    export PATH=$TC/bin:$PATH
    export CC=$TRIPLE-clang CXX=$TRIPLE-clang++ AR=llvm-ar RANLIB=llvm-ranlib STRIP=llvm-strip NM=llvm-nm LD=ld.lld
    export CFLAGS="-Os -fPIE -fPIC -fno-integrated-as -I$P/include" CXXFLAGS="-Os -fPIE -fPIC -I$P/include" CPPFLAGS="-I$P/include" LDFLAGS="-L$P/lib"
    SSLT="linux-generic32 no-asm -D__ANDROID_API__=$API"; FFOS="--target-os=android"
fi
mkdir -p "$W/src" "$B" "$P"
export PKG_CONFIG_PATH=$P/lib/pkgconfig PKG_CONFIG_LIBDIR=$P/lib/pkgconfig
log() { echo "== $(date +%T) $*"; }
fetch() {   # URL: into src/, unpacked into build/<name>
    f=$W/src/${1##*/}
    [ -f "$f" ] || curl -fsSL --retry 3 -o "$f" "$1"
    d=$B/$(basename "$f" | sed -E 's/\.tar\.(gz|bz2|xz)$//')
    [ -d "$d" ] || { mkdir -p "$B"; tar -C "$B" -xf "$f"; }
    echo "$d"
}
autobuild() {   # DIR configure-args...
    d=$1; shift
    ( cd "$d" && { [ -f Makefile ] || ./configure --host=$HOST --prefix="$P" --disable-shared --enable-static "$@" ; } && make -j"$(nproc)" && make install )
}

step() { m=$W/.done-$1${GNU:+-gnu}; [ -f "$m" ] && { log "$1 done before"; return 0; }; log "$1"; "$@"; touch "$m"; }

openssl() {
    d=$(fetch https://www.openssl.org/source/openssl-3.0.16.tar.gz)
    ( cd "$d" && { make distclean >/dev/null 2>&1 || true; }
        ./Configure $SSLT no-shared no-tests no-ssl3 no-comp no-engine no-dso no-legacy \
        --prefix="$P" --openssldir="$P/ssl" && make build_generated && make -j"$(nproc)" libcrypto.a libssl.a && make install_dev )
}
libsodium() { d=$(fetch https://download.libsodium.org/libsodium/releases/libsodium-1.0.20-stable.tar.gz); autobuild "$B/libsodium-stable"; }
gpgerror() {
    d=$(fetch https://gnupg.org/ftp/gcrypt/libgpg-error/libgpg-error-1.50.tar.bz2)
    # configure reads the size of a lock from the compiled object with objdump and gets 0 (the lock then is 4 bytes: "gpgrt fatal: sizeof lock obj"
    # at start); the soft-float ARM description has the layout the hard-float ABI has, so it replaces the generated one
    ( cd "$d" && [ -f Makefile ] || ./configure --host=$HOST --prefix="$P" --disable-shared --enable-static --disable-doc --disable-tests --disable-nls --disable-languages )
    cp "$d/src/syscfg/lock-obj-pub.arm-unknown-linux-gnueabi.h" "$d/src/lock-obj-pub.native.h"
    ( cd "$d" && make -j"$(nproc)" && make install )
}
gcrypt() { d=$(fetch https://gnupg.org/ftp/gcrypt/libgcrypt/libgcrypt-1.11.0.tar.bz2); autobuild "$d" --disable-doc --disable-asm --with-libgpg-error-prefix="$P"; }
plist() { d=$(fetch https://github.com/libimobiledevice/libplist/releases/download/2.6.0/libplist-2.6.0.tar.bz2); autobuild "$d" --without-cython --disable-tools --disable-man 2>/dev/null || autobuild "$d" --without-cython; }
libconfig() { d=$(fetch https://github.com/hyperrealm/libconfig/releases/download/v1.7.3/libconfig-1.7.3.tar.gz); autobuild "$d" --disable-examples --disable-tests --disable-cxx; }
popt() { d=$(fetch http://ftp.rpm.org/popt/releases/popt-1.x/popt-1.19.tar.gz); autobuild "$d" --disable-nls ac_cv_header_glob_h=no ac_cv_func_glob=no; }
libuuid() { d=$(fetch https://www.kernel.org/pub/linux/utils/util-linux/v2.40/util-linux-2.40.2.tar.xz); autobuild "$d" --disable-all-programs --enable-libuuid --disable-nls --without-python --without-systemd --without-udev --disable-year2038; }
ffmpeg() {
    d=$(fetch https://ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz)
    ( cd "$d" && ./configure --prefix="$P" --enable-cross-compile $FFOS --arch=arm --cpu=armv7-a --cc="$CC" --ar="$AR" --ranlib="$RANLIB" --nm="$NM" --strip="$STRIP" \
        --disable-everything --disable-programs --disable-doc --disable-avdevice --disable-swscale --disable-postproc --disable-avfilter --disable-network --disable-iconv \
        --disable-debug --disable-asm --enable-small --enable-pic --disable-shared --enable-static \
        --enable-decoder=aac,aac_latm,alac,pcm_s16le,pcm_s24le --enable-parser=aac,aac_latm --enable-demuxer=aac,mov --enable-protocol=file \
        --enable-swresample --enable-avformat --enable-avcodec --extra-cflags="$CFLAGS" && make -j"$(nproc)" && make install )
}

for s in openssl libsodium gpgerror gcrypt plist libconfig popt libuuid ffmpeg; do step $s; done
log "all libraries in $P"
ls "$P/lib" | head -40
