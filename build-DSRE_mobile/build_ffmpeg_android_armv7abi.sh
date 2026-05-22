#!/usr/bin/env sh
set -eu

# Public-friendly DSRE-Mobile FFmpeg Android build script.
# Configure machine-local paths with environment variables.
# Required: ANDROID_NDK or NDK, LAME_PREFIX, FFmpeg source directory as current directory.

require_dir() {
  name="$1"; value="$2"
  if [ -z "$value" ] || [ ! -d "$value" ]; then
    echo "ERROR: $name not found: $value" >&2
    exit 1
  fi
}
require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "ERROR: command not found: $1" >&2
    exit 1
  fi
}

ANDROID_NDK_ROOT="${ANDROID_NDK:-${NDK:-}}"
API="${ANDROID_API:-24}"
HOST_TAG="${HOST_TAG:-linux-x86_64}"
TOOLCHAIN="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/$HOST_TAG"
LAME_PREFIX="${LAME_PREFIX:-}"
FFMPEG_PREFIX="${FFMPEG_PREFIX:-$(pwd)/android-build/armeabi-v7a}"

require_dir "ANDROID_NDK/NDK" "$ANDROID_NDK_ROOT"
require_dir "NDK toolchain" "$TOOLCHAIN"
require_dir "LAME_PREFIX" "$LAME_PREFIX"
require_cmd pkg-config
require_cmd make

PKG_CONFIG_BIN="$(command -v pkg-config)"
export PKG_CONFIG_LIBDIR="$LAME_PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH="$LAME_PREFIX/lib/pkgconfig"

./configure \
  --prefix="$FFMPEG_PREFIX" \
  --target-os=android \
  --arch="arm" \
  --cpu="armv7-a" \
  --enable-cross-compile \
  --cross-prefix="armv7a-linux-androideabi-" \
  --cc="$TOOLCHAIN/bin/armv7a-linux-androideabi${API}-clang" \
  --cxx="$TOOLCHAIN/bin/armv7a-linux-androideabi${API}-clang++" \
  --ar="$TOOLCHAIN/bin/llvm-ar" \
  --ranlib="$TOOLCHAIN/bin/llvm-ranlib" \
  --strip="$TOOLCHAIN/bin/llvm-strip" \
  --pkg-config="$PKG_CONFIG_BIN" \
  --pkg-config-flags="--static" \
  --enable-shared \
  --disable-static \
  --disable-programs \
  --disable-doc \
  --disable-debug \
  --disable-avdevice \
  --disable-postproc \
  --disable-network \
  --disable-autodetect \
  --disable-everything \
  --enable-zlib \
  --enable-protocol=file \
  --enable-protocol=pipe \
  --enable-demuxer=wav \
  --enable-demuxer=mp3 \
  --enable-demuxer=flac \
  --enable-demuxer=mov \
  --enable-demuxer=image2 \
  --enable-demuxer=mjpeg \
  --enable-muxer=wav \
  --enable-muxer=mp3 \
  --enable-muxer=flac \
  --enable-muxer=ipod \
  --enable-decoder=pcm_s16le \
  --enable-decoder=pcm_f32le \
  --enable-decoder=mp3 \
  --enable-decoder=flac \
  --enable-decoder=aac \
  --enable-decoder=alac \
  --enable-decoder=webp \
  --enable-decoder=png \
  --enable-decoder=bmp \
  --enable-decoder=mjpeg \
  --enable-encoder=pcm_s16le \
  --enable-encoder=pcm_f32le \
  --enable-encoder=libmp3lame \
  --enable-encoder=flac \
  --enable-encoder=alac \
  --enable-encoder=mjpeg \
  --enable-parser=mpegaudio \
  --enable-parser=aac \
  --enable-parser=flac \
  --enable-parser=mjpeg \
  --enable-parser=png \
  --enable-parser=bmp \
  --enable-parser=webp \
  --enable-bsfs \
  --enable-small \
  --enable-swresample \
  --enable-swscale \
  --enable-libmp3lame \
  --extra-cflags="-I$LAME_PREFIX/include" \
  --extra-ldflags="-L$LAME_PREFIX/lib" \
  --extra-libs="-lmp3lame -lz -lm"

make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
make install

echo "DONE: FFmpeg armeabi-v7a: $FFMPEG_PREFIX"
