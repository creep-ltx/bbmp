# SPDX-License-Identifier: GPL-3.0-or-later
"""libopenmpt: Protracker, Fasttracker, Impulse Tracker, OctaMED and dozens more tracker formats."""

from __future__ import annotations

import ctypes as C
from pathlib import Path

from .base import FRAME_BYTES, RATE, Decoder, Details, TrackInfo, Unsupported, load_library

_lib = None


def lib():
    global _lib
    if _lib is None:
        L = load_library("openmpt")
        L.openmpt_module_create_from_memory2.restype = C.c_void_p
        L.openmpt_module_create_from_memory2.argtypes = [
            C.c_void_p, C.c_size_t, C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p,
            C.POINTER(C.c_int), C.POINTER(C.c_char_p), C.c_void_p]
        L.openmpt_module_destroy.argtypes = [C.c_void_p]
        L.openmpt_module_read_interleaved_stereo.restype = C.c_size_t
        L.openmpt_module_read_interleaved_stereo.argtypes = [C.c_void_p, C.c_int32, C.c_size_t, C.c_void_p]
        L.openmpt_module_get_duration_seconds.restype = C.c_double
        L.openmpt_module_get_duration_seconds.argtypes = [C.c_void_p]
        L.openmpt_module_set_position_seconds.restype = C.c_double
        L.openmpt_module_set_position_seconds.argtypes = [C.c_void_p, C.c_double]
        L.openmpt_module_get_metadata.restype = C.c_void_p
        L.openmpt_module_get_metadata.argtypes = [C.c_void_p, C.c_char_p]
        L.openmpt_free_string.argtypes = [C.c_void_p]
        L.openmpt_module_get_num_subsongs.restype = C.c_int32
        L.openmpt_module_get_num_subsongs.argtypes = [C.c_void_p]
        L.openmpt_module_select_subsong.argtypes = [C.c_void_p, C.c_int32]
        L.openmpt_module_set_repeat_count.argtypes = [C.c_void_p, C.c_int32]
        L.openmpt_module_set_render_param.argtypes = [C.c_void_p, C.c_int, C.c_int32]
        for what in ("samples", "instruments", "patterns", "orders"):
            f = getattr(L, f"openmpt_module_get_num_{what}")
            f.restype, f.argtypes = C.c_int32, [C.c_void_p]
        for what in ("sample", "instrument"):
            f = getattr(L, f"openmpt_module_get_{what}_name")
            f.restype, f.argtypes = C.c_void_p, [C.c_void_p, C.c_int32]
        L.openmpt_module_get_num_channels.restype = C.c_int32
        L.openmpt_module_get_num_channels.argtypes = [C.c_void_p]
        L.openmpt_module_get_current_channel_vu_mono.restype = C.c_float
        L.openmpt_module_get_current_channel_vu_mono.argtypes = [C.c_void_p, C.c_int32]
        L.openmpt_module_get_current_pattern.restype = C.c_int32
        L.openmpt_module_get_current_pattern.argtypes = [C.c_void_p]
        L.openmpt_module_get_current_row.restype = C.c_int32
        L.openmpt_module_get_current_row.argtypes = [C.c_void_p]
        L.openmpt_module_get_pattern_num_rows.restype = C.c_int32
        L.openmpt_module_get_pattern_num_rows.argtypes = [C.c_void_p, C.c_int32]
        L.openmpt_module_format_pattern_row_channel.restype = C.c_void_p
        L.openmpt_module_format_pattern_row_channel.argtypes = [
            C.c_void_p, C.c_int32, C.c_int32, C.c_int32, C.c_size_t, C.c_int]
        _lib = L
    return _lib


_STEREO_SEPARATION = 2   # OPENMPT_MODULE_RENDER_STEREOSEPARATION_PERCENT


