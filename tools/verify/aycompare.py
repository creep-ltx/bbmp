#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare two AY register dumps (14 bytes a frame, register 13 0xff when not written).

    aycompare.py <ours> <reference> [--show N]

Only the bits the AY chip has are compared (tone periods 12 bits, noise 5, volumes 5,
envelope shape 4), and only what can be heard: a channel's tone period not when its
tone is off in the mixer, the noise period not when no channel has noise on. Register 13
counts only in a frame that writes it, as a write of it restarts the envelope. Says how
many frames are the same and lists the first that differ.
"""

from __future__ import annotations

import sys

MASK = (0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0x3F, 0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F)
NAMES = ("A tone", "A tone hi", "B tone", "B tone hi", "C tone", "C tone hi", "noise",
         "mixer", "A vol", "B vol", "C vol", "env lo", "env hi", "env shape")


def frames(data: bytes) -> list[tuple[int, ...]]:
    out = []
    for i in range(0, len(data) - 13, 14):
        row = data[i:i + 14]
        out.append(tuple((row[r] & MASK[r]) if r < 13 else (-1 if row[13] == 0xFF else row[13] & 0x0F)
                         for r in range(14)))
    return out


def audible(row: tuple[int, ...]) -> tuple[int, ...]:
    """What of a frame's registers can be heard: a channel's tone period not when its
    tone is off in the mixer, the noise period not when every channel's noise is off."""
    r = list(row)
    mixer = r[7]
    for ch in range(3):
        if mixer & (1 << ch):
            r[2 * ch] = r[2 * ch + 1] = None
    if mixer & 0x38 == 0x38:
        r[6] = None
    return tuple(r)


def compare(ours: bytes, ref: bytes) -> dict:
    a, b = [audible(x) for x in frames(ours)], [audible(x) for x in frames(ref)]
    n = min(len(a), len(b))
    diffs = []
    for f in range(n):
        if a[f] != b[f]:
            diffs.append((f, [r for r in range(14) if a[f][r] != b[f][r]]))
    return {"frames": n, "ours": len(a), "ref": len(b), "differ": len(diffs), "diffs": diffs,
            "a": a, "b": b}


def main(argv: list[str]) -> int:
    show = 10
    if "--show" in argv:
        show = int(argv[argv.index("--show") + 1])
    ours, ref = open(argv[1], "rb").read(), open(argv[2], "rb").read()
    r = compare(ours, ref)
    print(f"{r['frames']} frames compared ({r['ours']} ours, {r['ref']} reference): "
          f"{r['frames'] - r['differ']} the same, {r['differ']} differ")
    for f, regs in r["diffs"][:show]:
        print(f"  frame {f}: " + ", ".join(f"{NAMES[x]} {r['a'][f][x]} vs {r['b'][f][x]}" for x in regs))
    return 0 if r["differ"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
