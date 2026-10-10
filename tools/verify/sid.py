#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that libsidplayfp runs a C64 tune's own code as VICE does, by what it writes to the SID.

    tools/verify/sid.py [--songs 300] [--seed 1] [--seconds 30] [--again]

An HVSC tune is its own original player: the composer's 6502 code, which the C64
emulator runs. So what is checked is the emulator: libsidplayfp (tools/verify/
sidwrites.cpp) against VICE's vsid, the reference C64 emulator, which logs every SID
write with its cycle ("dump" sound device). From both, the changes of the first SID's
registers are taken (a write of the value a register has changes nothing), and
compared: the same changes in the same order, and how far apart in time. sidwrites
counts time by the SID's output samples (5 cycles a sample), which runs a little off
the C64's clock, so its times are fitted to VICE's (a factor and an offset, from a
second into the tune), and the worst distance is kept.

What differs is sorted: a tune that reads the SID's oscillator 3 (random numbers: the
chip emulation's, which differ), an RSID tune playing samples through the volume
register (whose timing is to the cycle), or else "differs".

vsid (the Arch package's, unpacked into ~/.local/opt/vice) needs a display: it runs in a
headless cage, with no sound out. Tunes come from your own mirror of Modland (BBMP_MIRROR), by
their paths in the Modland catalog's HVSC. Results: ~/.cache/bbmp-work/verify/sid.json.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import sqlite3
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path

WORK = Path.home() / ".cache/bbmp-work/verify"
VICE = Path.home() / ".local/opt/vice/usr"
CATALOG = Path.home() / ".local/share/bbmp/catalogs/modland.db"
HERE = Path(__file__).resolve().parent
PAL_CYCLES = 985248
SWAP = 2 * 63       # cycles: two changes this close may come in the other order


def sidwrites() -> Path:
    exe = WORK / "bin/sidwrites"
    src = HERE / "sidwrites.cpp"
    if not exe.exists() or exe.stat().st_mtime < src.stat().st_mtime:
        exe.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["g++", "-O2", "-std=c++17", str(src), "-lsidplayfp", "-o", str(exe)], check=True)
    return exe


def tunes() -> list[str]:
    con = sqlite3.connect(f"file:{CATALOG}?mode=ro", uri=True)
    try:
        return [p for (p,) in con.execute(
            "SELECT path FROM files WHERE format = 'HVSC' AND hidden = 0 AND path LIKE '%.sid'")]
    finally:
        con.close()


def fetch(path: str, to: Path) -> Path:
    mirror = os.environ.get("BBMP_MIRROR", "").rstrip("/")
    if not mirror:
        sys.exit("set BBMP_MIRROR to your mirror's address, e.g. http://192.168.1.10:8088")
    out = to / "tune.sid"
    with urllib.request.urlopen(f"{mirror}/modland/pub/modules/{urllib.parse.quote(path)}",
                                timeout=30) as r:
        out.write_bytes(r.read())
    return out


def changes_vice(dump: Path) -> list[tuple[int, int, int]]:
    out, state, t = [], [0] * 32, 0
    for line in dump.read_text().splitlines():
        d, r, v = (int(x) for x in line.split())
        t += d
        if r < 25 and state[r] != v:
            state[r] = v
            out.append((t, r, v))
    return out


def changes_ours(text: str) -> list[tuple[int, int, int]]:
    return [tuple(int(x) for x in line.split()) for line in text.splitlines()]


def vice(tune: Path, cycles: int, tmp: Path) -> list[tuple[int, int, int]] | None:
    dump = tmp / "vice.dump"
    env = {**os.environ, "WLR_BACKENDS": "headless", "WLR_RENDERER": "pixman",
           "WLR_LIBINPUT_NO_DEVICES": "1"}
    subprocess.run(["cage", "--", str(VICE / "bin/vsid"), "-console", "-directory",
                    str(VICE / "share/vice"), "-sounddev", "dump", "-soundarg", str(dump),
                    "-limitcycles", str(cycles), "-warp", "-soundwarpmode", "1", str(tune)],
                   env=env, capture_output=True, timeout=600, cwd=tmp)
    return changes_vice(dump) if dump.exists() else None


def compare(ours: list, ref: list) -> dict:
    a, b = [(r, v) for _, r, v in ours], [(r, v) for _, r, v in ref]
    # the shorter run decides how far; the last few changes may be cut by either's end
    n = min(len(a), len(b))
    # two changes a few cycles apart (within SWAP) may come in the other order: a timing
    # difference of a few cycles, counted, not a difference of what is written
    first, swaps, i = None, 0, 0
    while i < n:
        if a[i] == b[i]:
            i += 1
        elif (i + 1 < n and a[i] == b[i + 1] and a[i + 1] == b[i]
              and ref[i + 1][0] - ref[i][0] <= SWAP):
            swaps += 1
            i += 2
        else:
            first = i
            break
    same = n if first is None else first
    late = None
    # the tune's init and the emulators' own start-up code before the first play call
    # differ (VICE's PSID driver waits for the next frame): time is compared from the
    # first change a second in, with a factor and an offset fitted
    start = next((i for i in range(same) if ref[i][0] - ref[0][0] >= PAL_CYCLES), None)
    if start is not None and same - start > 2:
        idx = range(start, same)
        xs = [ref[i][0] for i in idx]
        ys = [ours[i][0] for i in idx]
        mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
        k = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / max(1, sum((x - mx) ** 2 for x in xs))
        off = my - k * mx
        worst = max(range(len(xs)), key=lambda i: abs((ys[i] - off) / k - xs[i]))
        late = {"cycles": round(abs((ys[worst] - off) / k - xs[worst])), "change": start + worst,
                "scale": round(k, 5)}
    return {"changes": n, "same": same, "swaps": swaps, "first": None if first is None else {
        "index": first, "cycle": ref[first][0], "ours": a[first], "vice": b[first]}, "timing": late}


