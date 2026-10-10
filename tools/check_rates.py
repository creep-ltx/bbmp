#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""For each helper, one song of the census' samples: does the sound it sends last as
long as it says? A helper that writes another sample rate than it is asked for plays
too fast or too slow, which a census of sound and silence can't hear. Furnace did:
it wrote 44100 Hz, and Deflemask songs played 9 % fast and high.

A ratio near 1.0 is right; a tail of some seconds (notes ringing out, a fade) is fine.
Near 0.919 or 1.088 is 44100 against 48000."""
import json, subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from bbmp.backends.pipe import HELPERS, helper
census = json.loads((Path(__file__).resolve().parent / "format_census.json").read_text())
samples = Path.home() / ".cache/bbmp/samples/modland"
seen = {}
for fmt, c in sorted(census.items()):
    b = c.get("best")
    if b in HELPERS and b not in seen:
        folder = samples / fmt
        files = [f for f in folder.rglob("*") if f.is_file()] if folder.exists() else []
        if files:
            seen[b] = (fmt, files)
for b, (fmt, files) in sorted(seen.items()):
    cmd = helper(b)
    if not cmd:
        print(f"{b:14s} not installed"); continue
    for f in files[:6]:
        try:
            p = subprocess.run([*cmd, str(f), "0", "48000"], capture_output=True, timeout=60)
        except subprocess.TimeoutExpired:
            continue
        line, _, pcm = p.stdout.partition(b"\n")
        try:
            head = json.loads(line)
        except Exception:
            continue
        d = head.get("duration_ms") or 0
        if head.get("tracks") and d and len(pcm) > 48000 * 4:
            got = len(pcm) / 192000
            ratio = got / (d / 1000)
            flag = "" if 0.97 < ratio < 1.03 or got < d / 1000 - 30 else "  <-- CHECK"
            print(f"{b:14s} {fmt[:30]:30s} says {d/1000:7.1f}s, sends {got:7.1f}s ratio {ratio:.3f}{flag}")
            break
    else:
        print(f"{b:14s} {fmt[:30]:30s} no song with a stated length")
