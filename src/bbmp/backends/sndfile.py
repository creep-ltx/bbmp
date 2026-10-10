# SPDX-License-Identifier: GPL-3.0-or-later
"""Ordinary audio files: MP3, Ogg Vorbis, Opus, FLAC, WAV, AIFF, through libsndfile,
resampled to the player's rate by libsamplerate (both part of every Linux desktop, and
of the Flatpak runtime). For collections that have such files (Scene.org's music); a
collection's format "Audio file" is played by it."""

from __future__ import annotations

import ctypes as C
from pathlib import Path

import numpy as np

from .base import RATE, Decoder, Details, TrackInfo, Unsupported, load_library

FORMAT = "Audio file"          # the format a collection gives such files

SFM_READ = 0x10
SF_FORMAT_TYPEMASK, SF_FORMAT_SUBMASK = 0x0FFF0000, 0x0000FFFF
TYPES = {0x010000: "WAV", 0x020000: "AIFF", 0x170000: "FLAC", 0x200000: "Ogg",
         0x230000: "MP3"}
SUBTYPES = {0x0060: "Vorbis", 0x0064: "Opus"}
# sf_get_string's kinds
TITLE, COPYRIGHT, SOFTWARE, ARTIST, COMMENT, DATE, ALBUM = 1, 2, 3, 4, 5, 6, 7
SRC_SINC_MEDIUM_QUALITY = 1
CHUNK = 4096            # frames read from the file at a time


class _Info(C.Structure):
    _fields_ = [("frames", C.c_int64), ("samplerate", C.c_int), ("channels", C.c_int),
                ("format", C.c_int), ("sections", C.c_int), ("seekable", C.c_int)]


class _SrcData(C.Structure):
    _fields_ = [("data_in", C.POINTER(C.c_float)), ("data_out", C.POINTER(C.c_float)),
                ("input_frames", C.c_long), ("output_frames", C.c_long),
                ("input_frames_used", C.c_long), ("output_frames_gen", C.c_long),
                ("end_of_input", C.c_int), ("src_ratio", C.c_double)]


_sf = _src = None


def _libs():
    global _sf, _src
    if _sf is None:
        sf = load_library("sndfile")
        sf.sf_open.restype = C.c_void_p
        sf.sf_open.argtypes = [C.c_char_p, C.c_int, C.POINTER(_Info)]
        sf.sf_readf_float.restype = C.c_int64
        sf.sf_readf_float.argtypes = [C.c_void_p, C.c_void_p, C.c_int64]
        sf.sf_seek.restype = C.c_int64
        sf.sf_seek.argtypes = [C.c_void_p, C.c_int64, C.c_int]
        sf.sf_get_string.restype = C.c_char_p
        sf.sf_get_string.argtypes = [C.c_void_p, C.c_int]
        sf.sf_strerror.restype = C.c_char_p
        sf.sf_strerror.argtypes = [C.c_void_p]
        sf.sf_close.argtypes = [C.c_void_p]
        src = load_library("samplerate")
        src.src_new.restype = C.c_void_p
        src.src_new.argtypes = [C.c_int, C.c_int, C.POINTER(C.c_int)]
        src.src_process.argtypes = [C.c_void_p, C.POINTER(_SrcData)]
        src.src_reset.argtypes = [C.c_void_p]
        src.src_delete.argtypes = [C.c_void_p]
        _sf, _src = sf, src
    return _sf, _src


def _text(b: bytes | None) -> str:
    return b.decode("utf-8", "replace").strip() if b else ""


class AudioFile(Decoder):
    backend = "sndfile"

    def __init__(self, path: Path):
        sf, src = _libs()
        info = _Info()
        self._file = sf.sf_open(str(path).encode(), SFM_READ, C.byref(info))
        if not self._file:
            raise Unsupported(_text(sf.sf_strerror(None)) or "not an audio file libsndfile reads")
        self._rate, self._channels = info.samplerate, info.channels
        if self._rate <= 0 or self._channels <= 0:
            sf.sf_close(self._file)
            raise Unsupported("an audio file without sound")
        self._src = None
        if self._rate != RATE:
            error = C.c_int(0)
            self._src = src.src_new(SRC_SINC_MEDIUM_QUALITY, 2, C.byref(error))
            if not self._src:
                sf.sf_close(self._file)
                raise Unsupported("libsamplerate failed to start")
        self._seekable = bool(info.seekable)
        self._ended = False
        self._out = np.zeros((0, 2), np.float32)
        kind = TYPES.get(info.format & SF_FORMAT_TYPEMASK, "audio")
        sub = SUBTYPES.get(info.format & SF_FORMAT_SUBMASK)
        self.info = TrackInfo(
            title=_text(sf.sf_get_string(self._file, TITLE)) or path.stem,
            author=_text(sf.sf_get_string(self._file, ARTIST)),
            format=f"{kind} ({sub})" if sub else kind,
            duration=info.frames / self._rate if 0 < info.frames < 2 ** 62 else None)
        self._facts = {label: _text(sf.sf_get_string(self._file, kind_))
                       for label, kind_ in (("Album", ALBUM), ("Date", DATE),
                                            ("Comment", COMMENT), ("Copyright", COPYRIGHT),
                                            ("Made with", SOFTWARE))}

    def _pull(self) -> None:
        """Read a piece of the file, as stereo at the player's rate, onto the output."""
        sf, src = _libs()
        buf = np.empty((CHUNK, self._channels), np.float32)
        got = sf.sf_readf_float(self._file, buf.ctypes.data, CHUNK)
        if got <= 0:
            self._ended = True
            got = 0
        piece = buf[:got]
        if self._channels == 1:
            piece = np.repeat(piece, 2, axis=1)
        elif self._channels > 2:
            piece = piece[:, :2]
        piece = np.ascontiguousarray(piece, np.float32)
        if self._src is None:
            self._out = np.concatenate([self._out, piece])
            return
        ratio = RATE / self._rate
        room = int(len(piece) * ratio) + 64 if len(piece) else 4096
        out = np.empty((room, 2), np.float32)
        data = _SrcData(piece.ctypes.data_as(C.POINTER(C.c_float)),
                        out.ctypes.data_as(C.POINTER(C.c_float)),
                        len(piece), room, 0, 0, int(self._ended), ratio)
        src.src_process(self._src, C.byref(data))
        self._out = np.concatenate([self._out, out[:data.output_frames_gen]])
        if self._ended and data.output_frames_gen == room:
            self._ended = False        # more of the resampler's tail is still to come

    def read(self, frames: int) -> bytes:
        while len(self._out) < frames and not self._ended:
            self._pull()
        take, self._out = self._out[:frames], self._out[frames:]
        return (np.clip(take, -1.0, 1.0) * 32767).astype("<i2").tobytes()

    def seek(self, seconds: float) -> bool:
        if not self._seekable:
            return False
        sf, src = _libs()
        if sf.sf_seek(self._file, int(max(0.0, seconds) * self._rate), 0) < 0:
            return False
        if self._src is not None:
            src.src_reset(self._src)
        self._out = np.zeros((0, 2), np.float32)
        self._ended = False
        return True

    def details(self) -> Details:
        d = super().details()
        for label, value in self._facts.items():
            d.fact(label, value)
        d.fact("Sample rate", f"{self._rate} Hz, {self._channels} channel"
                              f"{'s' if self._channels != 1 else ''}")
        return d

    def close(self) -> None:
        sf, src = _libs()
        if self._file:
            sf.sf_close(self._file)
            self._file = None
        if self._src:
            src.src_delete(self._src)
            self._src = None
