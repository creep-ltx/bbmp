# SPDX-License-Identifier: GPL-3.0-or-later
"""What every replayer backend looks like to the player."""

from __future__ import annotations

import ctypes
import ctypes.util
import sys
from dataclasses import dataclass, field
from pathlib import Path

from .. import paths as _paths

RATE = 48000          # every backend renders at this rate, 16-bit signed stereo
FRAME_BYTES = 4


class Unsupported(Exception):
    """This backend can't play the file."""


class NeedsDownload(Unsupported):
    """The song needs a part the player downloads on request (downloads.py), and it isn't
    there. The message says which, and how to get it. No other replayer is tried: with
    the part, this one plays the song."""


@dataclass
class TrackInfo:
    title: str = ""
    author: str = ""
    format: str = ""                 # the replayer's own name for the format
    duration: float | None = None    # seconds, None when the song doesn't say
    subsongs: int = 1
    subsong: int = 0                 # 0-based
    extra: dict[str, str] = field(default_factory=dict)


@dataclass
class Details:
    """What a file says about itself, shown on the Info tab exactly as it is in the file."""
    facts: list[tuple[str, str]] = field(default_factory=list)          # (label, value)
    texts: list[tuple[str, list[str]]] = field(default_factory=list)    # (heading, lines)

    def fact(self, label: str, value) -> None:
        value = str(value or "").strip()
        if value:
            self.facts.append((label, value))

    def text(self, heading: str, lines: list[str]) -> None:
        lines = [ln.rstrip() for ln in lines]
        while lines and not lines[-1]:
            lines.pop()
        if any(lines):
            self.texts.append((heading, lines))


class Decoder:
    """One loaded song. Not thread-safe: only the player thread touches it."""

    backend = "?"
    info: TrackInfo
    never_ends = False      # this song never ends by itself (the player stops it, ending.limit)

    def read(self, frames: int) -> bytes:
        """Up to `frames` stereo frames of int16 audio; b'' when the song has ended."""
        raise NotImplementedError

    def select_subsong(self, n: int) -> None:
        raise Unsupported("no subsongs")

    def seek(self, seconds: float) -> bool:
        """Jump to an absolute position; False if the backend can't."""
        return False

    def channel_levels(self) -> list[float] | None:
        """Per-channel levels 0..1 for the visualiser, if the backend knows them."""
        return None

    def details(self) -> Details:
        """Facts and texts from the file; the default is what TrackInfo already has."""
        d = Details()
        d.fact("Title", self.info.title)
        d.fact("Author", self.info.author)
        d.fact("Format", self.info.format)
        for k, v in self.info.extra.items():
            d.fact(k.capitalize(), v)
        return d

    def loop_forever(self, on: bool) -> bool:
        """Keep the song going round and round (the player's "on and on"), or end it as
        normal. False when the replayer can't: then the player starts it again when it
        ends."""
        return False

    def pattern_view(self, rows: int) -> tuple[int, list[str]] | None:
        """(current row index in the list, formatted pattern lines) for tracker formats."""
        return None

    def close(self) -> None:
        pass


def load_library(*names: str, paths: tuple[Path, ...] = ()) -> ctypes.CDLL:
    # a package's own copies first (the Mac one keeps them in opt/lib: a sandboxed
    # process isn't given DYLD_* to find them by)
    paths = (*paths, *(_paths.OPT / "lib" / f"lib{n}{_paths.SO}" for n in names))
    if sys.platform == "darwin":        # and Homebrew's, which a sandboxed process isn't told of
        paths += tuple(Path(b) / "lib" / f"lib{n}.dylib" for b in ("/opt/homebrew", "/usr/local")
                       for n in names)
    for p in paths:
        if p.exists():
            return ctypes.CDLL(str(p))
    for n in names:
        found = ctypes.util.find_library(n)
        if found:
            return ctypes.CDLL(found)
        try:
            return ctypes.CDLL(f"lib{n}{_paths.SO}")
        except OSError:
            pass
    raise OSError(f"library not found: {', '.join(names)}")
