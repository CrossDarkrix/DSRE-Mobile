import gc
import json
import os
import re
import shutil
import sys
import threading
import tempfile
import time
import traceback
from ctypes import CDLL, POINTER, byref, c_char_p, c_float, c_int, c_void_p
from typing import Any, Dict, List, Optional, Tuple

import numpy as np
from kivy.app import App
from kivy.clock import Clock
from kivy.core.window import Window
from kivy.graphics import Color, RoundedRectangle, Rectangle
from kivy.metrics import dp
from kivy.properties import BooleanProperty, ListProperty
from kivy.uix.boxlayout import BoxLayout
from kivy.uix.button import Button
from kivy.uix.filechooser import FileChooserListView
from kivy.uix.label import Label
from kivy.uix.modalview import ModalView
from kivy.uix.progressbar import ProgressBar
from kivy.uix.scrollview import ScrollView
from kivy.uix.spinner import Spinner
from kivy.uix.textinput import TextInput

APP_NAME = "DSRE Kivy Mobile CDLL v1.7-streaming"
APP_BASE_DIR = os.path.dirname(os.path.abspath(__file__))
EXTERNAL_STORAGE = os.getenv("EXTERNAL_STORAGE") or os.path.expanduser("~")
DSRE_DOCUMENT_DIR = os.path.join(EXTERNAL_STORAGE, "Documents", "DSRE")
os.makedirs(DSRE_DOCUMENT_DIR, exist_ok=True)
CONFIG_FILE = os.path.join(DSRE_DOCUMENT_DIR, "dsre_kivy_config.json")
FFLOG_FILE = os.path.join(DSRE_DOCUMENT_DIR, "fflog.txt")


def write_fflog(
    title: str,
    message: str = "",
    exc: Optional[BaseException] = None,
    extra: Optional[Dict[str, Any]] = None,
) -> None:
    """Append diagnostics to Documents/DSRE/fflog.txt without crashing the app."""
    try:
        os.makedirs(os.path.dirname(FFLOG_FILE), exist_ok=True)
        with open(FFLOG_FILE, "a", encoding="utf-8") as f:
            f.write("\n" + "=" * 80 + "\n")
            f.write(time.strftime("%Y-%m-%d %H:%M:%S") + "\n")
            f.write(str(title) + "\n")
            if message:
                f.write(str(message) + "\n")
            if extra:
                try:
                    f.write(json.dumps(extra, ensure_ascii=False, indent=2, default=str) + "\n")
                except Exception:
                    f.write(str(extra) + "\n")
            if exc is not None:
                f.write("--- exception ---\n")
                f.write("".join(traceback.format_exception(type(exc), exc, exc.__traceback__)))
            f.flush()
    except Exception:
        pass


def force_release_memory() -> None:
    """Best-effort cleanup for Python, NumPy and native heap pressure."""
    try:
        gc.collect()
    except Exception:
        pass
    try:
        libc = CDLL("libc.so")
        malloc_trim = getattr(libc, "malloc_trim", None)
        if malloc_trim:
            malloc_trim(0)
    except Exception:
        pass

AUDIO_EXTENSIONS = {
    ".wav",
    ".mp3",
    ".m4a",
    ".flac",
    ".ogg",
    ".aiff",
    ".aif",
    ".aac",
    ".wma",
    ".mka",
}

class DSRENativeAudio:
    """ctypes.CDLL wrapper for libdsre_audio.so.

    libdsre_audio.so は Android APK の native library として同梱してください。
    Buildozer では android.add_libs_arm64_v8a などで配置する想定です。

    C側PCMレイアウト:
      - decode output: interleaved float32, samples x channels
      - encode input : interleaved float32, samples x channels

    Python/DSRE内部レイアウト:
      - channels-first float32, channels x samples
    """

    def __init__(self, lib_path: Optional[str] = None):
        self.lib_path = lib_path or self._find_library_path()
        self.lib = CDLL(self.lib_path)
        self._bind_functions()

    def _find_library_path(self) -> str:
        candidates = [
            "libdsre_audio.so",
            os.path.join(APP_BASE_DIR, "libdsre_audio.so"),
            os.path.join(os.getcwd(), "libdsre_audio.so"),
            os.path.join(APP_BASE_DIR, "native_libs", "libdsre_audio.so"),
        ]
        errors = []
        for candidate in candidates:
            try:
                CDLL(candidate)
                return candidate
            except OSError as exc:
                errors.append(f"{candidate}: {exc}")
        raise RuntimeError(
            "libdsre_audio.so をロードできません。Buildozer の android.add_libs_* で "
            "libdsre_audio.so と FFmpeg 依存 .so を同梱してください。\n" + "\n".join(errors)
        )

    def _bind_functions(self):
        self.lib.dsre_decode_to_f32.argtypes = [
            c_char_p,
            c_int,
            POINTER(POINTER(c_float)),
            POINTER(c_int),
            POINTER(c_int),
        ]
        self.lib.dsre_decode_to_f32.restype = c_int

        self.lib.dsre_encode_from_f32.argtypes = [
            c_char_p,
            POINTER(c_float),
            c_int,
            c_int,
            c_int,
            c_char_p,
            c_char_p,
        ]
        self.lib.dsre_encode_from_f32.restype = c_int

        self.lib.dsre_free.argtypes = [c_void_p]
        self.lib.dsre_free.restype = None

        self.lib.dsre_last_error.argtypes = []
        self.lib.dsre_last_error.restype = c_char_p

        self.streaming_available = False
        self._bind_streaming_functions()

    def _bind_streaming_functions(self):
        """Bind optional streaming ABI. If the .so is old, keep legacy mode usable."""
        try:
            self.lib.dsre_decoder_open.argtypes = [
                c_char_p,
                c_int,
                c_int,
                POINTER(c_void_p),
                POINTER(c_int),
                POINTER(c_int),
            ]
            self.lib.dsre_decoder_open.restype = c_int

            self.lib.dsre_decoder_read_f32.argtypes = [
                c_void_p,
                POINTER(c_float),
                c_int,
                POINTER(c_int),
                POINTER(c_int),
            ]
            self.lib.dsre_decoder_read_f32.restype = c_int

            self.lib.dsre_decoder_close.argtypes = [c_void_p]
            self.lib.dsre_decoder_close.restype = None

            self.lib.dsre_encoder_open.argtypes = [
                c_char_p,
                c_char_p,
                c_char_p,
                c_int,
                c_int,
                POINTER(c_void_p),
            ]
            self.lib.dsre_encoder_open.restype = c_int

            self.lib.dsre_encoder_write_f32.argtypes = [
                c_void_p,
                POINTER(c_float),
                c_int,
            ]
            self.lib.dsre_encoder_write_f32.restype = c_int

            self.lib.dsre_encoder_close.argtypes = [c_void_p]
            self.lib.dsre_encoder_close.restype = c_int

            self.lib.dsre_encoder_abort.argtypes = [c_void_p]
            self.lib.dsre_encoder_abort.restype = None

            self.streaming_available = True
        except AttributeError:
            self.streaming_available = False

    def last_error(self) -> str:
        value = self.lib.dsre_last_error()
        return value.decode("utf-8", errors="replace") if value else ""

    def decode(self, input_path: str, target_sr: int) -> Tuple[np.ndarray, int]:
        pcm_ptr = POINTER(c_float)()
        channels = c_int()
        samples = c_int()

        ret = self.lib.dsre_decode_to_f32(
            os.fsencode(input_path),
            int(target_sr),
            byref(pcm_ptr),
            byref(channels),
            byref(samples),
        )
        if ret != 0:
            raise RuntimeError(f"dsre_decode_to_f32 failed: {ret}: {self.last_error()}")

        try:
            ch = int(channels.value)
            smp = int(samples.value)
            if ch <= 0 or smp <= 0:
                raise RuntimeError(f"Invalid decoded shape: channels={ch}, samples={smp}")
            total = ch * smp
            interleaved = np.ctypeslib.as_array(pcm_ptr, shape=(total,))
            y = interleaved.copy().reshape(smp, ch).T
            return sanitize_audio(y), int(target_sr)
        finally:
            self.lib.dsre_free(pcm_ptr)

    def encode(
        self,
        original_path: str,
        pcm_ch_first: np.ndarray,
        sr: int,
        output_path: str,
        fmt: str,
    ) -> str:
        pcm = ensure_ch_first(sanitize_audio(pcm_ch_first)).astype(np.float32, copy=False)
        channels, samples = pcm.shape
        interleaved = np.ascontiguousarray(pcm.T, dtype=np.float32)

        ret = self.lib.dsre_encode_from_f32(
            os.fsencode(original_path or ""),
            interleaved.ctypes.data_as(POINTER(c_float)),
            int(channels),
            int(samples),
            int(sr),
            os.fsencode(output_path),
            str(fmt).upper().encode("utf-8"),
        )
        if ret != 0:
            raise RuntimeError(f"dsre_encode_from_f32 failed: {ret}: {self.last_error()}")
        return output_path

    def decoder_open(self, input_path: str, target_sr: int, preferred_chunk_samples: int) -> Tuple[c_void_p, int, int]:
        if not self.streaming_available:
            raise RuntimeError("libdsre_audio.so does not expose streaming decoder API")
        handle = c_void_p()
        out_sr = c_int()
        out_channels = c_int()
        ret = self.lib.dsre_decoder_open(
            os.fsencode(input_path),
            int(target_sr),
            int(preferred_chunk_samples),
            byref(handle),
            byref(out_sr),
            byref(out_channels),
        )
        if ret != 0:
            raise RuntimeError(f"dsre_decoder_open failed: {ret}: {self.last_error()}")
        return handle, int(out_sr.value), int(out_channels.value)

    def decoder_read(self, handle: c_void_p, buffer: np.ndarray, max_samples: int) -> Tuple[int, bool]:
        if not self.streaming_available:
            raise RuntimeError("libdsre_audio.so does not expose streaming decoder API")
        if buffer.dtype != np.float32 or not buffer.flags["C_CONTIGUOUS"]:
            raise ValueError("decoder_read buffer must be C-contiguous float32")
        out_samples = c_int()
        out_eof = c_int()
        ret = self.lib.dsre_decoder_read_f32(
            handle,
            buffer.ctypes.data_as(POINTER(c_float)),
            int(max_samples),
            byref(out_samples),
            byref(out_eof),
        )
        if ret != 0:
            raise RuntimeError(f"dsre_decoder_read_f32 failed: {ret}: {self.last_error()}")
        return int(out_samples.value), bool(out_eof.value)

    def decoder_close(self, handle: Optional[c_void_p]) -> None:
        if handle and self.streaming_available:
            self.lib.dsre_decoder_close(handle)

    def encoder_open(
        self,
        original_path: str,
        output_path: str,
        fmt: str,
        sr: int,
        channels: int,
    ) -> c_void_p:
        if not self.streaming_available:
            raise RuntimeError("libdsre_audio.so does not expose streaming encoder API")
        handle = c_void_p()
        ret = self.lib.dsre_encoder_open(
            os.fsencode(original_path or ""),
            os.fsencode(output_path),
            str(fmt).upper().encode("utf-8"),
            int(sr),
            int(channels),
            byref(handle),
        )
        if ret != 0:
            raise RuntimeError(f"dsre_encoder_open failed: {ret}: {self.last_error()}")
        return handle

    def encoder_write(self, handle: c_void_p, pcm_interleaved: np.ndarray) -> None:
        if not self.streaming_available:
            raise RuntimeError("libdsre_audio.so does not expose streaming encoder API")
        pcm = np.ascontiguousarray(pcm_interleaved, dtype=np.float32)
        if pcm.ndim != 2:
            raise ValueError("encoder_write expects shape=(samples, channels)")
        samples = int(pcm.shape[0])
        if samples <= 0:
            return
        ret = self.lib.dsre_encoder_write_f32(
            handle,
            pcm.ctypes.data_as(POINTER(c_float)),
            samples,
        )
        if ret != 0:
            raise RuntimeError(f"dsre_encoder_write_f32 failed: {ret}: {self.last_error()}")

    def encoder_close(self, handle: Optional[c_void_p]) -> None:
        if handle and self.streaming_available:
            ret = self.lib.dsre_encoder_close(handle)
            if ret != 0:
                raise RuntimeError(f"dsre_encoder_close failed: {ret}: {self.last_error()}")

    def encoder_abort(self, handle: Optional[c_void_p]) -> None:
        if handle and self.streaming_available:
            self.lib.dsre_encoder_abort(handle)


