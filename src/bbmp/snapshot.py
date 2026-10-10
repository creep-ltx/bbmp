# SPDX-License-Identifier: GPL-3.0-or-later
"""The catalogs written out as text: the collections' listings, packed with xz.

It no longer comes with the program (the collections' lists are fetched at the first
start, and the chips come in known_chips.py); it reads catalogs of an older layout
when they are moved to the new one.

The snapshot is the collections' listings as text, packed with xz: what build() makes
the catalog from. It doesn't depend on the layout of the catalog's database, so a
snapshot outlives a change of that.

"""

from __future__ import annotations

import lzma
import os
import sqlite3
import tempfile
from pathlib import Path
from typing import Callable, Iterable

from . import collection as collections_, index as index_mod, paths
from .collection import Entry, Listing

FIRST = "bbmp catalog 1"
OLD_FIRST = "modland-player catalog 1"     # the same, from before the player had its name
HERE = Path(__file__).parent / "data" / "catalog.xz"


def where() -> Path:
    """The snapshot: what BBMP_SNAPSHOT says, else the one in the program."""
    return Path(paths.env("SNAPSHOT") or HERE)


def _clean(text: str) -> str:
    return text.replace("\t", " ").replace("\n", " ")


def _escape(text: str) -> str:
    """A note on one line: its line breaks (and backslashes) written as \\n (\\\\)."""
    return text.replace("\\", "\\\\").replace("\t", " ").replace("\n", "\\n")


def _unescape(text: str) -> str:
    out, i = [], 0
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text):
            out.append("\n" if text[i + 1] == "n" else text[i + 1])
            i += 2
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def _write_from(con: sqlite3.Connection, out: Path) -> dict[str, int]:
    """Write the collections of an open catalog (or the views of all of them) to `out`."""
    counts = {}
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + ".part")
    with lzma.open(tmp, "wt", encoding="utf-8", preset=9 | lzma.PRESET_EXTREME) as f:
        f.write(FIRST + "\n")
        for c, dated, built in con.execute("SELECT id, dated, built FROM collections"
                                           " ORDER BY hosted, id").fetchall():
            f.write(f"collection\t{c}\t{dated or 0}\t{built or 0}\n")
            counts[c] = 0
            for row in con.execute(
                    "SELECT path, format, artist, album, name, hidden, md5, size"
                    " FROM files WHERE collection = ? ORDER BY id", (c,)):
                path, fmt, artist, album, name, hidden, md5, size = row
                f.write("\t".join(("f", md5, str(size), str(hidden), _clean(fmt),
                                   _clean(artist), _clean(album), _clean(name),
                                   path)) + "\n")
                counts[c] += 1
            for path, lengths in con.execute(
                    "SELECT path, lengths FROM lengths WHERE collection = ?", (c,)):
                f.write(f"l\t{lengths}\t{path}\n")
            for day, kind, path in con.execute(
                    "SELECT day, kind, path FROM changes WHERE collection = ?"
                    " ORDER BY day DESC", (c,)):
                f.write(f"c\t{day}\t{kind}\t{path}\n")
            try:
                for chip, path in con.execute(
                        "SELECT chip, path FROM chips WHERE collection = ?", (c,)):
                    f.write(f"s\t{chip}\t{path}\n")
            except sqlite3.Error:
                pass        # a catalog from before songs' chips were kept
            try:
                for path, text in con.execute(
                        "SELECT path, text FROM notes WHERE collection = ?", (c,)):
                    f.write(f"n\t{_escape(text)}\t{path}\n")
            except sqlite3.Error:
                pass        # a catalog from before notes were kept
    os.replace(tmp, out)
    return counts


def write(out: Path, db_path: Path | None = None) -> dict[str, int]:
    """Write the catalogs' collections to `out`. Gives the files of each."""
    db_path = db_path if db_path is not None else paths.CATALOG_DIR
    ix = index_mod.Index(db_path)
    try:
        return _write_from(ix.con, out)
    finally:
        ix.close()


def listings_of_old(db: Path) -> tuple[dict[str, Listing], dict[str, int]]:
    """The listings in the one catalog of every collection there was before."""
    with tempfile.TemporaryDirectory(dir=db.parent) as tmp:
        packed = Path(tmp) / "old.xz"
        con = sqlite3.connect(index_mod.read_only(db), uri=True)
        try:
            _write_from(con, packed)
        finally:
            con.close()
        return read(packed)


def read(snapshot: Path) -> tuple[dict[str, Listing], dict[str, int]]:
    """The listings in a snapshot, and when each was downloaded from its collection."""
    listings: dict[str, Listing] = {}
    built: dict[str, int] = {}
    with lzma.open(snapshot, "rt", encoding="utf-8", newline="\n") as f:
        if f.readline().rstrip("\n") not in (FIRST, OLD_FIRST):
            raise ValueError(f"{snapshot} is not a catalog this player reads")
        entries = lengths = changes = chips = notes = None
        for line in f:
            kind, _, rest = line.rstrip("\n").partition("\t")
            if kind == "f":
                md5, size, hidden, fmt, artist, album, name, path = rest.split("\t", 7)
                entries.append(Entry(path, fmt, artist, album, name, hidden == "1", md5,
                                     int(size)))
            elif kind == "l":
                length, _, path = rest.partition("\t")
                lengths.append((path, length))
            elif kind == "c":
                day, what, path = rest.split("\t", 2)
                changes.append((day, what, path))
            elif kind == "s":
                chip, _, path = rest.partition("\t")
                chips.append((path, chip))
            elif kind == "n":
                text, _, path = rest.partition("\t")
                notes.append((path, _unescape(text)))
            elif kind == "collection":
                c, dated, when = rest.split("\t")
                entries, lengths, changes, chips, notes = [], [], [], [], []
                listings[c] = Listing(entries, lengths, changes, int(dated), chips, notes)
                built[c] = int(when)
    return listings, built


def install(db_path: Path | None = None, snapshot: Path | None = None,
            progress: Callable[[str], None] = lambda s: None,
            only: Iterable[str] = ()) -> bool:
    """Make the catalogs of the collections named (or of all of them) from the
    snapshot, where they have none and there is a snapshot. Says whether it did."""
    db_path = db_path if db_path is not None else paths.CATALOG_DIR
    snapshot = snapshot or where()
    only = set(only)
    have = set(index_mod._catalogs(db_path))
    if not snapshot.is_file() or (have and (not only or only <= have)):
        return False
    progress("Unpacking the catalog that came with the player...")
    try:
        listings, built = read(snapshot)
    except (OSError, ValueError, lzma.LZMAError, EOFError):
        return False
    # only the collections this player knows, that were asked for and aren't there
    listings = {c: l for c, l in listings.items() if collections_.get(c) is not None
                and (not only or c in only) and c not in have}
    if not listings:
        return False
    index_mod.build(listings, db_path, progress, built=built)
    return True
