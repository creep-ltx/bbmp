# SPDX-License-Identifier: GPL-3.0-or-later
"""Extra files: what some formats need and the player can't get by itself.

A few formats are played with a piece of the machine they are from: the drum samples
in a sound chip's ROM, a console's BIOS, the instrument sounds of a game. Those are
other people's, so the player doesn't download them. Who has them puts them into the
extras folder, ~/.local/share/bbmp/extras, under the names given here, and
the player finds them there.
"""

from __future__ import annotations

import hashlib
import os
import subprocess
from dataclasses import dataclass
from pathlib import Path

from . import paths


@dataclass(frozen=True)
class Extra:
    id: str
    names: tuple[str, ...]      # what the file may be called in the extras folder
    size: int                   # in bytes; another size is another file
    what: str                   # what it is
    formats: tuple[str, ...]    # the formats that use it
    required: bool              # True: without it no song of these formats plays
    without: str                # what happens without it
    source: str                 # where it comes from
    elsewhere: tuple[Path, ...] = ()   # other places it is looked for
    sha256: tuple[str, ...] = ()       # when given: the known good files; any other is refused

    @property
    def name(self) -> str:
        return self.names[0]


EXTRAS = (
    Extra("ym2608", ("ym2608_adpcm_rom.bin",), 8192,
          "the rhythm ROM of the YM2608 sound chip: its six built-in drum sounds",
          ("PMD", "FM sound driver (FMP)"), False,
          "songs that use the chip's drums play without them",
          "a dump of the chip's ROM from a PC-9801-86 or compatible sound board; emulators of "
          "the PC-98 use the same file under the same name",
          (paths.RHYTHM_ROM,)),
    Extra("coleco", ("coleco.rom",), 8192,
          "the ColecoVision's BIOS",
          ("SGC",), False,
          "the ColecoVision songs that call the BIOS are refused (2 of Modland's 386 SGC files); "
          "Master System and Game Gear songs don't need it",
          "a dump of the console's BIOS ROM; emulators of the ColecoVision use the same file",
          (paths.DATA_DIR / "coleco.rom",)),
    Extra("c64kernal", ("kernal.bin", "kernal", "kernal.rom", "kernal-901227-03.bin",
                        "kernal.901227-03.bin"), 8192,
          "the Commodore 64's KERNAL ROM: its operating system",
          ("HVSC", "RealSID"), False,
          "tunes written in BASIC are silent, and so are some that call the C64's system "
          "(about 1 in 8 of the RealSID tunes; few of HVSC's)",
          "a dump of the C64's ROM; C64 emulators such as VICE come with it"),
    Extra("c64basic", ("basic.bin", "basic", "basic.rom", "basic-901226-01.bin",
                       "basic.901226-01.bin"), 8192,
          "the Commodore 64's BASIC ROM",
          ("HVSC", "RealSID"), False,
          "tunes written in BASIC are silent; it is used together with the KERNAL ROM",
          "a dump of the C64's ROM; C64 emulators such as VICE come with it"),
    Extra("c64chargen", ("chargen.bin", "chargen", "chargen.rom", "chargen-901225-01.bin",
                         "characters.901225-01.bin"), 4096,
          "the Commodore 64's character ROM",
          ("HVSC", "RealSID"), False,
          "a few tunes that read the character set play wrong; it is used together with the "
          "KERNAL ROM",
          "a dump of the C64's ROM; C64 emulators such as VICE come with it"),
    Extra("ps2bios", ("hebios.bin", "scph10000_he.bin"), 524288,
          "the PlayStation 2's BIOS in the trimmed form Highly Experimental plays with: "
          "the parts of Sony's BIOS that play sound",
          ("Playstation Sound Format", "Playstation 2 Sound Format"), True,
          "PlayStation and PlayStation 2 songs can't be played",
          "made from a dump of the BIOS of your own PlayStation 2 (SCPH-10000, the first "
          "Japanese model) with mkhebios, the tool that comes with Highly Experimental's "
          "source (also in ZXTune's source); the full 4 MB dump itself is not taken",
          (), ("d7acfdb15575c41f825e20600c3e9bf3c4877b91583b4634543c8d2c255d2692",)),
    Extra("mariopaint", ("mario-paint-notes.wav", "notes.wav"), 26936466,
          "Mario Paint's instrument sounds: recordings of every instrument at every pitch",
          ("Shroom",), True,
          "Shroom's songs can't be played",
          "the file notes.wav in the data folder of Mariopants 1.1 by Brad Smith, a program "
          "that plays Mario Paint's songs"),
)


