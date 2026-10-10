# SPDX-License-Identifier: GPL-3.0-or-later
"""libsidplayfp: Commodore 64 SID tunes (HVSC, Sidplayer .mus).

libsidplayfp only has a C++ API, so csrc/sidshim.cpp wraps it in a few C
functions. The shim is compiled on first use with g++ into the data dir.
"""

from __future__ import annotations

import ctypes as C
import os
import subprocess
import sys
import threading
from pathlib import Path

from .. import extras, paths
from ..ending import DEFAULT_LENGTH
from .base import FRAME_BYTES, RATE, Decoder, TrackInfo, Unsupported

SHIM_SRC = Path(__file__).resolve().parents[3] / "csrc" / "sidshim.cpp"
SHIM_LIB = paths.NATIVE_DIR / f"libsidshim{paths.SO}"

_lib = None


def shim_command(out: Path) -> list[str]:
    """How the shim is compiled: g++ on Linux; on macOS Apple's compiler, with
    libsidplayfp found where Homebrew put it (pkg-config)."""
    if sys.platform != "darwin":
        return ["g++", "-O2", "-shared", "-fPIC", "-o", str(out), str(SHIM_SRC), "-lsidplayfp"]
    env = dict(os.environ)       # tools/build-sidplayfp.sh's, with reSIDfp (Homebrew's has none)
    env["PKG_CONFIG_PATH"] = ":".join(p for p in (str(paths.OPT / "sidplayfp/lib/pkgconfig"),
                                                  env.get("PKG_CONFIG_PATH", "")) if p)
    flags = subprocess.run(["pkg-config", "--cflags", "--libs", "libsidplayfp"],
                           capture_output=True, text=True, env=env).stdout.split()
    return ["c++", "-std=c++17", "-O2", "-dynamiclib", "-o", str(out), str(SHIM_SRC),
            *(flags or ["-lsidplayfp"])]


def build_shim() -> Path:
    built = paths.OPT / "sidshim" / f"libsidshim{paths.SO}"    # a package's, made when it was built
    if not SHIM_SRC.exists() and built.exists():
        return built
    stale = not SHIM_LIB.exists() or (SHIM_SRC.exists()
                                      and SHIM_SRC.stat().st_mtime > SHIM_LIB.stat().st_mtime)
    if stale:
        SHIM_LIB.parent.mkdir(parents=True, exist_ok=True)
        tmp = SHIM_LIB.with_suffix(f".{os.getpid()}.{threading.get_ident()}.tmp")
        done = subprocess.run(shim_command(tmp), capture_output=True, text=True)
        if done.returncode != 0:     # say why: the compiler's last words
            raise OSError("can't build the SID shim: "
                          + " ".join(done.stderr.strip().splitlines()[-3:]))
        os.replace(tmp, SHIM_LIB)
    return SHIM_LIB


def lib():
    global _lib
    if _lib is None:
        try:
            L = C.CDLL(str(build_shim()))
        except (subprocess.CalledProcessError, FileNotFoundError) as e:
            raise OSError(f"can't build the SID shim: {e}") from None
        L.sid_open.restype = C.c_void_p
        L.sid_open.argtypes = [C.c_char_p, C.c_uint]
        L.sid_select.argtypes = [C.c_void_p, C.c_uint]
        L.sid_songs.restype = C.c_uint
        L.sid_songs.argtypes = [C.c_void_p]
        L.sid_start_song.restype = C.c_uint
        L.sid_start_song.argtypes = [C.c_void_p]
        L.sid_info_string.restype = C.c_char_p
        L.sid_info_string.argtypes = [C.c_void_p, C.c_uint]
        L.sid_format.restype = C.c_char_p
        L.sid_format.argtypes = [C.c_void_p]
        L.sid_render.argtypes = [C.c_void_p, C.c_void_p, C.c_int]
        L.sid_skip.restype = C.c_long
        L.sid_skip.argtypes = [C.c_void_p, C.c_long]
        L.sid_close.argtypes = [C.c_void_p]
        L.sid_roms.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p]
        L.sid_roms.restype = None
        _lib = L
    return _lib


_roms: tuple | None = None


def use_roms() -> None:
    """Give libsidplayfp the C64's ROMs, if the user has put them into the extras
    folder. Tunes written in BASIC are silent without them."""
    global _roms
    found = tuple(extras.find(id_) for id_ in ("c64kernal", "c64basic", "c64chargen"))
    if found == _roms:
        return
    _roms = found
    data = [p.read_bytes() if p else None for p in found]
    lib().sid_roms(*data)


def _s(b: bytes | None) -> str:
    return b.decode("latin-1").strip() if b else ""


class SID(Decoder):
    backend = "sidplayfp"

    def __init__(self, path: Path, lengths: list[float] = ()):
        L = lib()
        use_roms()
        self._h = L.sid_open(os.fsencode(path), RATE)
        if not self._h:
            raise Unsupported("not a SID tune libsidplayfp can load")
        self._lengths = list(lengths)
        self._buf = C.create_string_buffer(0)
        self.info = TrackInfo(
            title=_s(L.sid_info_string(self._h, 0)), author=_s(L.sid_info_string(self._h, 1)),
            format=_s(L.sid_format(self._h)), subsongs=max(1, L.sid_songs(self._h)),
            extra={"released": _s(L.sid_info_string(self._h, 2))})
        self.select_subsong(max(0, L.sid_start_song(self._h) - 1))

    def select_subsong(self, n: int) -> None:
        if not lib().sid_select(self._h, n + 1):
            raise Unsupported("libsidplayfp couldn't start the tune")
        self.info.subsong = n
        self._pos = 0
        known = self._lengths[n] if n < len(self._lengths) else 0
        # SID tunes loop forever; the player stops them at this length.
        self.info.duration = known or DEFAULT_LENGTH

    def seek(self, seconds: float) -> bool:
        """The emulated C64 runs at full speed to the position (backwards: from the start)."""
        target = int(max(0.0, seconds) * RATE)
        if target < self._pos:
            if not lib().sid_select(self._h, self.info.subsong + 1):
                return False
            self._pos = 0
        self._pos += lib().sid_skip(self._h, target - self._pos)
        return True

    def read(self, frames: int) -> bytes:
        if len(self._buf) < frames * FRAME_BYTES:
            self._buf = C.create_string_buffer(frames * FRAME_BYTES)
        n = lib().sid_render(self._h, self._buf, frames)
        self._pos += n
        return self._buf.raw[:n * FRAME_BYTES]

    def close(self) -> None:
        if self._h:
            lib().sid_close(self._h)
            self._h = None
