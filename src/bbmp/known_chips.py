# SPDX-License-Identifier: GPL-3.0-or-later
"""What the player knows of songs' chips that their collections don't say, and that
comes with the program: which SID each HVSC tune was written for, which system each
Deflemask song and VGM file is for. Tools work it out once by reading the files
(tools/sid_chips.py, tools/song_chips.py); tools/make_chips.py writes it here from the
catalogs on this computer, and a new catalog takes it in when it is built.

The collections' song lists themselves don't come with the program: a new user's
player fetches them fresh from the collections at the first start.

A line: collection, the file's checksum, the chip, the path. A fact is taken only
where the file is still the same one (the same checksum), so a file that was changed
since isn't told a chip it may not have.
"""

from __future__ import annotations

import lzma
import os
import sqlite3
from pathlib import Path
from typing import Iterator

FIRST = "bbmp chips 1"
HERE = Path(__file__).parent / "data" / "chips.xz"


def where() -> Path:
    """The file: what BBMP_CHIPS says, else the one in the program."""
    return Path(os.environ.get("BBMP_CHIPS") or HERE)


def write(out: Path, catalogs: dict[str, Path]) -> int:
    """Write the chips of these catalogs (collection -> its database) to `out`."""
    n = 0
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(".tmp")
    with lzma.open(tmp, "wt", encoding="utf-8", newline="\n", preset=9) as f:
        f.write(FIRST + "\n")
        for c, db in sorted(catalogs.items()):
            con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
            try:
                rows = con.execute("SELECT f.md5, s.chip, s.path FROM chips s JOIN files f"
                                   " ON f.path = s.path ORDER BY s.path, s.chip").fetchall()
            except sqlite3.Error:
                rows = []
            finally:
                con.close()
            for md5, chip, path in rows:
                f.write(f"{c}\t{md5}\t{chip}\t{path}\n")
                n += 1
    os.replace(tmp, out)
    return n


def of(collection: str, path: Path | None = None) -> Iterator[tuple[str, str, str]]:
    """(path, checksum, chip) of one collection's songs, from the file that came with
    the program; nothing when there is none."""
    path = path or where()
    try:
        with lzma.open(path, "rt", encoding="utf-8", newline="\n") as f:
            if f.readline().rstrip("\n") != FIRST:
                return
            for line in f:
                c, md5, chip, song = line.rstrip("\n").split("\t", 3)
                if c == collection:
                    yield song, md5, chip
    except (OSError, EOFError, lzma.LZMAError, ValueError):
        return
