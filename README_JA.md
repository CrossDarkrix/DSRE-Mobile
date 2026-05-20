<div align="center">
	<a href="https://github.com/CrossDarkrix/DSRE-Mobile">
	<img width="150px" height="150px" alt="DSRE-Mobile" src="https://raw.githubusercontent.com/CrossDarkrix/DSRE-Mobile/refs/heads/main/icon.png"></a>
</div>

---

# DSRE-Mobile

[English README](README.md) | [日本語版 README](README_JA.md)

**Android向け Deep Sound Resolution Enhancer**

DSRE-Mobile は、**DSRE / Deep Sound Resolution Enhancer** のAndroid移植版です。軽量なDSP指向の処理パイプラインと、FFmpegベースのネイティブ音声バックエンドを使用して、Android端末上でローカルに音声ファイルを強化します。

- Original concept: [DSRE / Digital Sound Resolution Enhancer English](https://github.com/Urabewe/DSRE---Digital-Sound-Resolution-Enhancer-English)
- Android release page: [DSRE-Mobile Releases](https://github.com/CrossDarkrix/DSRE-Mobile/releases)

> DSRE-Mobile は、Android上でローカルかつバッチ処理向けに音声処理を行う実験的なオーディオエンハンスメントツールです。

---

## スクリーンショット

<img src="https://raw.githubusercontent.com/CrossDarkrix/DSRE-Mobile/refs/heads/main/image/screenshot1.jpg" align="center" width="150px" height="300px" alt="android1"><img src="https://raw.githubusercontent.com/CrossDarkrix/DSRE-Mobile/refs/heads/main/image/screenshot2.jpg" align="center" width="150px" height="300px" alt="android2">


---

## 特徴

- **Androidネイティブの音声強化**
  - Android端末上でローカルに動作します。
  - FFmpegベースのネイティブ音声バックエンドを使用します。
- **バッチ処理**
  - 複数の音声ファイルをまとめて処理できます。
- **複数の出力形式**
  - ALAC / M4A
  - FLAC
  - `libmp3lame` を使用したMP3
- **カバー画像とメタデータの保持**
  - 元ファイルに埋め込まれたカバー画像とメタデータの保持を試みます。
  - カバー画像の保持はベストエフォートであり、入力画像形式と有効化されているFFmpeg codecに依存します。
- **Musicフォルダへの出力**
  - Androidの「すべてのファイルへのアクセス」が許可されている場合、`Music/DSRE` へ直接出力することを想定しています。
- **調整可能なエンハンスメントパラメータ**
  - Harmonic generation
  - Enhancement strength
  - Sample rate
  - Stereo width
  - Dynamic response
  - DSP context
  - Streaming chunk size
  - GC interval
- **ストリーミング / チャンクベース処理**
  - 大きなファイルを処理する際のメモリ負荷を抑えるため、ストリーミング処理パイプラインを使用します。

---

## コンセプト

DSRE-Mobile は、Sony DSEE HX のような高周波補完・音声強化システムに着想を得ていますが、**ディープラーニングモデルは使用していません**。代わりに、モバイル端末でも扱いやすい軽量なDSP指向アプローチを採用しています。

目的は、圧縮音源から失われた情報を完全に復元することではありません。DSRE-Mobile は、Android端末上で現実的な処理負荷に収めながら、制御された高周波倍音成分と空間的な広がりを加えることを目指しています。

---

## 技術概要

### ネイティブ音声バックエンド

DSRE-Mobile は、FFmpegライブラリを中心としたネイティブC音声バックエンドを使用します。

代表的なネイティブコンポーネントは以下です。

- `libdsre_audio.so`
- `libavcodec.so`
- `libavformat.so`
- `libavutil.so`
- `libswresample.so`
- `libswscale.so`
- MP3エンコード用の `libmp3lame.so`

Python/Kivy層は、`ctypes` を通じてネイティブライブラリを呼び出します。

### FFmpegベースのデコードとエンコード

ネイティブバックエンドは以下を行います。

1. 入力音声のデコード
2. 設定されたターゲットサンプリングレートへのリサンプリング
3. Float32 interleaved PCM のストリーミング
4. DSREエンハンスメント処理
5. FFmpegによる出力エンコード

利用可能な出力エンコーダは、FFmpegのビルド構成に依存します。

MP3出力では、DSRE-Mobile は `libmp3lame` を使用します。動的リンクを使用する場合、APKには対象ABIに一致する `libmp3lame.so` を含める必要があります。

### 対応Android ABI

現在のビルドでは、以下のネイティブライブラリを含めることがあります。

- `arm64-v8a`
- `armeabi-v7a`

各ABIには、そのABI向けにビルドされた `.so` ファイルを含める必要があります。`arm64-v8a` と `armeabi-v7a` のライブラリを混在させると、ネイティブライブラリのロードエラーが発生します。

---

## 処理パイプライン

DSRE-Mobile はストリーミング音声処理パイプラインを使用します。

```text
入力ファイル
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
Music/DSRE への出力ファイル
```

---

## Overlap-Based Processing

DSRE-Mobile は、メモリ使用量を抑えるために音声をチャンク単位で処理します。ただし、各チャンクを完全に独立して処理すると、チャンク境界で不連続や違和感が発生する可能性があります。この境界アーティファクトを抑えるため、DSRE-Mobile は overlap-aware な設計を使用しています。

### なぜオーバーラップが必要か

DSP処理が各サンプル周辺の文脈情報を使用する場合、チャンクの先頭と末尾は、チャンク中央部に比べて安定しにくくなることがあります。これは特に以下の場合に目立ちます。

- 高周波成分を強調する場合
- ステレオ / 空間処理を適用する場合
- 動的応答の整形を行う場合
- 長いファイルを小さなストリーミングチャンクで処理する場合

### オーバーラップモデルの仕組み

プロセッサは、チャンク境界の周辺に設定可能なDSP contextを保持します。オーバーラップ領域は、各チャンクがより自然に処理されるよう、周辺サンプル情報を追加で与えます。

概念的には以下の形です。

```text
Previous context | Current processing region | Next context
```

主な出力領域として扱うのは、安定した中央部分です。context領域は処理を滑らかにし、境界アーティファクトを減らすために使用されます。

### 関連パラメータ

- **DSP Context**
  - 各チャンク周辺で使用する音声コンテキスト量を制御します。
  - 値を大きくすると境界の安定性が向上する可能性がありますが、処理コストも増加します。

---

## Androidストレージと権限

DSRE-Mobile は、出力ファイルを以下へ直接保存することを想定しています。

```text
/storage/emulated/0/Music/DSRE
```

ビルドによっては、設定ファイルや診断ファイルのために以下を使用する場合もあります。

```text
/storage/emulated/0/Documents/DSRE
```

近年のAndroidでは共有ストレージへの直接アクセスが制限されているため、Music/Documentsフォルダへ直接アクセスする場合、DSRE-Mobile は **すべてのファイルへのアクセス** を必要とします。

必要な権限許可の流れは以下です。

1. DSRE-Mobileをインストールします。
2. アプリを開きます。
3. 案内が表示されたらAndroid設定を開きます。
4. DSRE-Mobileに対して **すべてのファイルへのアクセス** を有効にします。
5. アプリに戻り、処理を再度開始します。

### Buildozer権限設定例

ローカル配布 / サイドロード用ビルドでは、以下のような設定を使用できます。

```ini
android.permissions = android.permission.INTERNET,android.permission.READ_MEDIA_AUDIO,(name=android.permission.READ_EXTERNAL_STORAGE;maxSdkVersion=32),(name=android.permission.WRITE_EXTERNAL_STORAGE;maxSdkVersion=28),android.permission.MANAGE_EXTERNAL_STORAGE
```

> Note: `MANAGE_EXTERNAL_STORAGE` はAndroidの特殊な権限です。このプロジェクトはローカル配布またはサイドロード配布を想定しています。Google Playで公開する場合は、Googleのストレージ権限ポリシーを事前に確認してください。

---

## パラメータ

| Parameter | 説明 |
|---|---|
| Harmonic | 倍音強調の量を制御します。 |
| Strength | 全体的なエンハンスメント強度を制御します。 |
| Sample Rate | 出力のターゲットサンプリングレートを設定します。 |
| Stereo Width | ステレオイメージの広がりを調整します。 |
| Dynamic | 動的応答の挙動を制御します。 |
| DSP Context | ストリーミングチャンク周辺で使用するオーバーラップ / コンテキスト量を設定します。 |
| Stream Chunk | ストリーミングチャンク長を制御します。 |
| GC Interval | 長時間処理時のガベージコレクション間隔を制御します。 |
| Output Format | ALAC、FLAC、MP3の出力形式を選択します。 |

推奨設定は、入力ファイルや端末性能によって変わる場合があります。

---

## 出力形式

### ALAC / M4A

Apple互換のロスレス出力が必要な場合におすすめです。

### FLAC

オープンなロスレス出力が必要な場合におすすめです。

### MP3

ロスレス品質よりも互換性やファイルサイズを優先する場合におすすめです。

MP3出力では `libmp3lame` を使用します。MP3変換に失敗する場合は以下を確認してください。

- FFmpegが `--enable-libmp3lame` 付きでビルドされていること。
- 各対象ABI向けの `libmp3lame.so` がAPKに含まれていること。
- 実行時に `libavcodec.so` から `libmp3lame.so` を参照できること。

---

## カバー画像とメタデータ

DSRE-Mobile は、メタデータと埋め込みカバー画像の保持を試みます。

MP3出力では、プレイヤーごとに対応するID3バージョンや文字エンコーディングが異なるため、メタデータ互換性が問題になる場合があります。このビルドでは、可能な場合にID3v2.3を優先し、ID3v1を避けることを意図しています。

### カバー画像保持に必要なもの

カバー画像保持のため、FFmpegビルドには以下のサポートが必要です。

- MJPEG decoder
- 入力カバーがPNGの場合は PNG decoder
- 入力カバーがBMPの場合は BMP decoder
- 入力カバーがWebPの場合は WebP decoder
- MJPEG encoder
- `libswscale`

MJPEG encoder が不足していると、カバー画像を変換して添付できない場合があります。

### メタデータに関する既知の制限

入力ファイルのメタデータが `ffprobe` の時点ですでに文字化けしている場合、DSRE-Mobileは元のタグ文字列を確実に復元できません。その場合は、処理前にタグ編集ソフトなどで元ファイルのタグを修正してください。

確認例:

```bash
ffprobe -hide_banner -show_format input.mp3
```

ここで title、artist、album などがすでに文字化けしている場合、DSRE-Mobileが受け取る前の段階で入力メタデータが正しくデコードされていません。

---

## ネイティブライブラリ確認リスト

アプリは起動するのに処理が始まらない場合は、ネイティブ依存関係を確認してください。

### APK内容の確認

```bash
unzip -l app.apk | grep '\.so'
```

期待される配置例:

```text
lib/arm64-v8a/libdsre_audio.so
lib/arm64-v8a/libavcodec.so
lib/arm64-v8a/libavformat.so
lib/arm64-v8a/libavutil.so
lib/arm64-v8a/libswresample.so
lib/arm64-v8a/libswscale.so
lib/arm64-v8a/libmp3lame.so
```

または:

```text
lib/armeabi-v7a/libdsre_audio.so
lib/armeabi-v7a/libavcodec.so
lib/armeabi-v7a/libavformat.so
lib/armeabi-v7a/libavutil.so
lib/armeabi-v7a/libswresample.so
lib/armeabi-v7a/libswscale.so
lib/armeabi-v7a/libmp3lame.so
```

### 依存ライブラリの確認

```bash
llvm-readelf -d libdsre_audio.so | grep NEEDED
llvm-readelf -d libavcodec.so | grep NEEDED
```

`NEEDED` に `libmp3lame.so` が表示される場合、対象ABIに一致する `libmp3lame.so` をAPKへ同梱する必要があります。

---

## ビルドノート

このプロジェクトはAndroidネイティブ共有ライブラリを使用します。複数ABI向けにビルドする場合は、ABIごとにビルドし、ABIごとにライブラリをパッケージしてください。

ABIフォルダ例:

```text
native_libs/arm64-v8a/
native_libs/armeabi-v7a/
```

Buildozer設定例:

```ini
android.add_libs = native_libs/*/*.so
android.archs = arm64-v8a, armeabi-v7a
```

### FFmpegライセンス指向のビルドノート

公開リリースビルドでは、DSRE-Mobile は LGPL-oriented なFFmpeg構成を使用することを意図しています。

FFmpegビルドでは、以下のようなGPL / nonfree系オプションを避けるべきです。

```text
--enable-gpl
--enable-version3
--enable-nonfree
```

一般的なリリースビルドでは、動的共有ライブラリを使用します。

```text
--enable-shared
--disable-static
--disable-programs
```

現在の音声処理およびカバー画像処理機能は、GPL専用のFFmpegコンポーネントを有効化しなくても動作することを意図しています。

---

## ライセンス

このリポジトリ内のDSRE-Mobileソースコードは、特に明記がない限りMIT Licenseで提供されます。

配布されるAndroid APKには、FFmpegやLAME/libmp3lameなどの第三者ネイティブライブラリが含まれる場合があります。これらのコンポーネントはMIT Licenseではなく、それぞれのライセンスに従います。

- FFmpegはデフォルトではLGPL v2.1 or laterでライセンスされています。ただし、`--enable-gpl` などのconfigureオプションにより、実効ライセンスが変わる場合があります。
- リリースビルドでは、GPL / nonfree系のFFmpeg configureオプションを避け、LGPL-orientedなFFmpeg構成を使用することを意図しています。
- LAME/libmp3lameはLGPLでライセンスされています。
- ユーザーおよび再配布者は、同梱されるすべての第三者コンポーネントのライセンスに従う責任があります。

詳細は `THIRD_PARTY_NOTICES.md` を参照してください。

---

## トラブルシューティング

### MP3変換に失敗する

以下を確認してください。

- `libmp3lame` がAndroid向けにクロスコンパイルされていること。
- FFmpegが `--enable-libmp3lame` 付きで構成されていること。
- `libmp3lame.so` がAPKに含まれていること。
- ABIが端末と一致していること。

### カバー画像が消える

FFmpegビルドに以下が含まれているか確認してください。

- MJPEG decoder
- 入力カバーがPNGの場合は PNG decoder
- MJPEG encoder
- `libswscale`

MJPEG encoder が不足していると、カバー画像を変換して添付できない場合があります。

### MP3出力のタグが文字化けする

まず入力ファイルを確認してください。

```bash
ffprobe -hide_banner -show_format input.mp3
```

入力ファイルの時点ですでに `ffprobe` 上で文字化けしている場合は、処理前にタグ編集ソフトなどで元ファイルのタグを修正してください。

### 処理が開始されない

以下を確認してください。

- すべてのファイルへのアクセスが有効になっていること。
- 入力ファイルリストが空ではないこと。
- ネイティブ `.so` 依存関係がAPKに含まれていること。
- `libdsre_audio.so` が必要なストリーミング用シンボルをexportしていること。
- `adb logcat` で `dlopen failed`、`UnsatisfiedLinkError`、`cannot locate symbol` などが出ていないこと。

---

## 免責事項

DSRE-Mobile は実験的なオーディオエンハンスメントアプリケーションです。知覚される効果は、入力音源、選択したパラメータ、再生機器、聴取環境、ユーザーの好みによって変わります。

このプロジェクトは、失われた音声情報を完全に復元することを主張するものではありません。Android上でのローカル実験および実用的な音声アップコンバートワークフローのために、調整可能なエンハンスメントパイプラインを提供します。

---

## クレジット

- Original DSRE concept: [DSRE / Digital Sound Resolution Enhancer English](https://github.com/Urabewe/DSRE---Digital-Sound-Resolution-Enhancer-English)
- Android port: [DSRE-Mobile](https://github.com/CrossDarkrix/DSRE-Mobile)
- FFmpeg: [FFmpeg project](https://ffmpeg.org/)
- LAME MP3 encoder: [LAME project](https://lame.sourceforge.io/)
