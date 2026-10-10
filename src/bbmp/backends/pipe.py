# SPDX-License-Identifier: GPL-3.0-or-later
"""Backends that run a helper program and read raw audio from its stdout.

The helpers speak the same protocol (see csrc/): bbmp-zxtune (ZXTune: Spectrum
trackers, the xSF family, V2), bbmp-vgm (S98, VGM of every chip), bbmp-chip
(YM, SC68, AdLib, MDX, PMD, FMP, MSX, Euphony, Organya, Piston Collage, QSF),
bbmp-psgplay (SNDH), bbmp-snsf (SNSF), bbmp-furnace
(Deflemask, FamiTracker), bbmp-xmp (libxmp: Acorn Archimedes trackers) and
bbmp-rv (Retrovert's plugins: iXalance, Klystrack), and helpers with replayers of
the player's own, such as bbmp-musicline. Line one of the output is JSON
describing the track, then s16le stereo follows. A separate process means an emulator that crashes or
hangs only ends that song.
"""

from __future__ import annotations

import json
import os
import select
import functools
import shutil
import subprocess
from pathlib import Path

from .. import downloads, extras, paths, sandbox
from .base import FRAME_BYTES, RATE, Decoder, Details, NeedsDownload, TrackInfo, Unsupported

STALL_SECONDS = 10   # a helper that produces nothing for this long is killed
MAX_LINE = 64 * 1024  # the helper's first line (JSON) is never longer


def sweep_leftovers(older_than: float = 3600) -> None:
    """Old helpers, killed before they could clean up, left their files in /tmp (which
    is often in memory). Today's leave nothing; this removes what older ones left."""
    import tempfile
    import time
    now = time.time()
    tmp = Path(tempfile.gettempdir())
    # the helper was called modland-furnace before the player had its name
    for d in [*tmp.glob("modland-furnace-*"), *tmp.glob("bbmp-furnace-*")]:
        try:
            if d.is_dir() and not d.is_symlink() and now - d.stat().st_mtime > older_than \
                    and d.stat().st_uid == os.getuid():
                shutil.rmtree(d, ignore_errors=True)
        except OSError:
            pass

def _plugin(name: str) -> str:
    """One of Retrovert's playback plugins, which bbmp-rv loads."""
    return str(paths.RV_PREFIX / "plugins" / f"{name}_playback.so")


# The engines of the player's own helpers. Each plays one format and refuses what it
# can't play right, with the reason.
OWN: list[str] = []


def _own(name: str, *engines: str) -> dict:
    """The helper bbmp-<name>, which plays one format, or one per engine it has."""
    exe, prefix = f"bbmp-{name}", paths.helper_prefix(name)
    found = {e: (exe, prefix, [e]) for e in engines} or {name: (exe, prefix, [])}
    OWN.extend(found)
    return found


