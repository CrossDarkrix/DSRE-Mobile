# Third-Party Notices

This file lists third-party open-source components that are included in, linked by, or commonly distributed with DSRE-Mobile release builds.

DSRE-Mobile source code is licensed separately under the MIT License unless otherwise noted. Third-party components remain under their own licenses.

> This document is provided for transparency and release packaging. It is not legal advice. Redistributors are responsible for verifying the exact license obligations of the binaries they distribute.

---

## DSRE-Mobile

- Project: [DSRE-Mobile](https://github.com/CrossDarkrix/DSRE-Mobile)
- License: MIT License, unless otherwise noted in individual files.

The MIT License applies to DSRE-Mobile project code owned by the DSRE-Mobile authors. It does not relicense third-party libraries bundled with the APK.

---

## FFmpeg

- Project: [FFmpeg](https://ffmpeg.org/)
- License: LGPL v2.1 or later by default, depending on the exact build configuration.
- Source code: [FFmpeg source repository](https://git.ffmpeg.org/ffmpeg.git)
- License information: [FFmpeg Legal](https://ffmpeg.org/legal.html)

DSRE-Mobile may include FFmpeg shared libraries such as:

- `libavcodec.so`
- `libavformat.so`
- `libavutil.so`
- `libswresample.so`
- `libswscale.so`

For public release builds, DSRE-Mobile is intended to use an LGPL-oriented FFmpeg configuration and avoid GPL/nonfree configure options such as:

```text
--enable-gpl
--enable-version3
--enable-nonfree
```

If FFmpeg is built with GPL-related options such as `--enable-gpl`, the effective license of that FFmpeg binary may change to GPL. In that case, the release package must be handled according to the applicable GPL terms.

### FFmpeg build configuration used by DSRE-Mobile

Release builds should document the exact FFmpeg configure line used to build the distributed native libraries.

Example LGPL-oriented intent:

```text
--enable-shared
--disable-static
--disable-programs
--disable-everything
--enable-libmp3lame
--enable-swresample
--enable-swscale
```

The exact configure line may differ by ABI and release version. The distributed FFmpeg source package, if provided, should correspond to the actual FFmpeg binaries included in the APK.

---

## LAME / libmp3lame

- Project: [LAME MP3 Encoder](https://lame.sourceforge.io/)
- License: LGPL
- Source code: [LAME downloads](https://sourceforge.net/projects/lame/files/lame/)

DSRE-Mobile may include `libmp3lame.so` for MP3 encoding.

Typical bundled files may include:

```text
lib/arm64-v8a/libmp3lame.so
lib/armeabi-v7a/libmp3lame.so
```

LAME/libmp3lame remains under its own license. If LAME is modified, those modifications must be handled according to the LAME/LGPL license terms.

---

## Android Native Libraries

DSRE-Mobile release APKs may include native shared libraries under ABI-specific directories, for example:

```text
lib/arm64-v8a/*.so
lib/armeabi-v7a/*.so
```

Each native library remains under its own applicable license. The DSRE-Mobile MIT License does not override the license of bundled third-party native libraries.

---

## Notes for Redistributors

If you redistribute DSRE-Mobile APKs or modified builds, you should verify:

1. The exact FFmpeg configure options used for each ABI.
2. Whether the FFmpeg build is LGPL-oriented or GPL-enabled.
3. Whether all bundled native libraries have matching license notices.
4. Whether corresponding source code or source links are provided where required.
5. Whether any locally modified third-party library source code is made available under the applicable license.

For LGPL-oriented FFmpeg builds, the FFmpeg project recommends documenting the source code used, the configure line, and the relationship between the distributed binaries and the corresponding source.

---

## No Warranty

Third-party components are provided under their respective licenses and warranty disclaimers. DSRE-Mobile itself is provided as an experimental audio enhancement application without warranty, as described in the project license and README.