_NATIVE_AUDIO: Optional[DSRENativeAudio] = None


def get_native_audio() -> DSRENativeAudio:
    global _NATIVE_AUDIO
    if _NATIVE_AUDIO is None:
        _NATIVE_AUDIO = DSRENativeAudio()
    return _NATIVE_AUDIO


def ffprobe_audio_info(file_path: str) -> Dict[str, Any]:
    """subprocess/ffprobe は使わないため、詳細情報は取得しない軽量スタブです。

    DSREProcessor のログ互換用にキーだけ返します。実際の decode は
    libdsre_audio.so 側の dsre_decode_to_f32 が行います。
    """
    return {
        "sample_rate": 0,
        "channels": 0,
        "duration": 0.0,
        "codec_name": "native",
        "bit_rate": 0,
    }


def load_audio_ffmpeg(file_path: str, target_sr: int) -> Tuple[np.ndarray, int]:
    """CDLL/native wrapper based loader. No subprocess is used."""
    if target_sr <= 0:
        raise ValueError(f"Invalid target sample rate: {target_sr}")
    return get_native_audio().decode(file_path, target_sr)


def extract_cover_image(in_path: str) -> Optional[str]:
    """CDLL版ではPython側での画像抽出は行いません。

    カバーアートの出力ファイルへの引き継ぎは、
    libdsre_audio.so 側の dsre_encode_from_f32() 内で best-effort に実行します。
    この関数は旧FFmpeg CLI版との互換用スタブです。
    """

    return None


def save_with_metadata(
    in_path: str,
    y_out: np.ndarray,
    sr: int,
    out_path: str,
    fmt: str = "ALAC",
    normalize: bool = True,
) -> str:
    """CDLL/native wrapper based saver. No subprocess is used."""
    if not os.path.exists(in_path):
        raise FileNotFoundError(f"Input file not found: {in_path}")
    if y_out is None or y_out.size == 0:
        raise ValueError("Empty audio data provided")
    if sr <= 0:
        raise ValueError(f"Invalid sample rate: {sr}")

    fmt = str(fmt).upper()
    ext_map = {"ALAC": "m4a", "FLAC": "flac", "MP3": "mp3"}
    if fmt not in ext_map:
        raise ValueError(f"Unsupported format: {fmt}")

    data = sanitize_audio(y_out)
    if normalize:
        peak = audio_peak(data)
        if peak > 1.0:
            data = data / peak
    else:
        data = np.clip(data, -1.0, 1.0).astype(np.float32, copy=False)

    if audio_peak(data) < 1e-10:
        raise ValueError("Audio data is essentially silent - cannot save")

    out_path = os.path.splitext(out_path)[0] + "." + ext_map[fmt]
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    result_path = get_native_audio().encode(in_path, data, sr, out_path, fmt)

    if not os.path.exists(result_path) or os.path.getsize(result_path) < 1000:
        raise RuntimeError(f"Output file was not created correctly: {result_path}")
    return result_path

def is_audio_file(path: str) -> bool:
    return (
        os.path.isfile(path)
        and os.path.splitext(path.lower())[1] in AUDIO_EXTENSIONS
    )


def collect_audio_files_from_directory(
    directory: str,
    recursive: bool = True,
) -> List[str]:
    found: List[str] = []

    if not os.path.isdir(directory):
        return found

    if recursive:
        for root, _, files in os.walk(directory):
            for name in files:
                path = os.path.join(root, name)
                if is_audio_file(path):
                    found.append(os.path.abspath(path))
    else:
        for name in os.listdir(directory):
            path = os.path.join(directory, name)
            if is_audio_file(path):
                found.append(os.path.abspath(path))

    return sorted(found)


def ensure_ch_first(y: np.ndarray) -> np.ndarray:
    y = np.asarray(y)

    if y.ndim == 1:
        return y[np.newaxis, :]

    if y.ndim == 2:
        if y.shape[0] > y.shape[1]:
            return y.T
        return y

    raise ValueError(f"Unsupported audio shape: {y.shape}")


def ensure_sf_shape(y: np.ndarray) -> np.ndarray:
    y = np.asarray(y)

    if y.ndim == 1:
        return y[:, None]

    if y.ndim == 2:
        if y.shape[0] <= y.shape[1]:
            return y.T
        return y

    raise ValueError(f"Unsupported audio shape: {y.shape}")


def sanitize_audio(
    x: Optional[np.ndarray],
    fallback: Optional[np.ndarray] = None,
) -> np.ndarray:
    if x is None:
        if fallback is not None:
            return fallback.copy().astype(np.float32)
        raise ValueError("Audio data is None")

    x = np.asarray(x)

    if x.size == 0:
        if fallback is not None:
            return fallback.copy().astype(np.float32)
        raise ValueError("Audio data is empty")

    x = np.nan_to_num(
        x,
        nan=0.0,
        posinf=0.0,
        neginf=0.0,
    ).astype(np.float32, copy=False)

    peak = float(np.max(np.abs(x))) if x.size else 0.0
    if peak > 1000.0:
        x = x / peak

    return x


def audio_peak(x: np.ndarray) -> float:
    if x is None or x.size == 0:
        return 0.0
    return float(np.max(np.abs(x)))


def audio_rms(x: np.ndarray) -> float:
    if x is None or x.size == 0:
        return 0.0
    return float(np.sqrt(np.mean(np.square(np.asarray(x, dtype=np.float64)))))


def apply_iir_filter(b, a, x):
    x = np.asarray(x, dtype=np.float64)
    b = np.asarray(b, dtype=np.float64)
    a = np.asarray(a, dtype=np.float64)

    if x.ndim != 1:
        raise ValueError("apply_iir_filter expects 1D array")

    if len(a) == 0 or a[0] == 0:
        raise ValueError("Invalid IIR coefficients")

    b = b / a[0]
    a = a / a[0]

    y = np.zeros_like(x, dtype=np.float64)

    nb = len(b)
    na = len(a)

    for n in range(len(x)):
        acc = 0.0

        for i in range(nb):
            if n - i >= 0:
                acc += b[i] * x[n - i]

        for i in range(1, na):
            if n - i >= 0:
                acc -= a[i] * y[n - i]

        y[n] = acc

    return y.astype(np.float32)


