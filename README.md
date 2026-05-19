<div align="center">
	<a href="https://github.com/CrossDarkrix/DSRE-Mobile">
	<img width="150px" height="150px" alt="DSRE-Mobile" src="https://raw.githubusercontent.com/CrossDarkrix/DSRE-Mobile/refs/heads/main/icon.png"></a>
</div>


# [DSRE / Deep Sound Resolution Enhancer](https://github.com/Urabewe/DSRE---Digital-Sound-Resolution-Enhancer-English)のAndroid版

## 説明 / Description

DSREは、あらゆるオーディオファイルをバッチ処理で高解像度（ハイレゾ）オーディオに変換できる高性能オーディオエンハンスメントツールです。大量のオーディオファイルを、大きな計算リソースを必要とせずに高速に処理できます。

DSREは、あらゆるオーディオファイルをバッチ処理で高解像度（ハイレゾ）オーディオに変換できる高性能オーディオエンハンスメントツールです。Sony DSEE HXに着想を得て開発されたDSREは、ディープラーニングを使用しない周波数強調アルゴリズムを採用することで、計算負荷を抑えながら大量のファイルを高速に処理できます。

**主な特長:**

* **バッチ処理:** 複数のオーディオファイルを一度に変換できます。

* **複数のフォーマットに対応:** WAV、MP3、FLAC、M4Aなどに対応しています。

* **カバー画像とメタデータを保持:** 手動編集は不要です。

* **柔軟なパラメータ設定:** モジュレーションカウント、ディケイ、ハイパスフィルターなどを調整できます。

* **高速かつ安定:** ディープラーニングモデルに依存しない処理です。

## スクリーンショット

<img src="https://raw.githubusercontent.com/CrossDarkrix/DSRE-Mobile/refs/heads/main/image/screenshot1.jpg" align="center" width="150px" height="300px" alt="android1">
<img src="https://raw.githubusercontent.com/CrossDarkrix/DSRE-Mobile/refs/heads/main/image/screenshot2.jpg" align="center" width="150px" height="300px" alt="android2">


## インストールと使用 / Installation & Usage

[ダウンロード / Download](https://github.com/CrossDarkrix/DSRE-Mobile/releases)


## パラメータの説明 / Parameters

| パラメータ | デフォルト | 説明 |

| -------------------------------------------- | ------------- | ------------------------------------------------------------------ |

| 変調回数 (m) | 8 | 強調処理の繰り返し回数。数値が大きいほど詳細度が高くなります。 |

| ディケイ | 1.25 | 高周波減衰制御 |

| 前処理ハイパスカットオフ | 3000 Hz | 強調処理前のハイパスフィルター |

| 後処理ハイパスカットオフ | 16000 Hz | 強調処理後のハイパスフィルター |

| フィルター次数 | 11 | ハイパスフィルター次数 |

| 目標サンプリングレート | 48000 Hz | 出力オーディオのサンプリングレート |

| 出力フォーマット | ALAC / FLAC | ハイレゾ出力フォーマットを選択                     |

| --------------------------------------------------------------------------------------------------------------------------------------

大元のリポジトリ: [DSER-English](https://github.com/Urabewe/DSRE---Digital-Sound-Resolution-Enhancer-English)