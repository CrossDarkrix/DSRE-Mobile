#!/usr/bin/env sh
set -eu
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ABI="armeabi-v7a" exec "$SCRIPT_DIR/build_libdsre_audio_c.sh"