HELPERS = {
    "zxtune": ("bbmp-zxtune", paths.ZXTUNE_PREFIX, []),
    "ym": ("bbmp-chip", paths.CHIP_PREFIX, ["ym"]),
    # psgplay (GPL 2 only) and snsf9x (Snes9x, non-commercial) can't be combined with GPL 3
    # code: helpers of their own since 7 Oct 2026 (bbmp-chip had them before)
    "sndh": ("bbmp-psgplay", paths.helper_prefix("psgplay"), []),
    "adplug": ("bbmp-chip", paths.CHIP_PREFIX, ["adplug"]),
    "mdx": ("bbmp-chip", paths.CHIP_PREFIX, ["mdx"]),
    "pmdc": ("bbmp-chip", paths.CHIP_PREFIX, ["pmdc"]),   # PMD on our own driver; the old "pmd" (pmdmini) is left out
    "sc68": ("bbmp-chip", paths.CHIP_PREFIX, ["sc68"]),
    "fmp": ("bbmp-chip", paths.CHIP_PREFIX, ["fmp"]),
    "kss": ("bbmp-chip", paths.CHIP_PREFIX, ["kss"]),
    "eup": ("bbmp-chip", paths.CHIP_PREFIX, ["eup"]),
    "organya": ("bbmp-chip", paths.CHIP_PREFIX, ["organya"]),
    "pxtone": ("bbmp-chip", paths.CHIP_PREFIX, ["pxtone"]),
    "qsf": ("bbmp-chip", paths.CHIP_PREFIX, ["qsf"]),
    "snsf": ("bbmp-snsf", paths.helper_prefix("snsf"), []),
    # VGM and S98 left ZXTune (libvgm's OPL4 is GPL 2 only) for a helper of its own on
    # 7 Oct 2026: the player's own VGM/S98 player with libvgm's licensed chip cores
    "vgm": ("bbmp-vgm", paths.helper_prefix("vgm"), []),
    "furnace": ("bbmp-furnace", paths.FURNACE_PREFIX, []),
    "xmp": ("bbmp-xmp", paths.XMP_PREFIX, []),
    "ixalance": ("bbmp-rv", paths.RV_PREFIX, ["ixalance"]),
    "klystrack": ("bbmp-rv", paths.RV_PREFIX, [_plugin("klystrack")]),
    **_own("musicline"),
    **_own("nez", "sgc", "gbr"),
    **_own("iigs", "soundsmith", "noisetracker"),
    **_own("console", "wsr", "spu"),
    **_own("fac"),
    **_own("misc", "edlib", "goattracker", "soundclub"),
    **_own("pcmusic", "monotone", "digitrekker"),
    **_own("adlib", "pixelpainters", "d01", "sig", "lem", "fmtracker", "samurai", "shadowlands", "vibrants"),
    **_own("psycle"),
    **_own("darkwave"),
    **_own("pc", "unitrk", "playerpro", "dsm", "mxm", "buzz"),
    **_own("synth", "protrekkr", "kdm", "jxs", "skale", "buz", "sbstudio"),
    **_own("amiga", "maxtrax", "zoundmonitor", "jesperolsen", "stonetracker"),
    **_own("retro", "sccmusixx", "ned", "picatune", "cybertracker", "mgb", "onyx", "wwsnd", "tss",
           "shroom", "victracker", "boyscout", "vgemm"),
    **_own("piece"),
    **_own("chipnsfx"),
    **_own("tiatracker"),
    **_own("synder"),
    **_own("t0ast"),
    **_own("berotracker"),
    **_own("axs"),
    **_own("ay", "beepola", "fls", "sks", "fxm", "amad", "vt2", "aks", "strc"),
}


def left_out(backend: str) -> str | None:
    """Why this package has no replayer for the backend, or None. A helper built for a
    package (BBMP_PUBLIC=1 in tools/sources.sh) lists any engine it left out in <its
    folder>/left-out.txt: a line of the engine, a tab and the reason."""
    if backend not in HELPERS:
        return None
    _, prefix, args = HELPERS[backend]
    engine = args[0] if args else backend
    if engine not in (listed := _left_out_list(prefix)):
        return None
    return listed[engine] or f"{backend} is not in this package"


@functools.lru_cache(maxsize=None)
def _left_out_list(prefix: Path) -> dict[str, str]:
    """A helper folder's left-out.txt, read once: the lists ask for every song's format."""
    try:
        text = (prefix / "left-out.txt").read_text(errors="replace")
    except OSError:
        return {}
    return {name: why.strip() for name, _, why in
            (line.partition("\t") for line in text.splitlines()) if name}


def helper(backend: str) -> list[str] | None:
    if left_out(backend):
        return None
    exe, prefix, args = HELPERS[backend]
    path = prefix / "bin" / exe
    found = str(path) if path.exists() else shutil.which(exe)
    if any(a.endswith(".so") and not Path(a).exists() for a in args):
        return None         # the helper is there and the plugin it needs is not
    return [found, *args] if found else None


def available(backend: str) -> bool:
    return helper(backend) is not None


def _text(value, limit: int = 2000) -> str:
    """Text from the helper: only text and numbers, and not without end."""
    return str(value)[:limit] if isinstance(value, (str, int, float)) else ""


_swept = False


