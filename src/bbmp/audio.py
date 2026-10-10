# SPDX-License-Identifier: GPL-3.0-or-later
"""Sound output: through PulseAudio's simple API on Linux (PipeWire provides it too), and
through libbbmp-out (miniaudio: CoreAudio) on macOS. BBMP_AUDIO chooses another: "pulse",
"miniaudio", or "silent" (miniaudio's null device: the whole path, and nothing heard)."""

from __future__ import annotations

import ctypes as C
import ctypes.util
import sys
from pathlib import Path

from . import paths
from .backends.base import FRAME_BYTES, RATE

PA_STREAM_PLAYBACK = 1
PA_SAMPLE_S16LE = 3


class _SampleSpec(C.Structure):
    _fields_ = [("format", C.c_int), ("rate", C.c_uint32), ("channels", C.c_uint8)]


class _BufferAttr(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ("maxlength", "tlength", "prebuf", "minreq", "fragsize")]


_lib = None


def lib():
    global _lib
    if _lib is None:
        name = ctypes.util.find_library("pulse-simple") or "libpulse-simple.so.0"
        L = C.CDLL(name)
        L.pa_simple_new.restype = C.c_void_p
        L.pa_simple_new.argtypes = [C.c_char_p, C.c_char_p, C.c_int, C.c_char_p, C.c_char_p,
                                    C.POINTER(_SampleSpec), C.c_void_p, C.POINTER(_BufferAttr),
                                    C.POINTER(C.c_int)]
        L.pa_simple_write.argtypes = [C.c_void_p, C.c_char_p, C.c_size_t, C.POINTER(C.c_int)]
        L.pa_simple_flush.argtypes = [C.c_void_p, C.POINTER(C.c_int)]
        L.pa_simple_drain.argtypes = [C.c_void_p, C.POINTER(C.c_int)]
        L.pa_simple_get_latency.restype = C.c_uint64
        L.pa_simple_get_latency.argtypes = [C.c_void_p, C.POINTER(C.c_int)]
        L.pa_simple_free.argtypes = [C.c_void_p]
        _lib = L
    return _lib


class AudioError(Exception):
    pass


class PulseOutput:
    """A blocking stereo 16-bit stream. Pause and skip empty it, so they stay quick.

    PipeWire's pulse server keeps about a third of the length asked for: 150 ms gave
    about 50 ms, which a busy screen or a pause to collect garbage ran dry (choppy sound,
    measured 1 October 2026); 400 gives about 200."""

    def __init__(self, latency_ms: int = 400):
        spec = _SampleSpec(PA_SAMPLE_S16LE, RATE, 2)
        tlength = RATE * FRAME_BYTES * latency_ms // 1000
        attr = _BufferAttr(0xFFFFFFFF, tlength, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF)
        err = C.c_int()
        self._pa = lib().pa_simple_new(None, b"ByteBandit Music Player", PA_STREAM_PLAYBACK, None, b"Music",
                                       C.byref(spec), None, C.byref(attr), C.byref(err))
        if not self._pa:
            raise AudioError(f"can't open PulseAudio/PipeWire output (error {err.value})")

    def write(self, pcm: bytes) -> None:
        err = C.c_int()
        if lib().pa_simple_write(self._pa, pcm, len(pcm), C.byref(err)) < 0:
            raise AudioError(f"audio write failed (error {err.value})")

    def latency(self) -> float:
        """Seconds between what we wrote and what is being heard."""
        err = C.c_int()
        return lib().pa_simple_get_latency(self._pa, C.byref(err)) / 1e6

    def flush(self) -> None:
        lib().pa_simple_flush(self._pa, None)

    def drain(self) -> None:
        lib().pa_simple_drain(self._pa, None)

    def close(self) -> None:
        if self._pa:
            lib().pa_simple_free(self._pa)
            self._pa = None


_out_lib = None


def out_library() -> Path:
    """Where libbbmp-out is (tools/build-out.sh)."""
    name = "libbbmp-out.dylib" if sys.platform == "darwin" else "libbbmp-out.so"
    return paths.helper_prefix("out") / "lib" / name


def out_lib():
    global _out_lib
    if _out_lib is None:
        try:
            L = C.CDLL(str(out_library()))
        except OSError as e:
            raise AudioError(f"the sound library isn't there ({out_library()}): {e}") from None
        L.bbmp_out_open.restype = C.c_void_p
        L.bbmp_out_open.argtypes = [C.c_uint, C.c_uint, C.c_int]
        L.bbmp_out_write.argtypes = [C.c_void_p, C.c_char_p, C.c_size_t]
        L.bbmp_out_latency.restype = C.c_double
        L.bbmp_out_latency.argtypes = [C.c_void_p]
        for f in ("bbmp_out_flush", "bbmp_out_drain", "bbmp_out_close"):
            getattr(L, f).argtypes = [C.c_void_p]
        L.bbmp_out_error.restype = C.c_char_p
        _out_lib = L
    return _out_lib


class MiniaudioOutput:
    """The same as PulseOutput, through libbbmp-out. `silent`: miniaudio's null device,
    which takes the sound at the speed it would be heard and plays nothing."""

    def __init__(self, latency_ms: int = 300, silent: bool = False):
        self._h = out_lib().bbmp_out_open(RATE, latency_ms, int(silent))
        if not self._h:
            raise AudioError(out_lib().bbmp_out_error().decode(errors="replace"))

    def write(self, pcm: bytes) -> None:
        if out_lib().bbmp_out_write(self._h, pcm, len(pcm)) < 0:
            raise AudioError("audio write failed")

    def latency(self) -> float:
        return out_lib().bbmp_out_latency(self._h)

    def flush(self) -> None:
        out_lib().bbmp_out_flush(self._h)

    def drain(self) -> None:
        out_lib().bbmp_out_drain(self._h)

    def close(self) -> None:
        if self._h:
            out_lib().bbmp_out_close(self._h)
            self._h = None


def open_output():
    """The sound output this system has (see the top of this file)."""
    how = paths.env("AUDIO") or ("miniaudio" if sys.platform == "darwin" else "pulse")
    if how == "silent":
        return MiniaudioOutput(silent=True)
    if how == "miniaudio":
        return MiniaudioOutput()
    return PulseOutput()


class NullOutput:
    """Discards audio at real-time speed; used by tests and when there is no sound server."""

    def __init__(self, realtime: bool = False):
        self.realtime = realtime
        self.written = 0

    def write(self, pcm: bytes) -> None:
        self.written += len(pcm)
        if self.realtime:
            import time
            time.sleep(len(pcm) / (RATE * FRAME_BYTES))

    def latency(self) -> float:
        return 0.0

    def flush(self) -> None:
        pass

    def drain(self) -> None:
        pass

    def close(self) -> None:
        pass
