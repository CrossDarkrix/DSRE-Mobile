#!/usr/bin/env sh
set -eu
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ABI="arm64-v8a" exec "$SCRIPT_DIR/build_libdsre_audio_c.sh"