class PipeDecoder(Decoder):
    def __init__(self, backend: str, path: Path):
        global _swept
        if not _swept:
            _swept = True
            sweep_leftovers()
        why = left_out(backend)
        if why:
            raise Unsupported(why)
        cmd = helper(backend)
        if cmd is None:
            raise Unsupported(f"{backend} helper not installed")
        needed = next((d for d in downloads.DOWNLOADS if d.id == backend and d.required), None)
        if needed is not None and not downloads.installed(needed):
            raise NeedsDownload(downloads.advice(backend))
        self.backend = backend
        self._cmd = cmd
        self._path = path
        self._proc: subprocess.Popen | None = None
        self._buf = b""
        self._meta: dict = {}
        self.info = TrackInfo()
        self._start(0, 0)

    def _start(self, track: int, start_ms: int) -> None:
        self._stop()
        env = {**extras.env(self.backend, self._cmd), **downloads.env(self.backend)}
        self._proc = sandbox.popen(
            sandbox.wrap([*self._cmd, str(self._path), str(track), str(RATE), str(start_ms)],
                         self._path, self._reads()), env=env,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
        self._buf = b""
        line = self._readline()
        try:
            meta = json.loads(line or "{}")
        except json.JSONDecodeError:
            meta = {}
        if not isinstance(meta, dict):
            meta = {}
        tracks = meta.get("tracks")
        if not isinstance(tracks, int) or tracks < 1 or "error" in meta:
            self._stop()
            said = _text(meta.get("error")) or f"{self.backend} can't play this file"
            advice = downloads.advice(self.backend, said)
            if advice:
                raise NeedsDownload(downloads.with_advice(said, advice))
            raise Unsupported(said)
        duration = meta.get("duration_ms")
        duration = duration / 1000 if isinstance(duration, (int, float)) and duration > 0 else 0
        self.info = TrackInfo(
            title=_text(meta.get("title")).strip(), author=_text(meta.get("author")).strip(),
            format=_text(meta.get("type")) or _text(meta.get("program")),
            duration=duration or None, subsongs=min(tracks, 10000), subsong=track,
            extra={k: v for k, v in (("program", _text(meta.get("program"))),) if v})
        self._meta = meta

    def _reads(self) -> list[Path]:
        """Files outside the usual places the helper reads: the ones the environment
        names (ROMs, sample sets)."""
        env = {**extras.env(self.backend, self._cmd), **downloads.env(self.backend)}
        return [Path(v) for k, v in env.items() if k.startswith("BBMP_") and v.startswith("/")]

    def _readline(self) -> str:
        """The helper's first line. What follows it (sound) is kept for read()."""
        out = bytearray()
        fd = self._proc.stdout.fileno()
        while b"\n" not in out and len(out) < MAX_LINE:
            ready, _, _ = select.select([fd], [], [], STALL_SECONDS)
            chunk = os.read(fd, 65536) if ready else b""
            if not chunk:
                break
            out += chunk
        line, newline, rest = bytes(out).partition(b"\n")
        if not newline:
            return ""                   # no line, or garbage too long to be one
        self._buf = rest
        return line.decode("utf-8", "replace").strip()

    def read(self, frames: int) -> bytes:
        want = frames * FRAME_BYTES
        self._fill(want)
        n = min(len(self._buf), want) // FRAME_BYTES * FRAME_BYTES
        out, self._buf = self._buf[:n], self._buf[n:]
        return out

    def _fill(self, want: int) -> None:
        if self._proc is None:
            return
        fd = self._proc.stdout.fileno()
        while len(self._buf) < want:
            ready, _, _ = select.select([fd], [], [], STALL_SECONDS)
            chunk = os.read(fd, max(65536, want)) if ready else b""
            if not chunk:
                self._stop()
                return
            self._buf += chunk

    def details(self) -> Details:
        d = super().details()
        facts = self._meta.get("facts")
        for label, value in list(facts.items() if isinstance(facts, dict) else ())[:100]:
            d.fact(_text(label, 100), _text(value))
        d.text("Comment", _text(self._meta.get("comment"), 20000).splitlines()[:1000])
        names = self._meta.get("names")
        d.text("Instrument names", [_text(n, 400) for n in
                                    (names if isinstance(names, list) else [])[:1000]])
        return d

    def select_subsong(self, n: int) -> None:
        self._start(n, 0)

    def seek(self, seconds: float) -> bool:
        self._start(self.info.subsong, int(max(0.0, seconds) * 1000))
        return True

    def _stop(self) -> None:
        if self._proc is not None:
            self._proc.kill()
            self._proc.stdout.close()
            self._proc.wait()
            self._proc = None

    def close(self) -> None:
        self._stop()