def filtfilt_np(b, a, x):
    x = np.asarray(x, dtype=np.float32)

    if len(x) < max(len(a), len(b)) * 3:
        return apply_iir_filter(b, a, x)

    pad = min(len(x) - 1, max(len(a), len(b)) * 3)

    front = 2 * x[0] - x[1 : pad + 1][::-1]
    back = 2 * x[-1] - x[-pad - 1 : -1][::-1]

    xp = np.concatenate([front, x, back])

    y = apply_iir_filter(b, a, xp)
    y = apply_iir_filter(b, a, y[::-1])[::-1]

    return y[pad : pad + len(x)].astype(np.float32)


def design_peaking_eq(freq, gain_db, q, sr):
    A = 10 ** (gain_db / 40.0)
    w0 = 2 * np.pi * freq / sr
    alpha = np.sin(w0) / (2 * q)
    cos_w0 = np.cos(w0)

    b0 = 1 + alpha * A
    b1 = -2 * cos_w0
    b2 = 1 - alpha * A

    a0 = 1 + alpha / A
    a1 = -2 * cos_w0
    a2 = 1 - alpha / A

    return (
        np.array([b0, b1, b2], dtype=np.float64),
        np.array([a0, a1, a2], dtype=np.float64),
    )


def bandpass_fft(
    x,
    sr,
    low_hz,
    high_hz,
    transition_ratio: float = 0.15,
):
    x = np.asarray(x, dtype=np.float64)
    n = len(x)

    if n == 0:
        return x.astype(np.float32)

    low_hz = max(1.0, float(low_hz))
    nyquist = sr / 2.0
    high_hz = min(float(high_hz), nyquist - 1.0)

    if low_hz >= high_hz:
        return np.zeros_like(x, dtype=np.float32)

    X = np.fft.rfft(x)
    freqs = np.fft.rfftfreq(n, d=1.0 / sr)

    mask = np.zeros_like(freqs, dtype=np.float64)

    bw = max(20.0, (high_hz - low_hz) * transition_ratio)

    low1 = max(0.0, low_hz - bw)
    low2 = low_hz
    high1 = high_hz
    high2 = min(nyquist, high_hz + bw)

    rising = (freqs >= low1) & (freqs < low2)
    if np.any(rising):
        t = (freqs[rising] - low1) / max(1e-12, low2 - low1)
        mask[rising] = 0.5 - 0.5 * np.cos(np.pi * t)

    passband = (freqs >= low2) & (freqs <= high1)
    mask[passband] = 1.0

    falling = (freqs > high1) & (freqs <= high2)
    if np.any(falling):
        t = (freqs[falling] - high1) / max(1e-12, high2 - high1)
        mask[falling] = 0.5 + 0.5 * np.cos(np.pi * t)

    y = np.fft.irfft(X * mask, n=n)

    return y.astype(np.float32)


def generate_harmonics(
    signal_band,
    fundamental_freq,
    sr,
    num_harmonics: int = 5,
    harmonic_strength: float = 0.3,
):
    signal_band = sanitize_audio(signal_band)

    if len(signal_band) == 0:
        return signal_band

    enhanced = signal_band.astype(np.float32).copy()

    for h in range(2, num_harmonics + 2):
        harmonic_freq = fundamental_freq * h

        if harmonic_freq < sr / 2:
            phase_increment = 2 * np.pi * harmonic_freq / sr

            if not np.isfinite(phase_increment):
                continue

            harmonic_oscillator = np.sin(
                phase_increment * np.arange(len(signal_band), dtype=np.float64)
            ).astype(np.float32)

            if not np.all(np.isfinite(harmonic_oscillator)):
                continue

            harmonic_content = signal_band * harmonic_oscillator * (
                harmonic_strength / h
            )

            if not np.all(np.isfinite(harmonic_content)):
                continue

            enhanced += harmonic_content

    return sanitize_audio(enhanced, fallback=signal_band)


def multiband_exciter(
    x,
    sr,
    harmonic_intensity: float = 0.6,
    progress_cb=None,
    abort_cb=None,
):
    x = ensure_ch_first(x).astype(np.float32, copy=False)

    enhanced = np.zeros_like(x, dtype=np.float32)
    nyquist = sr // 2
    base_strength_scale = float(np.clip(harmonic_intensity, 0.1, 1.5))

    band_definitions = [
        {
            "name": "Sub Bass",
            "low": 20,
            "high": 80,
            "gain": 1.10,
            "harmonics": 3,
            "strength": 0.08,
        },
        {
            "name": "Bass",
            "low": 80,
            "high": 250,
            "gain": 1.20,
            "harmonics": 4,
            "strength": 0.14,
        },
        {
            "name": "Low Mid",
            "low": 250,
            "high": 800,
            "gain": 1.25,
            "harmonics": 5,
            "strength": 0.18,
        },
        {
            "name": "Mid",
            "low": 800,
            "high": 2500,
            "gain": 1.35,
            "harmonics": 6,
            "strength": 0.22,
        },
        {
            "name": "High Mid",
            "low": 2500,
            "high": 8000,
            "gain": 1.45,
            "harmonics": 4,
            "strength": 0.25,
        },
        {
            "name": "Presence",
            "low": 8000,
            "high": 16000,
            "gain": 1.50,
            "harmonics": 3,
            "strength": 0.18,
        },
        {
            "name": "Air",
            "low": 16000,
            "high": min(20000, nyquist - 1000),
            "gain": 1.35,
            "harmonics": 2,
            "strength": 0.12,
        },
    ]

    bands = []
    for band in band_definitions:
        if (
            band["low"] < nyquist
            and band["high"] < nyquist
            and band["high"] > band["low"]
        ):
            bands.append(band)

    if not bands:
        return x.astype(np.float32)

    total_steps = max(1, len(bands) * x.shape[0])

    for ch in range(x.shape[0]):
        if abort_cb and abort_cb():
            break

        channel_enhanced = x[ch].copy()

        for i, band in enumerate(bands):
            if abort_cb and abort_cb():
                break

            if progress_cb:
                progress = int((i + ch * len(bands)) * 100 / total_steps)
                progress_cb(progress, f"Processing band {band['name']}")

            try:
                band_signal = bandpass_fft(
                    x[ch],
                    sr,
                    band["low"],
                    band["high"],
                )

                band_signal = sanitize_audio(
                    band_signal,
                    fallback=np.zeros_like(x[ch], dtype=np.float32),
                )

                if audio_peak(band_signal) < 1e-8:
                    continue

                center_freq = (band["low"] + band["high"]) / 2
                strength = band["strength"] * base_strength_scale

                harmonics_added = generate_harmonics(
                    band_signal,
                    center_freq,
                    sr,
                    band["harmonics"],
                    strength,
                )

                saturated = np.tanh(harmonics_added * 1.35).astype(np.float32) * 0.82
                band_enhanced = saturated * band["gain"]

                if not np.all(np.isfinite(band_enhanced)):
                    continue

                channel_enhanced = channel_enhanced + band_enhanced * 0.22

            except Exception:
                continue

        enhanced[ch] = sanitize_audio(channel_enhanced, fallback=x[ch])

    return enhanced


def psychoacoustic_enhancer(
    x,
    sr,
    strength: float = 1.0,
    progress_cb=None,
    abort_cb=None,
):
    x = ensure_ch_first(x).astype(np.float32, copy=False)
    enhanced = np.zeros_like(x, dtype=np.float32)
    scale = float(np.clip(strength, 0.3, 1.5))

    critical_bands = [
        {"freq": 1000, "boost": 1.0 * scale, "q": 1.5},
        {"freq": 2500, "boost": 1.8 * scale, "q": 2.0},
        {"freq": 4000, "boost": 2.0 * scale, "q": 1.8},
        {"freq": 6000, "boost": 1.5 * scale, "q": 1.2},
        {"freq": 10000, "boost": 1.0 * scale, "q": 0.8},
    ]

    total_steps = max(1, len(critical_bands) * x.shape[0])

    for ch in range(x.shape[0]):
        if abort_cb and abort_cb():
            break

        channel_enhanced = x[ch].copy()

        for i, band in enumerate(critical_bands):
            if abort_cb and abort_cb():
                break

            if progress_cb:
                progress = int((i + ch * len(critical_bands)) * 100 / total_steps)
                progress_cb(progress, f"Psychoacoustic enhancement at {band['freq']}Hz")

            if band["freq"] >= sr // 2:
                continue

            try:
                b, a = design_peaking_eq(
                    freq=band["freq"],
                    gain_db=band["boost"],
                    q=band["q"],
                    sr=sr,
                )

                filtered = filtfilt_np(b, a, x[ch])
                filtered = sanitize_audio(filtered, fallback=x[ch])

                blend_factor = 0.22
                channel_enhanced = (
                    channel_enhanced * (1.0 - blend_factor)
                    + filtered * blend_factor
                )

            except Exception:
                continue

        enhanced[ch] = sanitize_audio(channel_enhanced, fallback=x[ch])

    return enhanced


def stereo_width_enhancer(x, width_factor: float = 1.15):
    x = ensure_ch_first(x).astype(np.float32, copy=False)

    if x.shape[0] != 2:
        return x

    left, right = x[0], x[1]

    mid = (left + right) / 2.0
    side = (left - right) / 2.0

    side_enhanced = side * float(np.clip(width_factor, 1.0, 1.8))

    return np.array(
        [
            mid + side_enhanced,
            mid - side_enhanced,
        ],
        dtype=np.float32,
    )


