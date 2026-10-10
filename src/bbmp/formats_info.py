# SPDX-License-Identifier: GPL-3.0-or-later
"""What the player tells about a format: how well it plays, and what is known to be
wrong with it or why it can't be played.

The facts are in format_notes.py, which is generated (tools/format_status.py) from a
census of each format's songs and from notes on what was checked.
"""

from __future__ import annotations

from dataclasses import dataclass

from . import downloads, extras
from .backends import backend_for, is_supported, left_out

try:
    from .format_notes import NOTES
except ImportError:          # not generated yet
    NOTES = {}

TITLES = {
    "verified": "Verified against the original",
    "fine": "Plays, no fault known",
    "faults": "Plays with faults or gaps",
    "needs": "Needs a file",
    "off": "Switched off",
    "no": "Doesn't play",
}
MARKS = {"verified": "✓", "fine": "", "faults": "!", "needs": "+", "off": "✗", "no": "✗"}


@dataclass(frozen=True)
class FormatInfo:
    format: str
    group: str        # a key of TITLES
    plays: str        # "815 of 817", "38 of 40 tried", or ""
    text: str         # what was checked, what is wrong, why it doesn't play
    backend: str
    files: str = ""   # the files the format uses besides the song: which, and whether they are there
    files_short: str = ""   # the same in a line where all are there; in full for what is missing

    @property
    def title(self) -> str:
        return TITLES[self.group]

    @property
    def mark(self) -> str:
        return MARKS[self.group]

    @property
    def counted(self) -> str:
        """"815 of 817 songs play", where songs play and were counted."""
        if not self.plays or self.group in ("off", "no"):
            return ""
        return f"{self.plays} songs play" + (" with it" if self.group == "needs" else "")

    @property
    def line(self) -> str:
        """All of it in one line."""
        parts = [self.title]
        if self.counted:
            parts.append(self.counted)
        if self.text:
            parts.append(self.text)
        if self.files:
            parts.append(self.files)
        return " · ".join(parts)


def about(fmt: str) -> FormatInfo:
    """What is known about a format as the player shows it ("Video Game Music - Sega
    Megadrive"). A sub-format without notes of its own has its family's."""
    group, plays, text = NOTES.get(fmt) or NOTES.get(fmt.split(" - ", 1)[0]) or ("", "", "")
    playable = is_supported(fmt)
    why = left_out(backend_for(fmt) or "")
    if why:                 # this package was built without its replayer
        group, plays, text = "no", "", why + "."
    elif not group or (group in ("off", "no")) == playable:
        # no notes, or notes from before the format began or stopped to play
        group, plays, text = ("fine", "", "") if playable else ("no", "", "Not looked into.")
    if playable and needed(fmt):
        group = "needs"
    loads, needs = downloads.for_format(fmt), extras.for_format(fmt)
    files = " ".join([*(downloads.describe(d) for d in loads),
                      *(extras.describe(e) for e in needs)])
    there = ([d.name for d in loads if downloads.installed(d)]
             + [e.name for e in needs if extras.state(e)[0] is not None])
    short = " ".join([*(downloads.describe(d) for d in loads if not downloads.installed(d)),
                      *(extras.describe(e) for e in needs if extras.state(e)[0] is None)])
    if there:
        short = (f"Uses {', '.join(there)}: {'all ' if len(there) > 1 else ''}there. "
                 + short).strip()
    return FormatInfo(fmt, group, plays, text, backend_for(fmt) or "", files, short)


def needed(fmt: str) -> bool:
    """Is a file missing that no song of the format plays without?"""
    return bool(downloads.missing(fmt) or extras.missing(fmt))


def why_not(fmt: str) -> str:
    """For a message: why a format can't be played."""
    info = about(fmt)
    if info.group == "needs":
        return f"{fmt} needs a file. " + " ".join(
            [*(downloads.describe(d) for d in downloads.missing(fmt)),
             *(extras.describe(e) for e in extras.missing(fmt))])
    if info.group == "off":
        return f"{fmt} is switched off. {info.text}"
    return f"Can't play {fmt} files. {info.text}".strip()
