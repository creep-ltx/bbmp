#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Note in Modland's catalog what each Deflemask and VGM song says it plays on: the
system a Deflemask song was made for (the Mega Drive, the Game Boy...) and the chips a
VGM file drives. Only the first 4 KB of each file is asked for (a Range request), so
about 70 MB is read for 17,800 songs, not 840.

    tools/song_chips.py            every such song
    tools/song_chips.py --only Deflemask

The catalog keeps what it wrote through its updates, and tools/make_chips.py takes
it into the catalog that comes with the player. (HVSC's SID models: tools/sid_chips.py.)
"""

from __future__ import annotations

import argparse
import sqlite3
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from bbmp import catalogs, index as index_mod, paths, song_chips  # noqa: E402

WORKERS = 6


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--only", help="one format, or a start of formats' names")
    args = ap.parse_args()
    db = index_mod._catalogs(paths.CATALOG_DIR).get("modland")
    if db is None:
        raise SystemExit("there is no catalog of Modland")
    con = sqlite3.connect(index_mod.read_only(db), uri=True)
    wanted = [f for (f,) in con.execute("SELECT DISTINCT format FROM files")
              if f.startswith(song_chips.formats()) and (not args.only or f.startswith(args.only))]
    songs = con.execute(f"SELECT path, format FROM files WHERE hidden = 0 AND format IN "
                        f"({', '.join('?' for _ in wanted)})", wanted).fetchall()
    con.close()
    print(f"{len(songs):,} songs in {len(wanted)} formats")
    found: list[tuple[str, str]] = []
    failed = 0
    began = time.time()

    def one(song: tuple[str, str]) -> tuple[str, tuple[str, ...]]:
        path, fmt = song
        try:
            return path, song_chips.read(fmt, song_chips.head(path))
        except OSError:
            return path, ("?",)
    with ThreadPoolExecutor(WORKERS) as pool:
        for n, (path, said) in enumerate(pool.map(one, songs), 1):
            if said == ("?",):
                failed += 1
            else:
                found += [(path, chip) for chip in said]
            if n % 1000 == 0:
                print(f"  {n:,} of {len(songs):,}, {time.time() - began:.0f} s")
    with catalogs.updating(paths.CATALOG_DIR):
        con = sqlite3.connect(db, timeout=600)
        con.execute("CREATE TABLE IF NOT EXISTS chips(collection TEXT, path TEXT, chip TEXT,"
                    " PRIMARY KEY (collection, path, chip)) WITHOUT ROWID")
        con.executemany("DELETE FROM chips WHERE path = ?", ((p,) for p, _ in songs))
        con.executemany("INSERT OR IGNORE INTO chips VALUES ('modland', ?, ?)", found)
        con.commit()
        con.close()
    said = len({p for p, _ in found})
    print(f"{said:,} songs said what they play on; {failed:,} could not be read; "
          f"{len(songs) - said - failed:,} said nothing")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
