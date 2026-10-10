#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Find the songs of a format that stay silent, and add them to the census's list of
songs known to fail (tools/format_census.json), so that the player marks them and radio
passes them over, instead of each being found by half a minute of silence.

    PYTHONPATH=src tools/find_silent.py "Dreamcast Sound Format" --where Naomi
        --why "an empty track of the rip: no music (two emulators agree)"

Each song is played (never through the speakers) for --seconds; one that makes no sound
in that time is written down with --why. Then tools/format_status.py makes the
player's list (known_trouble.py) from the census.
"""

from __future__ import annotations

import argparse
import array
import json
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE / "src"))

from bbmp.backends import open_song  # noqa: E402
from bbmp.fetch import Fetcher  # noqa: E402
from bbmp.index import Index  # noqa: E402

CENSUS = HERE / "tools" / "format_census.json"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("format")
    ap.add_argument("--where", default="", help="only songs with this in their path")
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--why", default="stays silent")
    ap.add_argument("--workers", type=int, default=2)
    args = ap.parse_args()
    ix, fetcher = Index(), Fetcher()
    rows = ix._all("SELECT path FROM files WHERE collection = 'modland' AND format = ?"
                   " AND hidden = 0 AND path LIKE ? ORDER BY path",
                   (args.format, f"%{args.where}%"))
    print(f"{len(rows)} songs", flush=True)

    def one(path: str) -> tuple[str, str]:
        key = "modland:" + path
        try:
            file = fetcher.clone().get(key, ix.companions(key))
            d = open_song(file, args.format, ix.lengths(key))
        except Exception as e:          # a song that fails is the census's to count
            return path, "failed " + type(e).__name__
        try:
            for _ in range(args.seconds):
                pcm = d.read(48000)
                if not pcm:
                    break
                a = array.array("h", pcm)
                if max(max(a), -min(a)) > 300:
                    return path, "sound"
            return path, "silent"
        finally:
            d.close()
    silent, n = [], 0
    with ThreadPoolExecutor(args.workers) as pool:
        for path, state in pool.map(one, [p for (p,) in rows]):
            n += 1
            if state == "silent":
                silent.append(path)
            if n % 100 == 0:
                print(f"  {n} of {len(rows)}: {len(silent)} silent", flush=True)
    census = json.loads(CENSUS.read_text())
    entry = census[args.format]
    trouble = entry.setdefault("trouble", [])
    known = {t["song"] for t in trouble}
    for path in silent:
        if path not in known:
            trouble.append({"backend": entry.get("best", ""), "by": "find_silent",
                            "note": args.why, "song": path, "state": "silent"})
    CENSUS.write_text(json.dumps(census, indent=1, ensure_ascii=False) + "\n")
    print(f"{len(silent)} of {len(rows)} stay silent; the census has {len(trouble)} songs "
          f"known to fail in {args.format}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
