# SPDX-License-Identifier: GPL-3.0-or-later
""""This sounds wrong": a list of songs that the listener thinks play badly.

The "!" key notes the song that plays, where in it the listener is, which replayer
plays it, and a word or two, in a plain text file. A tester sends that file; `bbmp
wrong` says where it is and shows it. Nothing is sent anywhere by the player.
"""

from __future__ import annotations

import time
from pathlib import Path

from . import __version__, paths

FILE = paths.DATA_DIR / "sounds-wrong.txt"


def clock(seconds: float) -> str:
    seconds = max(0, int(seconds))
    return f"{seconds // 60}:{seconds % 60:02d}"


def note(song, words: str = "", *, at: float = 0.0, subsong: int = 0, subsongs: int = 1,
         replayer: str = "", file: Path | None = None, when: float | None = None) -> str:
    """Add the song to the list; gives what was written. `subsong` counts from 0."""
    file = file or FILE
    stamp = time.strftime("%Y-%m-%d %H:%M", time.localtime(when))
    where = f"at {clock(at)}"
    if subsongs > 1:
        where = f"subsong {subsong + 1} of {subsongs}, {where}"
    facts = [f"format {song.format}"] + ([f"replayer {replayer}"] if replayer else [])
    facts += [where, f"bbmp {__version__}"]
    words = " ".join(words.split())
    entry = (f"{stamp}  {song.key}\n  {', '.join(facts)}\n"
             + (f'  "{words}"\n' if words else "") + "\n")
    file.parent.mkdir(parents=True, exist_ok=True)
    with open(file, "a", encoding="utf-8") as out:
        out.write(entry)
    return entry


def count(file: Path | None = None) -> int:
    """How many songs are on the list."""
    file = file or FILE
    try:
        text = file.read_text(encoding="utf-8")
    except OSError:
        return 0
    return sum(1 for line in text.splitlines() if line and not line.startswith(" "))