def dynamic_range_enhancer(
    x,
    ratio: float = 1.12,
    attack_ms: float = 5,
    release_ms: float = 50,
    sr: int = 44100,
):
    x = ensure_ch_first(x).astype(np.float32, copy=False)

    attack_samples = max(1, int(attack_ms * sr / 1000))
    release_samples = max(1, int(release_ms * sr / 1000))

    enhanced = np.zeros_like(x, dtype=np.float32)

    for ch in range(x.shape[0]):
        signal_ch = x[ch]
        envelope = np.abs(signal_ch)
        smoothed_env = np.zeros_like(envelope)

        if len(envelope) == 0:
            enhanced[ch] = signal_ch
            continue

        current_env = envelope[0]

        for i in range(len(envelope)):
            if envelope[i] > current_env:
                current_env += (envelope[i] - current_env) / attack_samples
            else:
                current_env -= (current_env - envelope[i]) / release_samples

            smoothed_env[i] = current_env

        threshold = 0.08
        gain = np.ones_like(smoothed_env)

        above = smoothed_env > threshold

        gain[above] = (smoothed_env[above] / threshold) ** (ratio - 1.0)
        gain = np.clip(gain, 1.0, 1.8)

        enhanced[ch] = signal_ch * gain

    return sanitize_audio(enhanced, fallback=x)


