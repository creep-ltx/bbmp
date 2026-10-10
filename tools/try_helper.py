#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Try a helper program on the songs of one format and say what came out.

    tools/try_helper.py "SoundSmith" -- ~/.local/opt/bbmp-x/bin/bbmp-x [engine]

The helper is run as the player runs it: <helper...> <file> <track> <rate>, and has to
answer with one line of JSON and then s16le stereo (see backends/pipe.py). The songs
come from a cache of downloaded songs (--cache), by default the test set in
~/.cache/bbmp/samples. Nothing is played through the speakers.

A song counts as playing when the helper gives a header and sound that is neither
silence nor noise. That is a first check and no proof that it plays right: compare
with a recording or another player where there is one.
"""

from __future__ import annotations

import argparse
import array
import json
import math
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from bbmp.index import Index  # noqa: E402

RATE = 48000


def judge(pcm: bytes) -> dict:
    a = array.array("h")
    a.frombytes(pcm[:len(pcm) // 4 * 4])
    left = a[::2]
    if not left:
        return {"seconds": 0.0, "rms": 0.0, "clipped": 0.0, "crossings": 0.0}
    step = max(1, len(left) // 400_000)
    some = left[::step]
    rms = math.sqrt(sum(x * x for x in some) / len(some))
    clipped = sum(1 for x in some if abs(x) >= 32700) / len(some)
    # how often neighbouring samples change sign: music is far below noise's 0.5
    near = left[:min(len(left), 2_000_000)]
    crossings = sum(1 for x, y in zip(near, near[1:]) if (x < 0) != (y < 0)) / max(1, len(near) - 1)
    return {"seconds": len(left) / RATE, "rms": rms, "clipped": clipped, "crossings": crossings}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("format", help='as the player shows it, e.g. "SoundSmith"')
    ap.add_argument("--cache", type=Path, default=Path.home() / ".cache/bbmp/samples")
    ap.add_argument("--seconds", type=int, default=30, help="give up on a song after this long")
    ap.add_argument("--limit", type=int, default=0, help="try only this many songs")
    ap.add_argument("--track", type=int, default=0)
    ap.add_argument("--quiet", action="store_true", help="only the songs that don't play")
    ap.add_argument("helper", nargs="+", help="after --: the helper and its first arguments")
    args = ap.parse_args()

    ix = Index()
    rows = ix._all("SELECT path FROM files WHERE hidden = 0 AND format = ? ORDER BY path",
                   (args.format,))
    files = [(p, args.cache / "modland" / p) for (p,) in rows]
    files = [(p, f) for p, f in files if f.exists()]
    if args.limit:
        files = files[:args.limit]
    if not files:
        print(f"no songs of {args.format!r} in {args.cache}", file=sys.stderr)
        return 2
    counts = {"plays": 0, "silent": 0, "noise": 0, "failed": 0}
    for path, file in files:
        try:
            done = subprocess.run([*args.helper, str(file), str(args.track), str(RATE)],
                                  capture_output=True, timeout=args.seconds)
            out, cut = done.stdout, False
        except subprocess.TimeoutExpired as e:
            out, cut = e.stdout or b"", True
        head, _, pcm = out.partition(b"\n")
        try:
            meta = json.loads(head or b"{}")
        except ValueError:
            meta = {"error": f"no header: {head[:60]!r}"}
        j = judge(pcm)
        if "error" in meta or not meta.get("tracks"):
            state, note = "failed", str(meta.get("error", "no tracks"))
        elif j["seconds"] < 0.5:
            state, note = "failed", "a header and no sound" + (" (still working)" if cut else "")
        elif j["rms"] < 20:
            state, note = "silent", ""
        elif j["crossings"] > 0.35 or j["clipped"] > 0.2:
            state, note = "noise", f"crossings {j['crossings']:.2f}, clipped {j['clipped']:.0%}"
        else:
            state, note = "plays", ""
        counts[state] += 1
        if state != "plays" or not args.quiet:
            length = f"{(meta.get('duration_ms') or 0) / 1000:6.0f}s" if state != "failed" else "      -"
            print(f"{state:6} {j['seconds']:6.0f}s{'+' if cut else ' '} says {length}  rms {j['rms']:6.0f}"
                  f"  tracks {meta.get('tracks', 0):2}  {path[len(args.format.split(' - ')[0]) + 1:][:58]}"
                  f"  {note}")
    total = sum(counts.values())
    print(f"\n{args.format}: {counts['plays']} of {total} play"
          + "".join(f", {n} {k}" for k, n in counts.items() if k != "plays" and n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
