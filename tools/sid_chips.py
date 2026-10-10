#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Note in the catalogs which SID each HVSC tune was written for (6581, 8580 or both),
as the tunes' headers say, for the views and search's chip:sid6581 and chip:sid8580.

    tools/sid_chips.py                 download HVSC's archive of itself (85 MB) and read it
    tools/sid_chips.py <C64Music>      read an HVSC that is unpacked already

It writes into HVSC's catalog, and into Modland's for the HVSC folder Modland has; the
catalogs keep what it wrote through their updates, and tools/make_chips.py takes it
into the file of chips that comes with the player. Run it before that one.
"""

from __future__ import annotations

import shutil
import sqlite3
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from bbmp import catalogs, collection as collections_, index as index_mod, paths  # noqa: E402
from bbmp.collection.hvsc import sid_chips  # noqa: E402
from bbmp.keep import _hvsc_archive_url  # noqa: E402


def unpacked(work: Path) -> Path:
    url = _hvsc_archive_url()
    packed = work / "hvsc.7z"
    print(f"Downloading {url} ...")
    request = urllib.request.Request(url, headers={"User-Agent": "bbmp"})
    with urllib.request.urlopen(request, timeout=120) as r, open(packed, "wb") as f:
        shutil.copyfileobj(r, f)
    seven = shutil.which("7z") or shutil.which("7za") or shutil.which("7zz")
    if seven is None:
        raise SystemExit("7z is needed to unpack it")
    subprocess.run([seven, "x", "-y", "-bd", f"-o{work}", str(packed)], check=True,
                   capture_output=True)
    return next(work.rglob("C64Music"))


def main() -> int:
    work = None
    if len(sys.argv) > 1:
        folder = Path(sys.argv[1])
    else:
        paths.CACHE_DIR.mkdir(parents=True, exist_ok=True)
        work = Path(tempfile.mkdtemp(prefix="sid-chips-", dir=paths.CACHE_DIR))
        folder = unpacked(work)
    try:
        chips = sid_chips(folder)
        print(f"{len({p for p, _ in chips}):,} tunes name their SID.")
        host = collections_.get("hvsc").host_path
        found = index_mod._catalogs(paths.CATALOG_DIR)
        with catalogs.updating(paths.CATALOG_DIR):
            for c, rows in (("hvsc", chips), ("modland", [(host(p), chip) for p, chip in chips])):
                if c not in found:
                    continue
                con = sqlite3.connect(found[c], timeout=600)   # a reader may have it a while
                con.execute("CREATE TABLE IF NOT EXISTS chips(collection TEXT, path TEXT,"
                            " chip TEXT, PRIMARY KEY (collection, path, chip)) WITHOUT ROWID")
                con.execute("DELETE FROM chips WHERE chip IN ('sid6581', 'sid8580')")
                con.executemany("INSERT OR IGNORE INTO chips SELECT ?, path, ? FROM files"
                                " WHERE path = ?", ((c, chip, path) for path, chip in rows))
                (n,) = con.execute("SELECT count(*) FROM chips").fetchone()
                con.commit()
                con.close()
                print(f"{collections_.name(c)}: {n:,} songs' chips noted.")
    finally:
        if work is not None:
            shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
