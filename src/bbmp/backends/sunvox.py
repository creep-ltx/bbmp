# SPDX-License-Identifier: GPL-3.0-or-later
"""SunVox projects through the SunVox library (MIT, prebuilt by its author).

tools/install-sunvox.sh puts sunvox.so in ~/.local/opt/sunvox.
"""

from __future__ import annotations

import bisect
import ctypes as C
import os
from pathlib import Path

import numpy as np

from .. import paths
from .base import RATE, Decoder, TrackInfo, Unsupported

LIB = Path(paths.env("SUNVOX_LIB") or paths.OPT / f"sunvox/sunvox{paths.SO}")

# sv_init flags
NO_DEBUG_OUTPUT, OFFLINE, AUDIO_INT16, AUDIO_FLOAT32, ONE_THREAD = 1, 1 << 1, 1 << 2, 1 << 3, 1 << 4
CEILING = 0.97          # of full scale: what the limiter keeps the sound under
RELEASE_DB = 3.0        # a second: how fast the limiter lets go again
TIME_MAP_FRAMES = 1     # sv_get_time_map: the frame each line starts at
MAX_MAPPED_LINES = 1 << 20   # a million lines: hours of music, 4 MB of map

_lib = None


def lib():
    global _lib
    if _lib is None:
        if not LIB.exists():
            raise OSError(f"SunVox library missing ({LIB}); run tools/install-sunvox.sh")
        L = C.CDLL(str(LIB))
        L.sv_init.argtypes = [C.c_char_p, C.c_int, C.c_int, C.c_uint32]
        L.sv_audio_callback.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_uint32]
        L.sv_get_ticks.restype = C.c_uint32
        L.sv_load.argtypes = [C.c_int, C.c_char_p]
        L.sv_get_song_name.restype = C.c_char_p
        L.sv_get_song_length_frames.restype = C.c_uint32
        L.sv_get_song_length_lines.restype = C.c_uint32
        L.sv_get_time_map.argtypes = [C.c_int, C.c_int, C.c_int, C.POINTER(C.c_uint32), C.c_int]
        L.sv_rewind.argtypes = [C.c_int, C.c_int]
        # Floats: many projects go over full scale, which in 16 bits is clipped. From
        # floats a limiter brings the peaks down without the crackle.
        if L.sv_init(None, RATE, 2, NO_DEBUG_OUTPUT | OFFLINE | AUDIO_FLOAT32 | ONE_THREAD) < 0:
            raise OSError("SunVox failed to initialise")
        _lib = L
    return _lib


class SunVox(Decoder):
    backend = "sunvox"

    def __init__(self, path: Path):
        if path.suffix.lower() != ".sunvox":   # the library also loads XM/MOD; leave those to openmpt
            raise Unsupported("not a SunVox project")
        L = lib()
        L.sv_open_slot(0)
        if L.sv_load(0, os.fsencode(path)) < 0:
            L.sv_close_slot(0)
            raise Unsupported("SunVox can't load this project")
        L.sv_volume(0, 256)
        L.sv_set_autostop(0, 1)
        L.sv_play_from_beginning(0)
        name = L.sv_get_song_name(0)
        frames = L.sv_get_song_length_frames(0)
        self.info = TrackInfo(title=(name or b"").decode("utf-8", "replace").strip(),
                              format="SunVox", duration=frames / RATE or None)
        self._buf = C.create_string_buffer(0)
        self._open = True
        self._limit = 1.0           # what the limiter takes off, as a factor
        # where each line of the project starts, in frames, for seeking
        # (a damaged project can say it has billions of lines: that one isn't mapped)
        lines = L.sv_get_song_length_lines(0)
        self._line_starts = None
        if 0 < lines <= MAX_MAPPED_LINES:
            self._line_starts = (C.c_uint32 * lines)()
            if L.sv_get_time_map(0, 0, lines, self._line_starts, TIME_MAP_FRAMES) < 0:
                self._line_starts = None

    def read(self, frames: int) -> bytes:
        L = lib()
        if not self._open or L.sv_end_of_song(0):
            return b""
        size = frames * 2 * 4                   # stereo floats
        if len(self._buf) < size:
            self._buf = C.create_string_buffer(size)
        L.sv_audio_callback(self._buf, frames, 0, L.sv_get_ticks())
        sound = np.frombuffer(self._buf.raw[:size], dtype="<f4")
        peak = float(np.abs(sound).max()) if len(sound) else 0.0
        needed = min(1.0, CEILING / peak) if peak > 0 else 1.0
        if needed < self._limit:
            self._limit = needed                                    # down at once
            ramp = np.full(len(sound), needed, dtype=np.float32)
        else:                                                       # and up slowly
            back = min(needed, self._limit * 10 ** (RELEASE_DB * frames / RATE / 20))
            ramp = np.linspace(self._limit, back, len(sound), dtype=np.float32)
            self._limit = back
        return np.clip(np.rint(sound * ramp * 32767), -32768, 32767).astype("<i2").tobytes()

    def seek(self, seconds: float) -> bool:
        """SunVox jumps to a line of the project: the last one that starts before the position."""
        if not self._open or self._line_starts is None:
            return False
        target = int(max(0.0, seconds) * RATE)
        line = max(0, bisect.bisect_right(self._line_starts, target) - 1)
        if lib().sv_rewind(0, line) < 0:
            return False
        if lib().sv_end_of_song(0):      # a seek after the end has stopped it
            lib().sv_play(0)
        return True

    def close(self) -> None:
        if self._open:
            lib().sv_stop(0)
            lib().sv_close_slot(0)
            self._open = False