def folder() -> Path:
    """The extras folder; BBMP_EXTRAS names another one."""
    return Path(paths.env("EXTRAS") or paths.EXTRAS_DIR).expanduser()


def get(id: str) -> Extra:
    return next(e for e in EXTRAS if e.id == id)


def for_format(fmt: str) -> list[Extra]:
    """The extra files a format uses, as the player shows it or by its family."""
    family = fmt.split(" - ", 1)[0]
    return [e for e in EXTRAS if fmt in e.formats or family in e.formats]


def state(extra: Extra) -> tuple[Path | None, str]:
    """(where the file is, or None; what there is to say about it)."""
    places = []
    try:
        there = {f.name.lower(): f for f in folder().iterdir() if f.is_file()}
    except OSError:
        there = {}
    places += [there[n.lower()] for n in extra.names if n.lower() in there]
    places += [p for p in extra.elsewhere if p.is_file()]
    wrong = ""
    for p in places:
        try:
            size = p.stat().st_size
        except OSError:
            continue
        if size == extra.size:
            try:
                known = not extra.sha256 or _sha256(p) in extra.sha256
            except OSError:
                continue
            if known:
                return p, "there"
            wrong = (f"{p.name} is there with the right size, but it is another file "
                     f"than the one that is needed")
            continue
        wrong = (f"{p.name} is there with {size:,} bytes, and the file that is needed "
                 f"has {extra.size:,}")
    return None, wrong or "not there"


_hashes: dict[tuple[str, int, int], str] = {}


def _sha256(p: Path) -> str:
    """The file's SHA-256, worked out again only when the file changes."""
    st = p.stat()
    key = (str(p), st.st_size, st.st_mtime_ns)
    if key not in _hashes:
        _hashes[key] = hashlib.sha256(p.read_bytes()).hexdigest()
    return _hashes[key]


def find(id: str) -> Path | None:
    return state(get(id))[0]


def missing(fmt: str) -> list[Extra]:
    """The files a format can't do without and that aren't there."""
    return [e for e in for_format(fmt) if e.required and state(e)[0] is None]


def describe(extra: Extra) -> str:
    """For the user: what the file is, its name, where it goes, whether it is there."""
    path, note = state(extra)
    text = (f"{extra.name} ({extra.size:,} bytes): {extra.what}. "
            f"It goes into {paths.shown(folder())}. ")
    if path is not None:
        return text + ("It is there." if path.parent == folder()
                       else f"It is there ({paths.shown(path)}).")
    return text + f"It is {note}; without it {extra.without}."


# ---- what the helpers are told ----

def _made(source: Path, dest: Path, command: list[str]) -> Path | None:
    """`dest`, made of `source` by `command` when it isn't there or is older.

    It is taken as made only when the command went to its end: a mark is put next
    to it then ("<dest>.made"). What a command that failed half-way left is made
    again the next time."""
    mark = dest.with_name(dest.name + ".made")
    try:
        if (dest.exists() and mark.exists()
                and mark.stat().st_mtime >= source.stat().st_mtime):
            return dest
        mark.unlink(missing_ok=True)
        dest.parent.mkdir(parents=True, exist_ok=True)
        done = subprocess.run(command, capture_output=True, timeout=300)
        if done.returncode != 0 or not dest.exists():
            return None
        mark.touch()
    except (OSError, subprocess.TimeoutExpired):
        return None
    return dest


def shroom_notes(tool: Path) -> Path | None:
    """Mario Paint's instruments in the form Shroom's replayer reads."""
    notes = find("mariopaint")
    if notes is None or not tool.exists():
        return None
    return _made(notes, paths.SHROOM_NOTES, [str(tool), str(notes), str(paths.SHROOM_NOTES)])


def env(backend: str, helper: list[str]) -> dict[str, str]:
    """The environment a helper is started in: where the extra files are."""
    out = dict(os.environ)
    if backend in ("pmdc", "fmp"):          # they read the YM2608's rhythm ROM themselves
        if rom := find("ym2608"):
            out["BBMP_YM2608_ROM"] = str(rom)
    elif backend == "sgc":
        if bios := find("coleco"):
            out["BBMP_COLECO_BIOS"] = str(bios)
    elif backend == "zxtune":               # its PlayStation player runs Sony's BIOS
        if bios := find("ps2bios"):
            out["BBMP_PS2_BIOS"] = str(bios)
    elif backend == "shroom":
        if notes := shroom_notes(Path(helper[0]).with_name("shroom-samples")):
            out["BBMP_SHROOM_NOTES"] = str(notes)
    return out
