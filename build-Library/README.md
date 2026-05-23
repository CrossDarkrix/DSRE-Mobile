# DSRE-Mobile Build chart.

`build.py` coordinates the existing FFmpeg and native build scripts from one entry point.

```sh
python3 build.py configure   --ndk "$ANDROID_NDK"   --host-tag linux-x86_64   --ffmpeg-source ./ffmpeg   --lame-root ./lame-3.100/android-build

python3 build.py all --abis all
```

Partial builds:

```sh
python3 build.py ffmpeg --abi arm64-v8a --sync-native
python3 build.py native --abi arm64-v8a
python3 build.py all --abis all --dry-run
```

Notes:

- `build.py` does not download FFmpeg or LAME source code.
- FFmpeg source must contain `./configure`.
- LAME prefixes are expected at `<lame-root>/<ABI>` unless `--lame-prefix` is provided.
- `all` builds FFmpeg, copies `include/` and `lib/` into `dsre_native/ffmpeg/<ABI>/`, then builds `libdsre_audio.so`.
- The FFmpeg scripts keep the existing LGPL-oriented intent: no `--enable-gpl`, no `--enable-version3`, no `--enable-nonfree`.
