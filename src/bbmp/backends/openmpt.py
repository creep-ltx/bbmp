# SPDX-License-Identifier: GPL-3.0-or-later
"""libopenmpt: Protracker, Fasttracker, Impulse Tracker, OctaMED and dozens more tracker formats."""

from __future__ import annotations

import ctypes as C
import os
import time
from array import array
from pathlib import Path

from .base import (FRAME_BYTES, RATE, WAVE_RATE, WAVE_SPAN, Decoder, Details, TrackInfo,
                   Unsupported, load_library)

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
        vp = C.c_void_p
        L.openmpt_module_get_position_seconds.restype = C.c_double
        L.openmpt_module_get_position_seconds.argtypes = [vp]
        L.openmpt_module_read_mono.restype = C.c_size_t
        L.openmpt_module_read_mono.argtypes = [vp, C.c_int32, C.c_size_t, vp]
        try:            # the extension, for the channel scopes: a library without it plays on
            L.openmpt_module_ext_create_from_memory.restype = vp
            L.openmpt_module_ext_create_from_memory.argtypes = [
                vp, C.c_size_t, vp, vp, vp, vp, C.POINTER(C.c_int), C.POINTER(C.c_char_p), vp]
            L.openmpt_module_ext_destroy.argtypes = [vp]
            L.openmpt_module_ext_get_module.restype = vp
            L.openmpt_module_ext_get_module.argtypes = [vp]
            L.openmpt_module_ext_get_interface.restype = C.c_int
            L.openmpt_module_ext_get_interface.argtypes = [vp, C.c_char_p, vp, C.c_size_t]
            L.has_ext = True
        except AttributeError:
            L.has_ext = False
        _lib = L
    return _lib


class _Interactive(C.Structure):
    """libopenmpt_ext.h's openmpt_module_ext_interface_interactive, in its order (0.8):
    only set_channel_mute_status is called."""
    _fields_ = [(name, C.c_void_p) for name in (
        "set_current_speed", "set_current_tempo", "set_tempo_factor", "get_tempo_factor",
        "set_pitch_factor", "get_pitch_factor", "set_global_volume", "get_global_volume",
        "set_channel_volume", "get_channel_volume")] + [
        ("set_channel_mute_status", C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_int32, C.c_int))] + [
        (name, C.c_void_p) for name in (
            "get_channel_mute_status", "set_instrument_mute_status",
            "get_instrument_mute_status", "play_note", "stop_note")]


SCOPES = 32                 # channel scopes shown at most (visuals.ChannelScopes)
SCOPES_MEMORY = 128 << 20   # what the copies for them may take together