def check(path: str, seconds: int, tmp: Path) -> dict:
    tune = fetch(path, tmp)
    head = tune.read_bytes()[:0x7C]
    if len(head) >= 0x7C and head[4:6] >= b"\x00\x03" and (head[0x7A] or head[0x7B]):
        return {"state": "skipped", "why": "two or three SIDs (only the first is logged)"}
    cycles = seconds * PAL_CYCLES
    ref = vice(tune, cycles, tmp)
    if not ref:
        return {"state": "skipped", "why": "VICE wrote nothing"}
    ours = subprocess.run([str(sidwrites()), str(tune), "0", str(cycles + PAL_CYCLES // 10)],
                          capture_output=True, timeout=600)
    if ours.returncode:
        return {"state": "skipped", "why": "libsidplayfp: " + ours.stderr.decode(errors="replace")[:100]}
    mine = changes_ours(ours.stdout.decode())
    # compare what both reached: VICE's span of time, in our units
    r = compare(mine, ref)
    if r["first"] is not None and r["first"]["index"] >= min(len(mine), len(ref)) - 3:
        r["first"] = None
    # the same changes in the same order; how far apart in time is kept with them
    # (r["timing"]): VICE's vsid keeps the screen on, whose bad lines take cycles from
    # the CPU, and libsidplayfp's driver doesn't, so a change can come a line or two apart
    state = "same" if r["first"] is None else "differs"
    if state == "differs":
        data = tune.read_bytes()
        if reads_the_sid(data):
            state = "reads the SID"
        elif data[:4] == b"RSID" and sum(1 for _, reg, _ in ref if reg == 24) > 300 * seconds:
            state = "samples"
    return {"state": state, **r}


def reads_the_sid(data: bytes) -> bool:
    """Does the tune's code read the SID's oscillator 3 or envelope 3 ($D41B, $D41C), as
    tunes do for random numbers? What it reads is the chip emulation's, and reSIDfp's is
    not VICE's: what such a tune writes then may differ, though its code runs the same."""
    ops = (0xAD, 0xAE, 0xAC, 0x2D, 0x4D, 0x6D, 0xCD, 0xED, 0x0D, 0xBD, 0xB9, 0xBE, 0xBC)
    return any(bytes([op, reg, 0xD4]) in data for reg in (0x1B, 0x1C) for op in ops)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--songs", type=int, default=300)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--again", action="store_true")
    args = ap.parse_args(argv)
    out = WORK / "sid.json"
    every = tunes()
    pick = random.Random(args.seed).sample(every, min(args.songs, len(every)))
    last = json.loads(out.read_text()) if out.exists() else {}
    if args.again:
        pick = [p for p, r in last.items() if r["state"] not in ("same",)]
    results = {}
    with tempfile.TemporaryDirectory(prefix="bbmp-verify-sid-") as t:
        tmp = Path(t)
        for i, path in enumerate(pick, 1):
            try:
                res = check(path, args.seconds, tmp)
            except (OSError, subprocess.TimeoutExpired) as e:
                res = {"state": "skipped", "why": str(e)[:100]}
            results[path] = res
            extra = "" if res["state"] == "same" else f" {res.get('first') or res.get('timing') or res.get('why')}"
            print(f"[{i}/{len(pick)}] {res['state']}: {path} ({res.get('same', 0)} of "
                  f"{res.get('changes', 0)} changes){extra}", flush=True)
            for f in tmp.iterdir():
                f.unlink()
    if args.again:
        results = {**last, **results}
    out.write_text(json.dumps(results, indent=1, ensure_ascii=False) + "\n")
    states = [r["state"] for r in results.values()]
    worst = sorted(r["timing"]["cycles"] for r in results.values() if r.get("state") == "same" and r.get("timing"))
    if worst:
        print(f"timing of the same: median worst distance {worst[len(worst) // 2]} cycles, "
              f"90% within {worst[len(worst) * 9 // 10]}, most {worst[-1]}")
    print(f"{states.count('same')} the same, {states.count('reads the SID')} that read the SID "
          f"(oscillator 3: the chip emulation's), {states.count('samples')} RSID tunes playing "
          f"samples (timing to the cycle), {states.count('differs')} differ, "
          f"{states.count('skipped')} skipped, of {len(states)} ({out})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
