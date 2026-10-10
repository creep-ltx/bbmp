# SPDX-License-Identifier: GPL-3.0-or-later
"""The PSF family (PlayStation, Saturn, Dreamcast, Nintendo and others): a song names
the library files it needs inside itself, in a tag (_lib=, _lib2=, ...), and its
replayer opens them by exactly that name, next to the song.

Modland's names are not always those: it has "vab kaze.psflib" where the song asks for
"Vab_kaze.psflib", "sq0.ssflib" for "SQ0.ssflib", and "soa-329.dsflib" for
"SOA_329.dsflib". fit_libs() gives each library the
name the song asks for, as a link next to the file that is there.
"""

from __future__ import annotations

import os
import re
import struct
from pathlib import Path

# the extensions of songs of the family, and of the libraries they name
SONG_EXTS = {"psf", "minipsf", "psf2", "minipsf2", "ssf", "minissf", "dsf", "minidsf",
             "2sf", "mini2sf", "usf", "miniusf", "gsf", "minigsf", "qsf", "miniqsf",
             "snsf", "minisnsf", "ncsf", "minincsf"}
LIB_TAG = re.compile(r"^_lib\d*$")


def libs_named(file: Path) -> list[str]:
    """The library files a PSF-family file names in its tag, in the order it names them."""
    try:
        data = file.read_bytes()
    except OSError:
        return []
    if len(data) < 16 or data[:3] != b"PSF":
        return []
    reserved, program = struct.unpack_from("<II", data, 4)
    start = 16 + reserved + program
    if data[start:start + 5] != b"[TAG]":
        return []
    names = []
    for line in data[start + 5:].decode("utf-8", "replace").splitlines():
        key, eq, value = line.partition("=")
        if eq and LIB_TAG.match(key.strip().lower()) and value.strip():
            names.append(value.strip())
    return names


def _plain(name: str) -> str:
    """A name as Modland may have changed it: case, and _ or - for a space (Skies of
    Arcadia's songs ask for "SOA_329.dsflib", Modland has "soa-329.dsflib")."""
    return re.sub(r"[\s_-]+", " ", name.lower()).strip()


def fit_libs(song: Path) -> list[Path]:
    """Give the libraries the song (and its libraries) need the names they are asked by,
    where the folder has them under another. Gives the links it made."""
    if song.suffix.lower().lstrip(".") not in SONG_EXTS:
        return []
    folder = song.parent
    made, seen, todo = [], set(), [song]
    while todo:
        file = todo.pop()
        for wanted in libs_named(file):
            if "/" in wanted or "\\" in wanted or wanted in seen:
                continue
            seen.add(wanted)
            target = folder / wanted
            if not target.exists():
                there = [f for f in folder.iterdir()
                         if f.is_file() and _plain(f.name) == _plain(wanted)]
                if len(there) != 1:
                    continue        # not there, or not beyond doubt
                try:
                    if target.is_symlink():
                        target.unlink()         # a link to a file that went away
                    os.symlink(there[0].name, target)
                except OSError:
                    continue
                made.append(target)
            todo.append(target)
    return made
