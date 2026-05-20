# DSRE-Mobile

**Deep Sound Resolution Enhancer for Android**

DSRE-Mobile is an Android port of **DSRE / Deep Sound Resolution Enhancer**. It enhances audio files locally on Android devices using a lightweight DSP-oriented pipeline and a native FFmpeg-based audio backend.

- Original concept: [DSRE / Digital Sound Resolution Enhancer English](https://github.com/Urabewe/DSRE---Digital-Sound-Resolution-Enhancer-English)
- Android release page: [DSRE-Mobile Releases](https://github.com/CrossDarkrix/DSRE-Mobile/releases)

> DSRE-Mobile is an experimental audio enhancement tool for local, batch-oriented audio processing on Android.

---

## Features

- **Android-native audio enhancement**
  - Runs locally on Android.
  - Uses a native FFmpeg-based audio backend.
- **Batch processing**
  - Process multiple audio files in one run.
- **Multiple output formats**
  - ALAC / M4A
  - FLAC
  - MP3 using `libmp3lame`
- **Cover art and metadata preservation**
  - Attempts to preserve embedded cover images and metadata from the original file.
  - Cover art handling is best-effort and depends on the source image format and enabled FFmpeg codecs.
- **Music-folder output**
  - Designed to write enhanced files directly to `Music/DSRE` when Android All files access is granted.
- **Configurable enhancement parameters**
  - Harmonic generation
  - Enhancement strength
  - Sample rate
  - Stereo width
  - Dynamic response
  - DSP context
  - Streaming chunk size
  - GC interval
- **Streaming / chunk-based processing**
  - Uses a streaming pipeline to reduce memory pressure when processing large files.

---

## Concept

DSRE-Mobile is inspired by high-frequency restoration and audio enhancement systems such as Sony DSEE HX, but it does **not** use a deep-learning model. Instead, it uses a lightweight DSP-oriented approach suitable for mobile devices.

The goal is not to perfectly reconstruct information that no longer exists in a compressed source. Instead, DSRE-Mobile aims to add controlled high-frequency harmonic detail and spatial enhancement while keeping processing cost low enough for Android devices.

---

## Technical Overview

### Native audio backend

DSRE-Mobile uses a native C audio backend built around FFmpeg libraries.

Typical native components include:

- `libdsre_audio.so`
- `libavcodec.so`
- `libavformat.so`
- `libavutil.so`
- `libswresample.so`
- `libswscale.so`
- `libmp3lame.so` for MP3 encoding

The Python/Kivy layer calls the native library through `ctypes`.

### FFmpeg-based decoding and encoding

The native backend performs:

1. Input audio decoding
2. Resampling to the configured target sample rate
3. Float32 interleaved PCM streaming
4. DSRE enhancement processing
5. Output encoding through FFmpeg

Supported output encoders depend on the FFmpeg build configuration.

For MP3 output, DSRE-Mobile uses `libmp3lame`. When dynamic linking is used, the APK must include the matching ABI version of `libmp3lame.so`.

### Supported Android ABIs

Current builds may include native libraries for:

- `arm64-v8a`
- `armeabi-v7a`

Each ABI must contain its own matching `.so` files. Mixing `arm64-v8a` and `armeabi-v7a` libraries will cause native loading errors.

---

## Processing Pipeline

DSRE-Mobile uses a streaming audio pipeline.

```text
Input file
  ↓
FFmpeg decoder
  ↓
Float32 PCM stream
  ↓
DSRE enhancement process
  ↓
Overlap-aware chunk processing
  ↓
FFmpeg encoder
  ↓
Output file in Music/DSRE
```

---

## Overlap-Based Processing

DSRE-Mobile processes audio in chunks to reduce memory usage. However, processing each chunk independently can create discontinuities at chunk boundaries. To reduce boundary artifacts, the pipeline uses an overlap-aware design.

### Why overlap is needed

When DSP processing uses contextual information around each sample, the beginning and end of each chunk can be less stable than the center of the chunk. This is especially noticeable when:

- enhancing high-frequency content,
- applying stereo/spatial processing,
- using dynamic response shaping,
- processing long files in small streaming chunks.

### How the overlap model works

The processor keeps a configurable DSP context around chunk boundaries. The overlapped area gives the algorithm extra surrounding samples so that each chunk can be processed more naturally.

Conceptually:

```text
Previous context | Current processing region | Next context
```

Only the stable center region is treated as the main output region. The context area is used to make processing smoother and reduce edge artifacts.

### Related parameter

- **DSP Context**
  - Controls how much surrounding audio context is used around each chunk.
  - Larger values may improve boundary stability but increase processing cost.

---

## Android Storage and Permissions

DSRE-Mobile is designed to save output files directly to:

```text
/storage/emulated/0/Music/DSRE
```

It may also use:

```text
/storage/emulated/0/Documents/DSRE
```

for configuration or diagnostic files, depending on the build.

Because modern Android restricts direct shared-storage access, DSRE-Mobile expects **All files access** when direct Music/Documents folder access is required.

Required permission flow:

1. Install DSRE-Mobile.
2. Open the app.
3. When prompted, open Android settings.
4. Enable **All files access** for DSRE-Mobile.
5. Return to the app and start processing again.

### Suggested Buildozer permissions

A typical local/sideload build may use:

```ini
android.permissions = android.permission.INTERNET,android.permission.READ_MEDIA_AUDIO,(name=android.permission.READ_EXTERNAL_STORAGE;maxSdkVersion=32),(name=android.permission.WRITE_EXTERNAL_STORAGE;maxSdkVersion=28),android.permission.MANAGE_EXTERNAL_STORAGE
```

> Note: `MANAGE_EXTERNAL_STORAGE` is a special Android permission. This project is intended for local or sideload distribution. If you plan to publish to Google Play, review Google's storage permission policies first.

---

## Parameters

| Parameter | Description |
|---|---|
| Harmonic | Controls the amount of harmonic enhancement. |
| Strength | Controls the overall enhancement intensity. |
| Sample Rate | Sets the target output sample rate. |
| Stereo Width | Adjusts stereo image width. |
| Dynamic | Controls dynamic response behavior. |
| DSP Context | Sets the amount of overlap/context used around streaming chunks. |
| Stream Chunk | Controls streaming chunk length. |
| GC Interval | Controls garbage collection interval during long processing runs. |
| Output Format | Selects ALAC, FLAC, or MP3 output. |

Recommended balanced settings may depend on the source file and device performance.

---

## Output Formats

### ALAC / M4A

Recommended when you want Apple-compatible lossless output.

### FLAC

Recommended when you want open lossless output.

### MP3

Recommended when compatibility and file size are more important than lossless quality.

MP3 output uses `libmp3lame`. If MP3 conversion fails, check that:

- FFmpeg was built with `--enable-libmp3lame`.
- `libmp3lame.so` is included in the APK for each target ABI.
- `libavcodec.so` can find `libmp3lame.so` at runtime.

---

## Cover Art and Metadata

DSRE-Mobile attempts to preserve metadata and embedded cover art.

For MP3 output, metadata compatibility can be sensitive because different players support different ID3 versions and text encodings. This build is intended to prefer ID3v2.3 and avoid ID3v1 when possible.

### Cover art requirements

For cover image preservation, the FFmpeg build should include support for:

- MJPEG decoder
- PNG decoder if the source cover is PNG
- BMP decoder if the source cover is BMP
- WebP decoder if the source cover is WebP
- MJPEG encoder
- `libswscale`

A missing MJPEG encoder can prevent cover art from being converted and attached.

### Known metadata limitation

If the input file already appears garbled in `ffprobe`, DSRE-Mobile cannot reliably reconstruct the original tag text. In that case, the source file should be fixed with a tag editor before processing.

Example check:

```bash
ffprobe -hide_banner -show_format input.mp3
```

If the displayed title, artist, or album is already garbled, the input metadata is being decoded incorrectly before DSRE-Mobile receives it.

---

## Native Library Checklist

If the app starts but processing does not begin, check native dependencies.

### Check APK contents

```bash
unzip -l app.apk | grep '\.so'
```

Expected layout:

```text
lib/arm64-v8a/libdsre_audio.so
lib/arm64-v8a/libavcodec.so
lib/arm64-v8a/libavformat.so
lib/arm64-v8a/libavutil.so
lib/arm64-v8a/libswresample.so
lib/arm64-v8a/libswscale.so
lib/arm64-v8a/libmp3lame.so
```

and/or:

```text
lib/armeabi-v7a/libdsre_audio.so
lib/armeabi-v7a/libavcodec.so
lib/armeabi-v7a/libavformat.so
lib/armeabi-v7a/libavutil.so
lib/armeabi-v7a/libswresample.so
lib/armeabi-v7a/libswscale.so
lib/armeabi-v7a/libmp3lame.so
```

### Check dependencies

```bash
llvm-readelf -d libdsre_audio.so | grep NEEDED
llvm-readelf -d libavcodec.so | grep NEEDED
```

If `libmp3lame.so` appears in `NEEDED`, the matching ABI version must be packaged in the APK.

---

## Build Notes

This project uses Android-native shared libraries. When building for multiple ABIs, build and package each ABI separately.

Example ABI folders:

```text
native_libs/arm64-v8a/
native_libs/armeabi-v7a/
```

Buildozer example:

```ini
android.add_libs = native_libs/*/*.so
android.archs = arm64-v8a, armeabi-v7a
```

### FFmpeg license-oriented build notes

For the public release build, DSRE-Mobile is intended to use an LGPL-oriented FFmpeg configuration.

The FFmpeg build should avoid GPL/nonfree options such as:

```text
--enable-gpl
--enable-version3
--enable-nonfree
```

A typical release build should use dynamic shared libraries:

```text
--enable-shared
--disable-static
--disable-programs
```

The current audio and cover-art feature set is intended to work without enabling GPL-only FFmpeg components.

---

## License

The DSRE-Mobile source code in this repository is licensed under the MIT License unless otherwise noted.

The distributed Android APK may include third-party native libraries, including FFmpeg and LAME/libmp3lame. These components are not licensed under the MIT License and remain under their respective licenses.

- FFmpeg is licensed under LGPL v2.1 or later by default. Its effective license may change depending on configure options such as `--enable-gpl`.
- The release build is intended to avoid GPL/nonfree FFmpeg configure options and use an LGPL-oriented FFmpeg configuration.
- LAME/libmp3lame is licensed under the LGPL.
- Users and redistributors are responsible for complying with the licenses of all bundled third-party components.

See `THIRD_PARTY_NOTICES.md` for details.

---

## Troubleshooting

### MP3 conversion fails

Check that:

- `libmp3lame` was cross-compiled for Android.
- FFmpeg was configured with `--enable-libmp3lame`.
- `libmp3lame.so` is included in the APK.
- The ABI matches the device.

### Cover art disappears

Check that the FFmpeg build includes:

- MJPEG decoder
- PNG decoder if the source cover is PNG
- MJPEG encoder
- `libswscale`

A missing MJPEG encoder can prevent cover art from being converted and attached.

### Tags are garbled in MP3 output

Check the input first:

```bash
ffprobe -hide_banner -show_format input.mp3
```

If the input is already garbled in `ffprobe`, fix the source tags with a tag editor before processing.

### Processing does not start

Check:

- All files access is enabled.
- The input file list is not empty.
- Native `.so` dependencies are included in the APK.
- `libdsre_audio.so` exports the required streaming symbols.
- `adb logcat` for `dlopen failed`, `UnsatisfiedLinkError`, or `cannot locate symbol`.

---

## Disclaimer

DSRE-Mobile is an experimental audio enhancement application. The perceived effect depends on the source material, selected parameters, playback device, and listener preference.

This project does not claim to restore lost audio information perfectly. It provides a configurable enhancement pipeline for local experimentation and practical audio up-conversion workflows on Android.

---

## Credits

- Original DSRE concept: [DSRE / Digital Sound Resolution Enhancer English](https://github.com/Urabewe/DSRE---Digital-Sound-Resolution-Enhancer-English)
- Android port: [DSRE-Mobile](https://github.com/CrossDarkrix/DSRE-Mobile)
- FFmpeg: [FFmpeg project](https://ffmpeg.org/)
- LAME MP3 encoder: [LAME project](https://lame.sourceforge.io/)
