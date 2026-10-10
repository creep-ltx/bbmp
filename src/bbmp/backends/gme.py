# SPDX-License-Identifier: GPL-3.0-or-later
"""Game Music Emu: NES, SNES SPC, Game Boy, PC Engine, MSX KSS, Atari SAP, Sega VGM/GYM, Spectrum AY."""

from __future__ import annotations

import bz2
import ctypes as C
import struct
import zlib
from pathlib import Path

from .. import paths
from .base import FRAME_BYTES, RATE, Decoder, Details, TrackInfo, Unsupported, load_library


class _Info(C.Structure):
    _fields_ = ([(n, C.c_int) for n in ("length", "intro_length", "loop_length", "play_length",
                                         "fade_length")]
                + [(f"i{n}", C.c_int) for n in range(5, 16)]
                + [(n, C.c_char_p) for n in ("system", "game", "song", "author", "copyright",
                                              "comment", "dumper")]
                + [(f"s{n}", C.c_char_p) for n in range(7, 16)])


_lib = None


def lib():
    global _lib
    if _lib is None:
        L = load_library("gme", paths=(paths.GME_PREFIX / "lib/libgme.so.0",
                                       paths.GME_PREFIX / "lib/libgme.dylib"))
        L.gme_open_data.restype = C.c_char_p
        L.gme_open_data.argtypes = [C.c_void_p, C.c_long, C.POINTER(C.c_void_p), C.c_int]
        L.gme_start_track.restype = C.c_char_p
        L.gme_start_track.argtypes = [C.c_void_p, C.c_int]
        L.gme_play.restype = C.c_char_p
        L.gme_play.argtypes = [C.c_void_p, C.c_int, C.c_void_p]
        L.gme_track_ended.argtypes = [C.c_void_p]
        L.gme_track_count.argtypes = [C.c_void_p]
        L.gme_track_info.restype = C.c_char_p
        L.gme_track_info.argtypes = [C.c_void_p, C.POINTER(C.POINTER(_Info)), C.c_int]
        L.gme_free_info.argtypes = [C.POINTER(_Info)]
        L.gme_set_fade.argtypes = [C.c_void_p, C.c_int]
        L.gme_seek.restype = C.c_char_p
        L.gme_seek.argtypes = [C.c_void_p, C.c_int]
        L.gme_delete.argtypes = [C.c_void_p]
        _lib = L
    return _lib


_GYMX_HEADER = 428   # "GYMX", 5 x 32-byte strings, 256-byte comment, loop start, packed size


def _unpack_gym(data: bytes) -> bytes:
    """gme can't read packed GYM files, so unpack them first: zlib, or EZPK."""
    if data[:4] != b"GYMX" or len(data) < _GYMX_HEADER:
        return data
    (packed,) = struct.unpack_from("<I", data, _GYMX_HEADER - 4)
    if not packed:
        return data
    try:
        if data[_GYMX_HEADER:_GYMX_HEADER + 4] == b"EZPK":
            body = _unpack_ezpk(data[_GYMX_HEADER:])
        else:
            body = zlib.decompress(data[_GYMX_HEADER:])
    except (zlib.error, OSError, ValueError, IndexError, EOFError, struct.error,
            StopIteration):     # a damaged file: gme is given it as it is, and refuses it
        return data
    return data[:_GYMX_HEADER - 4] + b"\0\0\0\0" + body


# EZPK (Devil Crash, Super Shinobi 2 beta: 35 of Modland's GYM files) is bzip2 in
# disguise: "EZPK", its length, then a bzip2 stream whose "BZh" is three spaces and
# whose two 48-bit magics (block, end) have 5 added to each byte. What it holds is the
# song split into five streams (the commands, the registers, the data for each port,
# the PSG's data), after a header of nine u32: their counts, three zeros, the total.
_BLOCK, _END = 0x314159265359, 0x177245385090


def _disguised(magic: int) -> str:
    return format(int.from_bytes(bytes((b + 5) & 255 for b in magic.to_bytes(6, "big")), "big"),
                  "048b")


