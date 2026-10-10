#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Make gbr_tracks.inc: which subsongs of Modland's GBR files are worth playing.

    csrc/nez/gbr-tracks.py ~/.cache/bbmp/samples/modland/"Gameboy Sound System GBR" \
        > csrc/nez/gbr_tracks.inc

A GBR file doesn't say how many songs it has: every number from 0 to 255 can be asked
for, and what the game does with a number it has no song for is up to the game.
Finding out takes a few seconds per file, too long to do when a song is to be played,
so it is done here once. Every subsong is run for ten seconds (bbmp-nez scan); the
ones that make a sound for a second or more and don't begin exactly as an earlier one
are kept. That leaves out the silent numbers, the clicks and the repeats, not what a
game plays by accident for a number it has no song for: nothing in the file tells
that from music.
"""

import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HELPER = Path.home() / ".local/opt/bbmp-nez/bin/bbmp-nez"
SECONDS = 10
QUIET = 12.0


def fnv(data: bytes) -> int:
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def tracks(path: Path) -> list[int]:
    out = subprocess.run([str(HELPER), "scan", str(path), "0", "255", str(SECONDS)],
                         capture_output=True, text=True, check=True).stdout
    rows = [dict(zip(out.splitlines()[0].split("\t"), line.split("\t")))
            for line in out.splitlines()[1:]]
    seen, keep = set(), []
    for r in rows:
        if int(r["calls"]) < 16 or int(r["writes"]) == 0 or float(r["level"]) < QUIET:
            continue
        if r["kind"] == "ends" and float(r["end"]) < 1.0:
            continue        # a click, as many drivers make when they are set up
        if r["begins"] in seen:
            continue
        seen.add(r["begins"])
        keep.append(int(r["song"]))
    return keep


def main() -> int:
    files = sorted(Path(sys.argv[1]).rglob("*.gbr"), key=lambda p: p.name)
    with ThreadPoolExecutor(8) as pool:
        found = list(pool.map(tracks, files))
    print("// SPDX-License-Identifier: GPL-3.0-or-later")
    print("// Made by gbr-tracks.py: the subsongs of Modland's GBR files that sound for a second")
    print("// or more and don't begin as an earlier one does. {FNV-1a of the file, its size, the subsongs as")
    print("// 256 bits, subsong 0 first}")
    for path, keep in zip(files, found):
        data = path.read_bytes()
        bits = bytearray(32)
        for s in keep:
            bits[s >> 3] |= 0x80 >> (s & 7)
        print(f"// {path.name}: {len(keep)}")
        print(f'{{0x{fnv(data):016x}ull, {len(data)}, "{bits.hex()}"}},')
    return 0


if __name__ == "__main__":
    sys.exit(main())