def enhanced_audio_algorithm(
    x: np.ndarray,
    sr: int,
    enhancement_strength: float = 0.7,
    harmonic_intensity: float = 0.6,
    stereo_width: float = 1.15,
    dynamic_enhancement: float = 1.12,
    progress_cb=None,
    abort_cb=None,
) -> np.ndarray:
    x = ensure_ch_first(x)
    x = sanitize_audio(x)

    if x is None or x.size == 0:
        raise ValueError("Input audio data is empty or None")

    if audio_peak(x) < 1e-10:
        raise ValueError("Input audio data appears to be silent")

    if progress_cb:
        progress_cb(0, "Starting enhancement process")

    if progress_cb:
        progress_cb(10, "Applying multi-band harmonic excitement")

    enhanced = multiband_exciter(
        x,
        sr,
        harmonic_intensity=harmonic_intensity,
        progress_cb=lambda p, desc: progress_cb(10 + p // 4, desc)
        if progress_cb
        else None,
        abort_cb=abort_cb,
    )

    enhanced = sanitize_audio(enhanced, fallback=x)

    if abort_cb and abort_cb():
        return x

    use_psycho = False

    if use_psycho:
        if progress_cb:
            progress_cb(35, "Applying psychoacoustic enhancement")

        psycho_enhanced = psychoacoustic_enhancer(
            enhanced,
            sr,
            strength=enhancement_strength,
            progress_cb=lambda p, desc: progress_cb(35 + p // 4, desc)
            if progress_cb
            else None,
            abort_cb=abort_cb,
        )
    else:
        psycho_enhanced = enhanced

    psycho_enhanced = sanitize_audio(psycho_enhanced, fallback=x)

    if abort_cb and abort_cb():
        return x

    if progress_cb:
        progress_cb(60, "Enhancing dynamic range")

    dynamic_enhanced = dynamic_range_enhancer(
        psycho_enhanced,
        ratio=float(np.clip(dynamic_enhancement, 1.0, 1.5)),
        sr=sr,
    )

    dynamic_enhanced = sanitize_audio(dynamic_enhanced, fallback=x)

    if abort_cb and abort_cb():
        return x

    if progress_cb:
        progress_cb(75, "Enhancing stereo width")

    stereo_enhanced = (
        stereo_width_enhancer(dynamic_enhanced, stereo_width)
        if x.shape[0] == 2
        else dynamic_enhanced
    )

    stereo_enhanced = sanitize_audio(stereo_enhanced, fallback=x)

    if abort_cb and abort_cb():
        return x

    if progress_cb:
        progress_cb(90, "Final processing and normalization")

    if audio_peak(stereo_enhanced) < 1e-10:
        final = x.copy()
    else:
        blend_factor = float(np.clip(enhancement_strength, 0.1, 0.8))
        final = x * (1.0 - blend_factor) + stereo_enhanced * blend_factor

    final = sanitize_audio(final, fallback=x)

    peak = audio_peak(final)

    if peak > 0.95:
        final *= 0.95 / peak

    if audio_peak(final) < 1e-10:
        final = x.copy()

    if progress_cb:
        progress_cb(100, "Enhancement complete")

    return sanitize_audio(final, fallback=x)


class DSREStreamingDSP:
    """Stateful streaming DSP for chunked native decode/encode.

    State carried between chunks:
      - input_tail: previous raw samples used as FFT/band context
      - dynamic_env: envelope for dynamic enhancement

    The output length always equals the input chunk length.
    """

    def __init__(
        self,
        sr: int,
        channels: int,
        params: Dict[str, Any],
        context_seconds: float = 0.08,
    ):
        self.sr = int(sr)
        self.channels = int(channels)
        self.params = params
        self.context_samples = max(0, int(self.sr * float(context_seconds)))
        self.input_tail = np.empty((self.channels, 0), dtype=np.float32)
        self.dynamic_env = np.zeros((self.channels,), dtype=np.float32)

    def process(self, chunk: np.ndarray) -> np.ndarray:
        chunk = ensure_ch_first(sanitize_audio(chunk)).astype(np.float32, copy=False)
        if chunk.size == 0 or chunk.shape[1] <= 0:
            return np.empty((self.channels, 0), dtype=np.float32)

        if self.input_tail.size > 0:
            work = np.ascontiguousarray(
                np.concatenate((self.input_tail, chunk), axis=1),
                dtype=np.float32,
            )
            prefix = self.input_tail.shape[1]
        else:
            work = chunk
            prefix = 0

        # Keep FFT/band/exciter context, but only use the valid part for output.
        enhanced_work = multiband_exciter(
            work,
            self.sr,
            harmonic_intensity=float(self.params["m"]) / 16.0,
            progress_cb=None,
            abort_cb=None,
        )
        enhanced_work = sanitize_audio(enhanced_work, fallback=work)
        valid_enhanced = enhanced_work[:, prefix:prefix + chunk.shape[1]]
        if valid_enhanced.shape[1] != chunk.shape[1]:
            valid_enhanced = valid_enhanced[:, :chunk.shape[1]]
            if valid_enhanced.shape[1] < chunk.shape[1]:
                pad = chunk[:, valid_enhanced.shape[1]:]
                valid_enhanced = np.concatenate((valid_enhanced, pad), axis=1)

        dynamic = self._stateful_dynamic_range(
            valid_enhanced,
            ratio=float(np.clip(float(self.params.get("dynamic", 1.12)), 1.0, 1.5)),
        )

        stereo = (
            stereo_width_enhancer(dynamic, float(self.params.get("stereo_width", 1.15)))
            if chunk.shape[0] == 2
            else dynamic
        )
        stereo = sanitize_audio(stereo, fallback=valid_enhanced)

        blend = float(np.clip(float(self.params.get("decay", 0.35)), 0.1, 0.8))
        final = chunk * (1.0 - blend) + stereo * blend
        final = sanitize_audio(final, fallback=chunk).astype(np.float32, copy=False)

        # Avoid per-chunk loudness normalization. Only apply safety scaling.
        peak = audio_peak(final)
        if peak > 0.98:
            final = final * (0.98 / peak)

        if self.context_samples > 0:
            if work.shape[1] > self.context_samples:
                self.input_tail = np.ascontiguousarray(work[:, -self.context_samples:], dtype=np.float32)
            else:
                self.input_tail = np.ascontiguousarray(work, dtype=np.float32)
        else:
            self.input_tail = np.empty((self.channels, 0), dtype=np.float32)

        return np.ascontiguousarray(final, dtype=np.float32)

    def flush(self) -> np.ndarray:
        self.input_tail = np.empty((self.channels, 0), dtype=np.float32)
        return np.empty((self.channels, 0), dtype=np.float32)

    def _stateful_dynamic_range(self, x: np.ndarray, ratio: float = 1.12) -> np.ndarray:
        x = ensure_ch_first(sanitize_audio(x)).astype(np.float32, copy=False)
        if x.size == 0:
            return x

        if self.dynamic_env.shape[0] != x.shape[0]:
            self.dynamic_env = np.zeros((x.shape[0],), dtype=np.float32)

        y = np.empty_like(x, dtype=np.float32)
        attack_ms = 5.0
        release_ms = 50.0
        attack = np.exp(-1.0 / max(1.0, self.sr * attack_ms / 1000.0)).astype(np.float32)
        release = np.exp(-1.0 / max(1.0, self.sr * release_ms / 1000.0)).astype(np.float32)
        amount = float(np.clip(ratio - 1.0, 0.0, 0.5))

        for n in range(x.shape[1]):
            sample_abs = np.abs(x[:, n])
            coeff = np.where(sample_abs > self.dynamic_env, attack, release).astype(np.float32)
            self.dynamic_env = coeff * self.dynamic_env + (1.0 - coeff) * sample_abs
            gain = 1.0 + amount * np.clip(self.dynamic_env, 0.0, 1.0)
            y[:, n] = x[:, n] * gain

        return sanitize_audio(y, fallback=x).astype(np.float32, copy=False)


class DSREProcessor:
    def __init__(
        self,
        files: List[str],
        output_dir: str,
        params: Dict[str, Any],
        log_cb,
        file_progress_cb,
        step_progress_cb,
        stats_cb,
        abort_cb,
    ):
        self.files = files
        self.output_dir = output_dir
        self.params = params
        self.logs = log_cb
        self.file_progress = file_progress_cb
        self.step_progress = step_progress_cb
        self.stats = stats_cb
        self.abort_cb = abort_cb

        self.processing_stats = {
            "total_files": len(files),
            "processed_files": 0,
            "failed_files": 0,
            "total_size_mb": 0.0,
            "processed_size_mb": 0.0,
            "start_time": None,
        }

    def get_file_size_mb(self, file_path: str) -> float:
        try:
            return os.path.getsize(file_path) / (1024 * 1024)
        except OSError:
            return 0.0

    def process_audio_chunked(
        self,
        y: np.ndarray,
        sr: int,
        chunk_seconds: float = 10.0,
        overlap_seconds: float = 0.05,
    ) -> np.ndarray:
        if y.ndim == 1:
            y = y[np.newaxis, :]

        total_samples = y.shape[1]
        chunk_size = max(2048, int(sr * chunk_seconds))
        overlap = max(256, int(sr * overlap_seconds))

        if total_samples <= chunk_size:
            return enhanced_audio_algorithm(
                y,
                sr,
                enhancement_strength=float(self.params["decay"]),
                harmonic_intensity=float(self.params["m"]) / 16.0,
                stereo_width=float(self.params["stereo_width"]),
                dynamic_enhancement=float(self.params["dynamic"]),
                progress_cb=None,
                abort_cb=self.abort_cb,
            )

        out = np.zeros_like(y, dtype=np.float32)
        weight = np.zeros((1, total_samples), dtype=np.float32)

        step = max(1, chunk_size - overlap)

        for start in range(0, total_samples, step):
            if self.abort_cb():
                break

            end = min(total_samples, start + chunk_size)
            chunk = y[:, start:end]

            if chunk.size == 0:
                continue

            processed_chunk = enhanced_audio_algorithm(
                chunk,
                sr,
                enhancement_strength=float(self.params["decay"]),
                harmonic_intensity=float(self.params["m"]) / 16.0,
                stereo_width=float(self.params["stereo_width"]),
                dynamic_enhancement=float(self.params["dynamic"]),
                progress_cb=None,
                abort_cb=self.abort_cb,
            )

            chunk_len = processed_chunk.shape[1]
            fade = np.ones(chunk_len, dtype=np.float32)

            if start > 0:
                fade_in = min(overlap, chunk_len)
                fade[:fade_in] = np.linspace(0.0, 1.0, fade_in, dtype=np.float32)

            if end < total_samples:
                fade_out = min(overlap, chunk_len)
                fade[-fade_out:] = np.minimum(
                    fade[-fade_out:],
                    np.linspace(1.0, 0.0, fade_out, dtype=np.float32),
                )

            out[:, start:end] += processed_chunk * fade[np.newaxis, :]
            weight[:, start:end] += fade[np.newaxis, :]

        weight[weight == 0] = 1.0

        return (out / weight).astype(np.float32)

    def categorize_error(self, error: Exception) -> str:
        error_str = str(error).lower()

        if any(
            keyword in error_str
            for keyword in (
                    "permission denied",
                    "access denied",
                    "disk full",
                    "no space",
                    "libdsre_audio",
                    "dsre_decode_to_f32",
                    "dsre_encode_from_f32",
                    "dsre_last_error",
                    "dlopen failed",
                    "avformat_write_header",
                    "av_interleaved_write_frame",
                    "av_write_trailer",
                    "native audio",
                )
        ):
            return "fatal"

        if any(
            keyword in error_str
            for keyword in (
                "file not found",
                "no such file",
                "network",
                "timeout",
                "connection",
            )
        ):
            return "io"

        if any(
            keyword in error_str
            for keyword in (
                "memory",
                "out of memory",
                "allocation",
                "cannot allocate",
                "malloc",
                "std::bad_alloc",
            )
        ):
            return "fatal"

        if any(
            keyword in error_str
            for keyword in (
                "format",
                "codec",
                "sample rate",
                "bitrate",
            )
        ):
            return "format"

        if any(
            keyword in error_str
            for keyword in (
                "ffmpeg",
                "encoder",
                "decoder",
                "ffprobe",
            )
        ):
            return "ffmpeg"

        return "retry"

    def process_audio_streaming(
        self,
        in_path: str,
        out_path: str,
        target_sr: int,
        fmt: str,
        chunk_seconds: float = 6.0,
        overlap_seconds: float = 0.0,
    ) -> str:
        """Decode/process/encode using stateful DSP carry-over.

        This preserves sample count and avoids boundary shortening. DSP state is
        carried by DSREStreamingDSP: input context tail + dynamic envelope.
        """
        native = get_native_audio()
        if not getattr(native, "streaming_available", False):
            raise RuntimeError("libdsre_audio.so streaming API is not available")

        dec_handle = None
        enc_handle = None
        out_root, out_ext = os.path.splitext(out_path)
        tmp_out = f"{out_root}.tmp{out_ext}" if out_ext else out_path + ".tmp"
        final_written = False

        if os.path.exists(tmp_out):
            try:
                os.remove(tmp_out)
            except OSError:
                pass

        def write_ch_first_to_encoder(handle: c_void_p, pcm_ch_first: np.ndarray) -> None:
            pcm_ch_first = ensure_ch_first(sanitize_audio(pcm_ch_first))
            if pcm_ch_first.size == 0 or pcm_ch_first.shape[1] <= 0:
                return
            pcm_interleaved = np.ascontiguousarray(
                ensure_sf_shape(pcm_ch_first),
                dtype=np.float32,
            )
            native.encoder_write(handle, pcm_interleaved)

        try:
            preferred_samples = max(1024, int(target_sr * chunk_seconds))
            dec_handle, sr, channels = native.decoder_open(
                in_path,
                target_sr=target_sr,
                preferred_chunk_samples=preferred_samples,
            )
            if channels <= 0 or sr <= 0:
                raise RuntimeError(f"Invalid streaming decoder info: sr={sr}, channels={channels}")

            enc_handle = native.encoder_open(
                original_path=in_path,
                output_path=tmp_out,
                fmt=fmt,
                sr=sr,
                channels=channels,
            )

            max_samples = max(1024, int(sr * chunk_seconds))
            dsp = DSREStreamingDSP(
                sr=sr,
                channels=channels,
                params=self.params,
                context_seconds=float(self.params.get("dsp_context", 0.0)),
            )
            decode_buffer = np.empty((max_samples, channels), dtype=np.float32)
            chunk_index = 0
            total_in_rms = 0.0
            total_out_rms = 0.0
            rms_count = 0

            self.step_progress(3, "stream open")
            self.logs(
                f"Streaming DSP state carry: context={dsp.context_samples} samples "
                f"({(dsp.context_samples / sr) if sr else 0:.3f}s), chunk={max_samples} samples"
            )

            while True:
                if self.abort_cb():
                    raise RuntimeError("Processing aborted")

                samples_read, eof = native.decoder_read(
                    dec_handle,
                    decode_buffer,
                    max_samples,
                )

                if samples_read > 0:
                    chunk_index += 1
                    chunk = np.ascontiguousarray(decode_buffer[:samples_read, :].T, dtype=np.float32)
                    processed = dsp.process(chunk)
                    write_ch_first_to_encoder(enc_handle, processed)

                    try:
                        total_in_rms += audio_rms(chunk)
                        total_out_rms += audio_rms(processed)
                        rms_count += 1
                    except Exception:
                        pass

                    self.step_progress(min(94, 8 + (chunk_index % 86)), f"chunk {chunk_index}")
                    chunk = None
                    processed = None
                    force_release_memory()

                if eof:
                    break

            tail = dsp.flush()
            if tail is not None and tail.size > 0:
                write_ch_first_to_encoder(enc_handle, tail)

            self.step_progress(96, "finalizing")
            native.encoder_close(enc_handle)
            enc_handle = None
            os.replace(tmp_out, out_path)
            final_written = True

            if rms_count > 0:
                self.logs(
                    f"Streaming RMS avg: input={total_in_rms / rms_count:.6f}, "
                    f"output={total_out_rms / rms_count:.6f}"
                )
            return out_path

        finally:
            if dec_handle is not None:
                native.decoder_close(dec_handle)
            if enc_handle is not None:
                native.encoder_abort(enc_handle)
            if not final_written and os.path.exists(tmp_out):
                try:
                    os.remove(tmp_out)
                except OSError:
                    pass
            force_release_memory()

    def run(self):
        total = len(self.files)
        done = 0

        self.processing_stats["start_time"] = time.time()
        self.processing_stats["total_size_mb"] = sum(
            self.get_file_size_mb(path) for path in self.files
        )

        self.file_progress(done, total, "")
        self.stats(dict(self.processing_stats))
        os.makedirs(self.output_dir, exist_ok=True)

        native = get_native_audio()
        if not getattr(native, "streaming_available", False):
            self.logs("[red]Streaming API が libdsre_audio.so に見つかりません。native 側を再ビルドしてください。[/]")
            return

        for idx, in_path in enumerate(self.files, start=1):
            if self.abort_cb():
                self.logs("[yellow]Processing aborted[/]")
                break

            fname = os.path.basename(in_path)
            file_size_mb = self.get_file_size_mb(in_path)
            self.file_progress(idx, total, fname)
            self.step_progress(0, fname)
            self.logs(
                f"[cyan]Processing(streaming)[/] {fname} "
                f"({file_size_mb:.1f} MB, {idx}/{total})"
            )

            retry_count = 0
            max_retries = 1

            while retry_count <= max_retries:
                if self.abort_cb():
                    break

                try:
                    target_sr = int(self.params["target_sr"])
                    base, _ = os.path.splitext(os.path.basename(in_path))
                    ext_map = {"ALAC": "m4a", "FLAC": "flac", "MP3": "mp3"}
                    out_ext = ext_map.get(self.params["format"], "m4a")
                    out_path = os.path.join(self.output_dir, f"{base}_enhanced.{out_ext}")

                    self.logs(
                        "Streaming enhancement parameters: "
                        f"strength={self.params['decay']}, "
                        f"harmonics={self.params['m']}, "
                        f"stereo_width={self.params['stereo_width']}, "
                        f"dynamic={self.params['dynamic']}, "
                        f"target_sr={target_sr}"
                    )

                    out_path = self.process_audio_streaming(
                        in_path=in_path,
                        out_path=out_path,
                        target_sr=target_sr,
                        fmt=self.params["format"],
                    )

                    self.logs(f"[green]Saved:[/] {out_path}")
                    self.processing_stats["processed_files"] += 1
                    self.processing_stats["processed_size_mb"] += file_size_mb
                    break

                except Exception as e:
                    err = "".join(
                        traceback.format_exception_only(type(e), e)
                    ).strip()
                    retry_count += 1
                    error_type = self.categorize_error(e)

                    write_fflog(
                        "DSRE streaming processing exception",
                        err,
                        e,
                        extra={
                            "file": in_path,
                            "filename": fname,
                            "retry_count": retry_count,
                            "max_retries": max_retries,
                            "error_type": error_type,
                            "params": self.params,
                            "output_dir": self.output_dir,
                        },
                    )
                    force_release_memory()

                    if self.abort_cb():
                        self.logs("[yellow]Processing aborted[/]")
                        break

                    if retry_count <= max_retries and error_type != "fatal":
                        self.logs(
                            f"[yellow][Retry {retry_count}/{max_retries}][/]"
                            f" {fname}: {err}"
                        )
                        time.sleep(1)
                    else:
                        self.logs(f"[red][Error][/] {fname}: {err}")
                        self.logs(f"詳細は {FFLOG_FILE} を確認してください")
                        self.processing_stats["failed_files"] += 1
                        break

            done += 1
            self.file_progress(done, total, fname)
            self.step_progress(100, fname)
            self.stats(dict(self.processing_stats))
            force_release_memory()

        self.logs("[bold green]Processing finished[/]")

MATERIAL = {
    "bg": (0.070, 0.082, 0.102, 1),
    "surface": (0.105, 0.121, 0.149, 1),
    "surface_alt": (0.130, 0.150, 0.185, 1),
    "primary": (0.250, 0.430, 0.860, 1),
    "secondary": (0.180, 0.800, 0.650, 1),
    "danger": (0.980, 0.300, 0.300, 1),
    "text": (0.925, 0.941, 0.965, 1),
    "muted": (0.650, 0.700, 0.780, 1),
}


def get_ui_font() -> Optional[str]:
    candidates = [
        os.path.join(os.getcwd(), "_ja_JP.ttf"),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "_ja_JP.ttf"),
    ]
    for path in candidates:
        if os.path.isfile(path):
            return path
    return None


def apply_font(widget):
    font_path = get_ui_font()
    if font_path and hasattr(widget, "font_name"):
        try:
            widget.font_name = font_path
        except Exception:
            pass
    return widget


def strip_status_text(text: str) -> str:
    if text is None:
        return ""
    s = str(text)
    if s.startswith("<ClockEvent "):
        return ""
    s = re.sub(r"\[/?[a-zA-Z0-9_ #=;,.:-]+\]", "", s)
    return s.replace("\n", " ").strip()


class MaterialCard(BoxLayout):
    def __init__(self, radius=16, bg_color=None, **kwargs):
        super().__init__(**kwargs)
        self.padding = kwargs.get("padding", dp(10))
        self.spacing = kwargs.get("spacing", dp(8))
        with self.canvas.before:
            Color(*(bg_color or MATERIAL["surface"]))
            self._rect = RoundedRectangle(pos=self.pos, size=self.size, radius=[dp(radius)])
        self.bind(pos=self._sync_canvas, size=self._sync_canvas)

    def _sync_canvas(self, *_):
        self._rect.pos = self.pos
        self._rect.size = self.size


class MaterialButton(Button):
    def __init__(self, kind="primary", **kwargs):
        super().__init__(**kwargs)
        self.background_normal = ""
        self.background_down = ""
        self.color = MATERIAL["text"]
        self.bold = True
        self.size_hint_y = None
        self.height = kwargs.get("height", dp(40))
        self.font_size = kwargs.get("font_size", "13sp")
        font = get_ui_font()
        if font:
            self.font_name = font
        self.background_color = {
            "danger": MATERIAL["danger"],
            "secondary": MATERIAL["secondary"],
            "flat": MATERIAL["surface_alt"],
        }.get(kind, MATERIAL["primary"])


class MaterialInput(TextInput):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.multiline = kwargs.get("multiline", False)
        self.size_hint_y = None
        self.height = kwargs.get("height", dp(40))
        self.padding = [dp(10), dp(9), dp(10), dp(9)]
        self.background_normal = ""
        self.background_active = ""
        self.background_color = MATERIAL["surface_alt"]
        self.foreground_color = MATERIAL["text"]
        self.cursor_color = MATERIAL["secondary"]
        self.hint_text_color = MATERIAL["muted"]
        self.font_size = kwargs.get("font_size", "13sp")
        font = get_ui_font()
        if font:
            self.font_name = font


class MaterialLabel(Label):
    def __init__(self, **kwargs):
        kwargs.setdefault("color", MATERIAL["text"])
        kwargs.setdefault("font_size", "13sp")
        kwargs.setdefault("halign", "left")
        kwargs.setdefault("valign", "middle")
        super().__init__(**kwargs)
        font = get_ui_font()
        if font:
            self.font_name = font
        self.bind(size=lambda *_: setattr(self, "text_size", self.size))


class SectionTitle(MaterialLabel):
    def __init__(self, **kwargs):
        kwargs.setdefault("color", MATERIAL["secondary"])
        kwargs.setdefault("font_size", "15sp")
        kwargs.setdefault("bold", True)
        kwargs.setdefault("size_hint_y", None)
        kwargs.setdefault("height", dp(24))
        super().__init__(**kwargs)


class SmallLabel(MaterialLabel):
    def __init__(self, **kwargs):
        kwargs.setdefault("color", MATERIAL["muted"])
        kwargs.setdefault("font_size", "11sp")
        kwargs.setdefault("size_hint_y", None)
        kwargs.setdefault("height", dp(20))
        super().__init__(**kwargs)


class FileChooserPopup(ModalView):
    def __init__(self, select_callback, choose_dir=False, **kwargs):
        super().__init__(**kwargs)
        self.select_callback = select_callback
        self.size_hint = (0.96, 0.92)
        self.auto_dismiss = False
        root = MaterialCard(orientation="vertical")
        root.add_widget(SectionTitle(text="ディレクトリを選択" if choose_dir else "音声ファイルを選択"))
        self.chooser = FileChooserListView(path=os.getenv('EXTERNAL_STORAGE'), dirselect=choose_dir)
        if not choose_dir:
            self.chooser.filters = [lambda folder, filename: os.path.isdir(filename) or os.path.splitext(filename.lower())[1] in AUDIO_EXTENSIONS]
        root.add_widget(self.chooser)
        row = BoxLayout(size_hint_y=None, height=dp(42), spacing=dp(8))
        cancel = MaterialButton(text="キャンセル", kind="flat")
        ok = MaterialButton(text="選択", kind="primary")
        cancel.bind(on_release=lambda *_: self.dismiss())
        ok.bind(on_release=self._select)
        row.add_widget(cancel)
        row.add_widget(ok)
        root.add_widget(row)
        self.add_widget(root)

    def _select(self, *_):
        if self.chooser.selection:
            self.select_callback(self.chooser.selection[0])
        self.dismiss()


class DSREKivyRoot(BoxLayout):
    processing = BooleanProperty(False)
    files = ListProperty([])

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.orientation = "vertical"
        self.cancel_requested = False
        self.processor_thread = None
        self.config_path = CONFIG_FILE
        Window.minimum_width = 360
        Window.minimum_height = 560
        Window.clearcolor = MATERIAL["bg"]
        with self.canvas.before:
            Color(*MATERIAL["bg"])
            self._bg = Rectangle(pos=self.pos, size=self.size)
        self.bind(pos=self._sync_bg, size=self._sync_bg)
        self._build_ui()
        self.load_config(log=False)
        self.update_status("Ready")

    def _sync_bg(self, *_):
        self._bg.pos = self.pos
        self._bg.size = self.size

    def _build_ui(self):
        header = MaterialCard(orientation="vertical", size_hint_y=None, height=dp(70), padding=dp(10))
        header.add_widget(MaterialLabel(text="DSRE Audio Enhancer", font_size="20sp", bold=True, size_hint_y=None, height=dp(30)))
        font_info = get_ui_font() or "_ja_JP.ttf not found"
        header.add_widget(SmallLabel(text="version: 2.0.0"))
        self.add_widget(header)

        scroll = ScrollView(do_scroll_x=False)
        content = BoxLayout(orientation="vertical", size_hint_y=None, spacing=dp(10), padding=dp(10))
        content.bind(minimum_height=content.setter("height"))
        scroll.add_widget(content)
        self.add_widget(scroll)
        file_card = MaterialCard(orientation="vertical", size_hint_y=None)
        file_card.bind(minimum_height=file_card.setter("height"))
        file_card.add_widget(SectionTitle(text="入力"))
        row_file = BoxLayout(size_hint_y=None, height=dp(40), spacing=dp(6))
        self.input_file = MaterialInput(hint_text="音声ファイルパス")
        browse_file = MaterialButton(text="参照", kind="flat", size_hint_x=None, width=dp(70))
        browse_file.bind(on_release=lambda *_: FileChooserPopup(self._set_file_path).open())
        row_file.add_widget(self.input_file)
        row_file.add_widget(browse_file)
        file_card.add_widget(row_file)
        add_file = MaterialButton(text="ファイル追加", kind="primary")
        add_file.bind(on_release=lambda *_: self.handle_add_file())
        file_card.add_widget(add_file)

        row_dir = BoxLayout(size_hint_y=None, height=dp(40), spacing=dp(6))
        self.input_directory = MaterialInput(hint_text="ディレクトリ一括追加")
        browse_dir = MaterialButton(text="参照", kind="flat", size_hint_x=None, width=dp(70))
        browse_dir.bind(on_release=lambda *_: FileChooserPopup(self._set_dir_path, choose_dir=True).open())
        row_dir.add_widget(self.input_directory)
        row_dir.add_widget(browse_dir)
        file_card.add_widget(row_dir)
        scan_dir = MaterialButton(text="再帰スキャン", kind="primary")
        scan_dir.bind(on_release=lambda *_: self.handle_scan_directory())
        file_card.add_widget(scan_dir)
        self.file_summary_label = SmallLabel(text="0 files", height=dp(38))
        file_card.add_widget(self.file_summary_label)
        clear = MaterialButton(text="リストクリア", kind="flat")
        clear.bind(on_release=lambda *_: self.clear_files())
        file_card.add_widget(clear)
        content.add_widget(file_card)
        param_card = MaterialCard(orientation="vertical", size_hint_y=None)
        param_card.bind(minimum_height=param_card.setter("height"))
        param_card.add_widget(SectionTitle(text="設定"))
        self.input_m = self._param(param_card, "Harmonic 1-32", "15")
        self.input_decay = self._param(param_card, "Strength 0.1-1.0", "0.47")
        self.input_sr = self._param(param_card, "Sample Rate", "48000")
        param_card.add_widget(SmallLabel(text="Format"))
        self.input_format = Spinner(text="ALAC", values=("ALAC", "FLAC", "MP3"), size_hint_y=None, height=dp(40), background_normal="", background_color=MATERIAL["surface_alt"], color=MATERIAL["text"])
        font = get_ui_font()
        if font:
            self.input_format.font_name = font
        param_card.add_widget(self.input_format)
        self.input_stereo_width = self._param(param_card, "Stereo Width", "0.98")
        self.input_dynamic = self._param(param_card, "Dynamic", "1.11")
        self.input_chunk_threshold = self._param(param_card, "Chunk MB", "150")
        self.input_dsp_context = self._param(param_card, "DSP Context sec", "0.04")
        param_card.add_widget(SmallLabel(text="Output Directory"))
        row_out = BoxLayout(size_hint_y=None, height=dp(40), spacing=dp(6))
        if os.getenv('EXTERNAL_STORAGE'):
            out = os.path.join(os.getenv('EXTERNAL_STORAGE'), "Documents")
        else:
            out = os.path.expanduser('~')
        self.input_output_dir = MaterialInput(text=os.path.join(out, "enhanced_output"))
        browse_out = MaterialButton(text="参照", kind="flat", size_hint_x=None, width=dp(70))
        browse_out.bind(on_release=lambda *_: FileChooserPopup(self._set_output_dir, choose_dir=True).open())
        row_out.add_widget(self.input_output_dir)
        row_out.add_widget(browse_out)
        param_card.add_widget(row_out)
        cfg_row = BoxLayout(size_hint_y=None, height=dp(40), spacing=dp(6))
        save = MaterialButton(text="設定保存", kind="secondary")
        load = MaterialButton(text="設定読込", kind="flat")
        save.bind(on_release=lambda *_: self.save_config())
        load.bind(on_release=lambda *_: self.load_config(log=True))
        cfg_row.add_widget(save)
        cfg_row.add_widget(load)
        param_card.add_widget(cfg_row)
        content.add_widget(param_card)
        proc_card = MaterialCard(orientation="vertical", size_hint_y=None)
        proc_card.bind(minimum_height=proc_card.setter("height"))
        proc_card.add_widget(SectionTitle(text="処理"))
        action_row = BoxLayout(size_hint_y=None, height=dp(42), spacing=dp(6))
        self.start_button = MaterialButton(text="開始", kind="primary")
        self.cancel_button = MaterialButton(text="キャンセル", kind="danger")
        self.retry_button = MaterialButton(text="再処理", kind="flat")
        self.start_button.bind(on_release=lambda *_: self.start_processing())
        self.cancel_button.bind(on_release=lambda *_: self.cancel_processing())
        self.retry_button.bind(on_release=lambda *_: self.start_processing())
        action_row.add_widget(self.start_button)
        action_row.add_widget(self.cancel_button)
        action_row.add_widget(self.retry_button)
        proc_card.add_widget(action_row)
        self.update_action_buttons()
        proc_card.add_widget(SmallLabel(text="現在ファイル"))
        self.file_bar = ProgressBar(max=100, value=0, size_hint_y=None, height=dp(14))
        proc_card.add_widget(self.file_bar)
        proc_card.add_widget(SmallLabel(text="全体"))
        self.overall_bar = ProgressBar(max=100, value=0, size_hint_y=None, height=dp(14))
        proc_card.add_widget(self.overall_bar)
        self.status_label = MaterialLabel(text="Ready", size_hint_y=None, height=dp(34), font_size="12sp")
        proc_card.add_widget(self.status_label)
        self.stats_label = MaterialLabel(text="0 files ready", color=MATERIAL["muted"], size_hint_y=None, height=dp(54), font_size="12sp")
        proc_card.add_widget(self.stats_label)
        content.add_widget(proc_card)

    def _param(self, parent, label, default):
        parent.add_widget(SmallLabel(text=label))
        widget = MaterialInput(text=default)
        parent.add_widget(widget)
        return widget

    def _set_file_path(self, path):
        self.input_file.text = path

    def _set_dir_path(self, path):
        self.input_directory.text = path

    def _set_output_dir(self, path):
        self.input_output_dir.text = path

    def write_log(self, message):
        clean = strip_status_text(message)

    def thread_log(self, message):
        clean = strip_status_text(message)
        if not clean:
            return
        def _update(_dt):
            self.write_log(clean)
            return None
        Clock.schedule_once(_update, 0)

    def update_status(self, text=None):
        if text and hasattr(self, "status_label"):
            self.status_label.text = text
        if hasattr(self, "file_summary_label"):
            if self.files:
                last = os.path.basename(self.files[-1])
                self.file_summary_label.text = f"{len(self.files)} files / last: {last}"
            else:
                self.file_summary_label.text = "0 files"
        if hasattr(self, "stats_label") and not self.processing:
            self.stats_label.text = f"{len(self.files)} files ready"

    def add_file_to_list(self, path):
        path = os.path.abspath(os.path.expanduser(path)) if path else ""
        if not is_audio_file(path) or path in self.files:
            return False
        self.files.append(path)
        self.update_status("Ready")
        return True

    def add_directory_to_list(self, directory, recursive=True):
        directory = os.path.abspath(os.path.expanduser(directory))
        added = 0
        for path in collect_audio_files_from_directory(directory, recursive=recursive):
            if self.add_file_to_list(path):
                added += 1
        self.update_status("Ready")
        return added

    def handle_add_file(self):
        path = self.input_file.text.strip()
        if self.add_file_to_list(path):
            self.write_log(f"Added: {os.path.basename(path)}")
        else:
            self.write_log("追加できません: 音声ファイルでない、存在しない、または追加済みです")

    def handle_scan_directory(self):
        directory = os.path.abspath(os.path.expanduser(self.input_directory.text.strip()))
        if not os.path.isdir(directory):
            self.write_log(f"Directory not found: {directory}")
            return
        added = self.add_directory_to_list(directory, recursive=True)
        self.write_log(f"Directory scan completed: {added} files added")

    def read_params(self):
        fmt = (self.input_format.text.strip() or "ALAC").upper()
        if fmt not in ("ALAC", "FLAC", "MP3"):
            raise ValueError("Output format must be ALAC, FLAC, or MP3")
        return {
            "m": int(np.clip(int(self.input_m.text.strip() or "15"), 1, 32)),
            "decay": float(np.clip(float(self.input_decay.text.strip() or "0.47"), 0.1, 1.0)),
            "target_sr": int(np.clip(int(self.input_sr.text.strip() or "48000"), 44100, 192000)),
            "format": fmt,
            "stereo_width": float(np.clip(float(self.input_stereo_width.text.strip() or "0.98"), 1.0, 1.8)),
            "dynamic": float(np.clip(float(self.input_dynamic.text.strip() or "1.11"), 1.0, 1.5)),
            "chunk_threshold_mb": max(1.0, float(self.input_chunk_threshold.text.strip() or "150")),
            "dsp_context": float(np.clip(float(self.input_dsp_context.text.strip() or "0.04"), 0.0, 0.08)),
        }

    def update_action_buttons(self):
        # Enable/disable processing action buttons based on processing state.
        is_processing = bool(self.processing)

        start = getattr(self, "start_button", None)
        retry = getattr(self, "retry_button", None)
        cancel = getattr(self, "cancel_button", None)

        for btn in (start, retry):
            if btn is not None:
                btn.disabled = is_processing
                btn.opacity = 0.45 if is_processing else 1.0

        if cancel is not None:
            cancel.disabled = not is_processing
            cancel.opacity = 1.0 if is_processing else 0.45

    def start_processing(self):
        if self.processing:
            self.write_log("Already processing")
            return
        if not self.files:
            self.write_log("No files selected")
            return
        try:
            params = self.read_params()
        except Exception as e:
            self.write_log(f"Invalid parameters: {e}")
            return
        output_dir = os.path.abspath(os.path.expanduser(self.input_output_dir.text.strip() or os.path.join(EXTERNAL_STORAGE, "Documents", "enhanced_output")))
        os.makedirs(output_dir, exist_ok=True)
        self.processing = True
        self.cancel_requested = False
        self.update_action_buttons()
        self._processor_finished = False
        self._finish_poll_event = None
        self.file_bar.value = 0
        self.overall_bar.value = 0
        self.status_label.text = "Processing..."
        processor = DSREProcessor(
            files=list(self.files),
            output_dir=output_dir,
            params=params,
            log_cb=self.thread_log,
            file_progress_cb=self.thread_update_file_progress,
            step_progress_cb=self.thread_update_step_progress,
            stats_cb=self.thread_update_stats,
            abort_cb=self.abort_requested,
        )
        self.processor_thread = threading.Thread(target=self._run_processor, args=(processor,), daemon=True)
        self.processor_thread.start()
        # Schedule polling from the Kivy main thread. The worker thread must not schedule Clock callbacks.
        self._finish_poll_event = Clock.schedule_interval(self._poll_processor_finished, 0.2)

    def _run_processor(self, processor):
        try:
            processor.run()
        finally:
            # Do not touch Kivy widgets and do not call Clock from this worker thread.
            self._processor_finished = True

    def _poll_processor_finished(self, _dt):
        if not getattr(self, "_processor_finished", False):
            return True
        self.on_processing_finished()
        return False

    def abort_requested(self):
        return self.cancel_requested

    def thread_update_file_progress(self, done, total, fname):
        def _update(_dt):
            self.update_file_progress(done, total, fname)
            return None
        Clock.schedule_once(_update, 0)

    def update_file_progress(self, done, total, fname):
        # Ignore late queued progress callbacks after finish/cancel has been applied.
        if not self.processing:
            return
        self.overall_bar.value = int(done * 100 / max(1, total))
        self.status_label.text = f"{done}/{total}: {fname}" if fname else "Processing..."

    def thread_update_step_progress(self, pct, fname):
        def _update(_dt):
            self.file_bar.value = max(0, min(100, int(pct)))
            return None
        Clock.schedule_once(_update, 0)

    def thread_update_stats(self, stats):
        def _update(_dt):
            self.update_stats(stats)
            return None
        Clock.schedule_once(_update, 0)

    def update_stats(self, stats):
        total = stats.get("total_files", 0)
        processed = stats.get("processed_files", 0)
        failed = stats.get("failed_files", 0)
        processed_size = stats.get("processed_size_mb", 0.0)
        total_size = stats.get("total_size_mb", 0.0)
        self.stats_label.text = (
            f"Processed: {processed}/{total}\n"
            f"Failed: {failed}\n"
            f"Size: {processed_size:.1f}/{total_size:.1f} MB"
        )

    def on_processing_finished(self):
        try:
            force_release_memory()
        except Exception:
            pass

        was_cancel_requested = bool(getattr(self, "cancel_requested", False))

        # Mark processing as fully stopped before updating action buttons.
        self.processing = False
        self.cancel_requested = False
        self._processor_finished = False

        # Restore button state: Start/Retry enabled, Cancel disabled.
        try:
            self.update_action_buttons()
        except Exception:
            pass

        try:
            self.file_bar.value = 100
            self.overall_bar.value = 100
        except Exception:
            pass

        self.status_label.text = "Canceled" if was_cancel_requested else "Finished"
        self.update_status()
    def cancel_processing(self):
        if self.processing:
            self.cancel_requested = True
            self.status_label.text = "Cancel requested"
            
        else:
            self.write_log("No active processing")

    def clear_files(self):
        if self.processing:
            self.write_log("Cannot clear while processing")
            return
        self.files = []
        self.file_bar.value = 0
        self.overall_bar.value = 0
        
        self.update_status("Ready")

    def show_alert(self, title: str, message: str):
        """Small in-app modal alert. Save/load must continue even if popup fails."""
        try:
            popup = ModalView(size_hint=(0.88, None), height=dp(190), auto_dismiss=True)
            root = MaterialCard(orientation="vertical", padding=dp(12), spacing=dp(10))
            root.add_widget(SectionTitle(text=str(title)))
            root.add_widget(
                MaterialLabel(
                    text=str(message),
                    size_hint_y=None,
                    height=dp(78),
                    halign="left",
                    valign="middle",
                )
            )
            ok = MaterialButton(text="OK", kind="primary", size_hint_y=None, height=dp(42))
            ok.bind(on_release=lambda *_: popup.dismiss())
            root.add_widget(ok)
            popup.add_widget(root)
            popup.open()
        except Exception:
            try:
                self.write_log(f"{title}: {message}")
            except Exception:
                pass

    def save_config(self):
        try:
            os.makedirs(os.path.dirname(self.config_path), exist_ok=True)
            config = {
                "m": self.input_m.text,
                "decay": self.input_decay.text,
                "target_sr": self.input_sr.text,
                "format": self.input_format.text,
                "stereo_width": self.input_stereo_width.text,
                "dynamic": self.input_dynamic.text,
                "chunk_threshold_mb": self.input_chunk_threshold.text,
                "dsp_context": getattr(self, "input_dsp_context", None).text if getattr(self, "input_dsp_context", None) else "0.04",
                "output_dir": self.input_output_dir.text,
                "last_directory": self.input_directory.text,
                "last_file": self.input_file.text,
            }
            with open(self.config_path, "w", encoding="utf-8") as f:
                json.dump(config, f, indent=2, ensure_ascii=False)

            self.write_log(f"[green]設定を保存しました:[/] {self.config_path}")
            self.status_label.text = "設定を保存しました"
            self.show_alert("設定保存", f"設定を書き込みました\n{self.config_path}")
        except Exception as e:
            self.write_log(f"[red]設定保存に失敗しました:[/] {e}")
            try:
                self.status_label.text = "設定保存に失敗しました"
            except Exception:
                pass
            try:
                self.show_alert("設定保存エラー", f"設定保存に失敗しました\n{e}")
            except Exception:
                pass

    def load_config(self, log=False):
        try:
            if not os.path.exists(self.config_path):
                if log:
                    self.write_log(f"設定ファイルが見つかりません: {self.config_path}")
                    self.status_label.text = "設定ファイルが見つかりません"
                    self.show_alert("設定読み込み", f"設定ファイルが見つかりません\n{self.config_path}")
                return

            with open(self.config_path, "r", encoding="utf-8") as f:
                config = json.load(f)

            self.input_m.text = str(config.get("m", "15"))
            self.input_decay.text = str(config.get("decay", "0.47"))
            self.input_sr.text = str(config.get("target_sr", "48000"))
            self.input_format.text = str(config.get("format", "ALAC"))
            self.input_stereo_width.text = str(config.get("stereo_width", "0.98"))
            self.input_dynamic.text = str(config.get("dynamic", "1.11"))
            self.input_chunk_threshold.text = str(config.get("chunk_threshold_mb", "150"))
            if hasattr(self, "input_dsp_context"):
                self.input_dsp_context.text = str(config.get("dsp_context", "0.04"))
            self.input_output_dir.text = str(
                config.get(
                    "output_dir",
                    os.path.join(EXTERNAL_STORAGE, "Documents", "enhanced_output"),
                )
            )
            self.input_directory.text = str(config.get("last_directory", ""))
            self.input_file.text = str(config.get("last_file", ""))

            if log:
                self.write_log(f"[green]設定を読み込みました:[/] {self.config_path}")
                self.status_label.text = "設定を読み込みました"
                self.show_alert("設定読み込み", f"設定を読み込みました\n{self.config_path}")
        except Exception as e:
            if log:
                self.write_log(f"[red]設定読み込みに失敗しました:[/] {e}")
                try:
                    self.status_label.text = "設定読み込みに失敗しました"
                except Exception:
                    pass
                try:
                    self.show_alert("設定読み込みエラー", f"設定読み込みに失敗しました\n{e}")
                except Exception:
                    pass

class DSREKivyApp(App):
    title = APP_NAME

    def build(self):
        return DSREKivyRoot()


def main():
    DSREKivyApp().run()


if __name__ == "__main__":
    main()
