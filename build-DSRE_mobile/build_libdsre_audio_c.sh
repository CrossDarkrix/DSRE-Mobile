#!/usr/bin/env sh
set -eu

# Build DSRE native C library for one Android ABI.
# Usage: ABI=arm64-v8a ./build_libdsre_audio_c.sh

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ANDROID_NDK_ROOT="${ANDROID_NDK:-${NDK:-}}"
ABI="${ABI:-arm64-v8a}"
ANDROID_PLATFORM="${ANDROID_PLATFORM:-android-24}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
NATIVE_DIR="${DSRE_NATIVE_DIR:-$SCRIPT_DIR/dsre_native}"
BUILD_DIR="${BUILD_DIR:-$NATIVE_DIR/build-$ABI}"
OUT_DIR="${NATIVE_LIBS_DIR:-$SCRIPT_DIR/native_libs/$ABI}"

if [ -z "$ANDROID_NDK_ROOT" ] || [ ! -d "$ANDROID_NDK_ROOT" ]; then
  echo "ERROR: ANDROID_NDK/NDK not found: $ANDROID_NDK_ROOT" >&2
  exit 1
fi
if [ ! -d "$NATIVE_DIR/ffmpeg/$ABI/lib" ]; then
  echo "ERROR: FFmpeg libraries not found: $NATIVE_DIR/ffmpeg/$ABI/lib" >&2
  exit 1
fi
mkdir -p "$OUT_DIR"
rm -rf "$BUILD_DIR"
cmake -S "$NATIVE_DIR" -B "$BUILD_DIR"   -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake"   -DANDROID_ABI="$ABI"   -DANDROID_PLATFORM="$ANDROID_PLATFORM"   -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE"
cp "$BUILD_DIR/libdsre_audio.so" "$OUT_DIR/"
echo "DONE: $OUT_DIR/libdsre_audio.so"