def _resident() -> int | None:
    """Bytes of memory this process has, where the system says (Linux)."""
    try:
        with open("/proc/self/statm") as f:
            return int(f.read().split()[1]) * os.sysconf("SC_PAGE_SIZE")
    except (OSError, ValueError, IndexError):
        return None


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
        self._data = data
        self._rows: dict[int, list[str]] = {}         # pattern_rows, by pattern
        # the channel scopes: one copy of the song per channel, the others muted in it
        # (libopenmpt's interactive extension), played along with the song
        self._copies: list[tuple[int, int]] = []      # (ext handle, module)
        self._origin: float | None = None     # where the song was last put by a seek
        self._since = 0             # and the frames read from there
        self._behind = 0            # frames the copies have still to catch up
        self._wave_buf = C.create_string_buffer(0)
        self._waves: list[bytes] | None = None
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
        self._since += n
        if self._copies:
            if self._behind:
                self._behind = self._catch_up(self._behind)
            if self._behind:
                self._behind += n
                self._waves = None
            else:
                self._read_waves(n)
        return self._buf.raw[:n * FRAME_BYTES]

    CATCH_UP = 0.008        # seconds of each read the copies may take to catch up

    def _catch_up(self, frames: int) -> int:
        """Copies made after the song had begun play on to where it is, a little with
        each read so that the sound never waits: the frames left."""
        L, start = lib(), time.perf_counter()
        while frames and time.perf_counter() - start < self.CATCH_UP:
            step = min(frames, 4800)
            if len(self._wave_buf) < step * 2:
                self._wave_buf = C.create_string_buffer(step * 2)
            for _, mod in self._copies:
                L.openmpt_module_read_mono(mod, RATE, step, self._wave_buf)
            frames -= step
        return frames

    def _read_waves(self, frames: int) -> None:
        """Each copy as far on as the song, rendered as the song is (at RATE: at a lower
        rate an XM's channels came out different, 13 November 2026): the end of its
        channel's sound, every second sample."""
        if len(self._wave_buf) < frames * 2:
            self._wave_buf = C.create_string_buffer(frames * 2)
        step = RATE // WAVE_RATE
        waves = []
        for _, mod in self._copies:
            got = lib().openmpt_module_read_mono(mod, RATE, frames, self._wave_buf)
            tail = array("h", self._wave_buf.raw[max(0, got - WAVE_SPAN * step) * 2:got * 2])
            waves.append(tail[::step].tobytes())
        self._waves = waves

    @property
    def can_waves(self) -> bool:
        return lib().has_ext

    def want_waves(self, on: bool) -> None:
        """One copy of the song for each channel (up to SCOPES), all its other channels
        muted, kept as far on as the song. Their sounds add up to the song's (checked:
        0.0-0.1% apart). Each copy holds the song's samples: as many as SCOPES_MEMORY
        allows, at least 4 (13 November 2026: 26 copies of a 3.3 MB XM took 137 MB,
        and 0.8% of a core). Made at the song's start they keep in step with it to the
        end; made later they are put where the song is, which libopenmpt can do only
        roughly (a sound already playing is not heard in them until its next note)."""
        if not on:
            self._drop_copies()
            return
        if self._copies or not self.can_waves:
            return
        L = lib()
        silent = C.cast(L.openmpt_log_func_silent, C.c_void_p)
        want = min(self._channels, SCOPES)
        before = _resident()
        for c in range(self._channels):
            if len(self._copies) >= want:
                break
            ext = L.openmpt_module_ext_create_from_memory(
                self._data, len(self._data), silent, None, None, None, None, None, None)
            if not ext:
                break
            face = _Interactive()
            if not L.openmpt_module_ext_get_interface(ext, b"interactive", C.byref(face),
                                                      C.sizeof(face)):
                L.openmpt_module_ext_destroy(ext)
                break
            for other in range(self._channels):
                face.set_channel_mute_status(ext, other, int(other != c))
            mod = L.openmpt_module_ext_get_module(ext)
            L.openmpt_module_set_repeat_count(mod, -1)      # it ends when the song does
            self._copies.append((ext, mod))
            if c == 0:
                now = _resident()
                each = max(now - before if now and before else 0, 2 * len(self._data), 1 << 20)
                want = min(want, max(4, SCOPES_MEMORY // each))
        # from where the song was last put, to play on from there as it did: a copy put
        # straight where the song is now is only roughly in step (13 November 2026)
        self._place_copies()
        self._behind = self._since
        self._waves = None

    def _drop_copies(self) -> None:
        for ext, _ in self._copies:
            lib().openmpt_module_ext_destroy(ext)
        self._copies, self._waves = [], None

    def _place_copies(self) -> None:
        L = lib()
        for _, mod in self._copies:
            if self.info.subsongs > 1:
                L.openmpt_module_select_subsong(mod, self.info.subsong)
            if self._origin is not None:
                L.openmpt_module_set_position_seconds(mod, self._origin)

    def _moved(self, seconds: float | None) -> None:
        """The song was put somewhere (a seek, another subsong): the copies with it."""
        self._origin, self._since, self._behind, self._waves = seconds, 0, 0, None
        self._place_copies()

    def channel_waves(self) -> list[bytes] | None:
        return self._waves

    def loop_forever(self, on: bool) -> bool:
        lib().openmpt_module_set_repeat_count(self._mod, -1 if on else 0)
        return True

    def select_subsong(self, n: int) -> None:
        lib().openmpt_module_select_subsong(self._mod, n)
        self.info.subsong = n
        self.info.duration = lib().openmpt_module_get_duration_seconds(self._mod) or None
        self._moved(None)

    def seek(self, seconds: float) -> bool:
        lib().openmpt_module_set_position_seconds(self._mod, max(0.0, seconds))
        self._moved(max(0.0, seconds))
        return True

    def channel_levels(self) -> list[float]:
        f = lib().openmpt_module_get_current_channel_vu_mono
        return [min(1.0, f(self._mod, c)) for c in range(self._channels)]

    def pattern_view(self, rows: int) -> tuple[int, list[str]]:
        pat, row = self.position()
        lines = self.pattern_rows(pat)
        half = rows // 2
        return half, [lines[r] if 0 <= r < len(lines) else ""
                      for r in range(row - half, row - half + rows)]

    def position(self) -> tuple[int, int]:
        L = lib()
        return (L.openmpt_module_get_current_pattern(self._mod),
                L.openmpt_module_get_current_row(self._mod))

    def pattern_rows(self, pattern: int) -> list[str]:
        """Formatted once for each pattern: the view asked for 11 rows, 8 channels each,
        ten times a second (13 November 2026)."""
        lines = self._rows.get(pattern)
        if lines is None:
            L = lib()
            lines = []
            for r in range(max(0, L.openmpt_module_get_pattern_num_rows(self._mod, pattern))):
                cells = []
                for c in range(min(self._channels, 8)):
                    p = L.openmpt_module_format_pattern_row_channel(self._mod, pattern, r, c, 13, 1)
                    cells.append(C.string_at(p).decode("latin-1") if p else "")
                    if p:
                        L.openmpt_free_string(p)
                lines.append(f"{r:02X}│" + "│".join(cells))
            self._rows[pattern] = lines
        return lines

    def close(self) -> None:
        self._drop_copies()
        if self._mod:
            lib().openmpt_module_destroy(self._mod)
            self._mod = None