def _unpack_ezpk(data: bytes) -> bytes:
    (total,) = struct.unpack_from("<I", data, 4)
    packed = data[8:total] if 8 < total <= len(data) else data[8:]
    level = packed[3:4] if packed[3:4].isdigit() else b"9"
    # the magics sit anywhere in the bit stream; a disguised one found by chance in
    # the data would fail bzip2's checksum, not give a wrong song
    bits = format(int.from_bytes(packed[4:], "big"), f"0{(len(packed) - 4) * 8}b")
    bits = (bits.replace(_disguised(_BLOCK), format(_BLOCK, "048b"))
                .replace(_disguised(_END), format(_END, "048b")))
    bits += "0" * (-len(bits) % 8)
    raw = bz2.decompress(b"BZh" + level + int(bits, 2).to_bytes(len(bits) // 8, "big"))
    counts = struct.unpack_from("<9I", raw)
    if counts[8] != len(raw) or sum(counts[:8]) + 36 != len(raw):
        raise ValueError("EZPK: its counts don't match")
    streams, at = [], 36
    for n in counts[:5]:
        streams.append(iter(raw[at:at + n]))
        at += n
    commands, registers, port0, port1, psg = streams
    out = bytearray()
    for c in commands:
        if c == 0:
            out.append(0)
        elif c in (1, 2):
            out += bytes((c, next(registers), next(port0 if c == 1 else port1)))
        elif c == 3:
            out += bytes((3, next(psg)))
        else:
            raise ValueError("EZPK: not a GYM command")
    return bytes(out)


def _s(b: bytes | None) -> str:
    return b.decode("utf-8", "replace").strip() if b else ""


def _first_track(data: bytes, tracks: int) -> int:
    """The track a file says its music starts at, which Game Music Emu doesn't use: a
    HES rip's start song (Bloody Wolf: 42; its track 0 is a click)."""
    if data[:4] == b"HESM" and len(data) > 5 and data[5] < tracks:
        return data[5]
    return 0


class GME(Decoder):
    backend = "gme"
    FADE_MS = 8000

    def __init__(self, path: Path):
        L = lib()
        data = _unpack_gym(path.read_bytes())
        emu = C.c_void_p()
        err = L.gme_open_data(data, len(data), C.byref(emu), RATE)
        if err or not emu:
            raise Unsupported(_s(err) or "gme can't open file")
        self._emu = emu
        self._buf = C.create_string_buffer(0)
        self._comment = ""
        self.info = TrackInfo(subsongs=max(1, L.gme_track_count(emu)))
        self.select_subsong(_first_track(data, self.info.subsongs))

    def select_subsong(self, n: int) -> None:
        L = lib()
        err = L.gme_start_track(self._emu, n)
        if err:
            raise Unsupported(_s(err))
        p = C.POINTER(_Info)()
        if not L.gme_track_info(self._emu, C.byref(p), n) and p:
            i = p.contents
            self.info.title = _s(i.song) or _s(i.game)
            self.info.author = _s(i.author)
            self.info.format = _s(i.system)
            self.info.extra = {k: v for k, v in (("game", _s(i.game)), ("copyright", _s(i.copyright)),
                                                   ("ripped by", _s(i.dumper))) if v}
            self._comment = _s(i.comment)
            # the length the file says; else once through the loop (gme would play it
            # twice); else 3 minutes, as songs that never end (ending.py)
            if i.length > 0:
                length = i.length
            elif i.loop_length > 0:
                length = max(0, i.intro_length) + i.loop_length
            else:
                length = 180_000
            L.gme_free_info(p)
            L.gme_set_fade(self._emu, length)
            self._length = length
            self.info.duration = (length + self.FADE_MS) / 1000
        self.info.subsong = n

    def loop_forever(self, on: bool) -> bool:
        # a fade that starts before the song does is no fade: it plays on
        lib().gme_set_fade(self._emu, -1 if on else getattr(self, "_length", 150_000))
        return True

    def details(self) -> Details:
        d = super().details()
        d.text("Comment", self._comment.splitlines())
        return d

    def read(self, frames: int) -> bytes:
        L = lib()
        if L.gme_track_ended(self._emu):
            return b""
        if len(self._buf) < frames * FRAME_BYTES:
            self._buf = C.create_string_buffer(frames * FRAME_BYTES)
        if L.gme_play(self._emu, frames * 2, self._buf):
            return b""
        return self._buf.raw[:frames * FRAME_BYTES]

    def seek(self, seconds: float) -> bool:
        return not lib().gme_seek(self._emu, int(max(0.0, seconds) * 1000))

    def close(self) -> None:
        if self._emu:
            lib().gme_delete(self._emu)
            self._emu = None