class OpenMPT(Decoder):
    backend = "openmpt"

    def __init__(self, path: Path, stereo_separation: int = 60):
        L = lib()
        data = path.read_bytes()
        err = C.c_int()
        silent = C.cast(L.openmpt_log_func_silent, C.c_void_p)
        self._mod = L.openmpt_module_create_from_memory2(
            data, len(data), silent, None, None, None, C.byref(err), None, None)
        if not self._mod:
            raise Unsupported(f"libopenmpt can't open {path.name}")
        # Amiga modules are hard-panned; 60% separation is easier on headphones.
        L.openmpt_module_set_render_param(self._mod, _STEREO_SEPARATION, stereo_separation)
        L.openmpt_module_set_repeat_count(self._mod, 0)
        self._buf = C.create_string_buffer(0)
        self._channels = L.openmpt_module_get_num_channels(self._mod)
        n = max(1, L.openmpt_module_get_num_subsongs(self._mod))
        self.info = TrackInfo(
            title=self._meta("title"), author=self._meta("artist"),
            format=self._meta("type_long"), subsongs=n,
            duration=L.openmpt_module_get_duration_seconds(self._mod) or None,
            extra={"tracker": self._meta("tracker"), "channels": str(self._channels)})

    def _meta(self, key: str) -> str:
        p = lib().openmpt_module_get_metadata(self._mod, key.encode())
        if not p:
            return ""
        s = C.string_at(p).decode("utf-8", "replace")
        lib().openmpt_free_string(p)
        return s.strip()

    def _names(self, what: str) -> list[str]:
        L = lib()
        out = []
        for i in range(getattr(L, f"openmpt_module_get_num_{what}s")(self._mod)):
            p = getattr(L, f"openmpt_module_get_{what}_name")(self._mod, i)
            out.append(C.string_at(p).decode("utf-8", "replace") if p else "")
            if p:
                L.openmpt_free_string(p)
        return out

    def details(self) -> Details:
        L = lib()
        d = Details()
        d.fact("Title", self.info.title)
        d.fact("Author", self._meta("artist"))
        d.fact("Format", self.info.format)
        d.fact("Made with", self._meta("tracker"))
        d.fact("Saved", self._meta("date"))
        d.fact("Channels", self._channels)
        d.fact("Patterns", L.openmpt_module_get_num_patterns(self._mod))
        d.fact("Song positions", L.openmpt_module_get_num_orders(self._mod))
        d.text("Song message", self._meta("message_raw").splitlines())
        d.text("Instrument names", self._names("instrument"))
        d.text("Sample names", self._names("sample"))
        return d

    def read(self, frames: int) -> bytes:
        if len(self._buf) < frames * FRAME_BYTES:
            self._buf = C.create_string_buffer(frames * FRAME_BYTES)
        n = lib().openmpt_module_read_interleaved_stereo(self._mod, RATE, frames, self._buf)
        return self._buf.raw[:n * FRAME_BYTES]

    def loop_forever(self, on: bool) -> bool:
        lib().openmpt_module_set_repeat_count(self._mod, -1 if on else 0)
        return True

    def select_subsong(self, n: int) -> None:
        lib().openmpt_module_select_subsong(self._mod, n)
        self.info.subsong = n
        self.info.duration = lib().openmpt_module_get_duration_seconds(self._mod) or None

    def seek(self, seconds: float) -> bool:
        lib().openmpt_module_set_position_seconds(self._mod, max(0.0, seconds))
        return True

    def channel_levels(self) -> list[float]:
        f = lib().openmpt_module_get_current_channel_vu_mono
        return [min(1.0, f(self._mod, c)) for c in range(self._channels)]

    def pattern_view(self, rows: int) -> tuple[int, list[str]]:
        L = lib()
        pat = L.openmpt_module_get_current_pattern(self._mod)
        row = L.openmpt_module_get_current_row(self._mod)
        total = L.openmpt_module_get_pattern_num_rows(self._mod, pat)
        half = rows // 2
        lines = []
        for r in range(row - half, row - half + rows):
            if 0 <= r < total:
                cells = []
                for c in range(min(self._channels, 8)):
                    p = L.openmpt_module_format_pattern_row_channel(self._mod, pat, r, c, 13, 1)
                    cells.append(C.string_at(p).decode("latin-1") if p else "")
                    if p:
                        L.openmpt_free_string(p)
                lines.append(f"{r:02X}│" + "│".join(cells))
            else:
                lines.append("")
        return half, lines

    def close(self) -> None:
        if self._mod:
            lib().openmpt_module_destroy(self._mod)
            self._mod = None
