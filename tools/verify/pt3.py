#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check ZXTune's Pro Tracker 2 and 3 against the original Z80 players, register by register.

    tools/verify/pt3.py [--format pt3|pt2] [--songs 200] [--seed 1] [--all]

Songs come from your own mirror of Modland (BBMP_MIRROR) by their paths in the Modland catalog.
For each, `bbmp-zxtune --aydump` writes what ZXTune sends to the AY chip frame by
frame, and ayref (ayref.c) runs the original player on an emulated Z80 and writes the
same; aycompare.py compares them. The players:

  pt3  Vortex Tracker II's PT3 player (S.V. Bulba, release 7, 2007), which plays the
       songs of every Pro Tracker 3 version as that version does; from Vortex Tracker
       II's source (ZXAYHOBETA/C000.bin, assembled for C000). Free to use, crediting
       its author.
  pt2  Pro Tracker 2.101's own player (GDC), from the listing the magazine article
       "PT2.TXT" (VfNG/NEW) printed, kept in S.V. Bulba's PT2Docs; get-references.sh
       fetches and assembles it. It plays a song's first row after the song's delay
       less one frame, where ZXTune plays it at once: the frames are compared from there.

Writes a line a song and a summary; the details go to ~/.cache/bbmp-work/verify/<fmt>.json.
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

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import aycompare  # noqa: E402

WORK = Path.home() / ".cache/bbmp-work/verify"
ZXTUNE = Path.home() / ".local/opt/zxtune/bin/bbmp-zxtune"
Z80 = Path.home() / ".local/src/superzazu-z80"
CATALOG = Path.home() / ".local/share/bbmp/catalogs/modland.db"

# format: (Modland format, player binary, where it is loaded, init (HL = song), play)
PLAYERS = {
    "pt3": ("Spectrum - Pro Tracker 3", WORK / "vt/ZXAYHOBETA/C000.bin", 0xC000, 0xC003, 0xC005),
    "pt2": ("Spectrum - Pro Tracker 2", WORK / "pt2std/pt2std.bin", 0xC000, 0xC003, 0xC006),
}
SONG_AT = 0x4000
# Pro Tracker 2's player reads the note table (96 notes at C80D) at C25D and C25F
# (pt2std.lst); a note past it reads what follows, unchecked
PT2_TABLE = ("table=0xC25D,0xC25F:0xC80D:0xC8CD",)
# S.V. Bulba's "Universal PT2 player" (2005), which mends the original's faults: from his
# PTxTools (get-references.sh), assembled for C000
BULBA_PT2 = WORK / "bulba/PTxTools/PT2Play/PT2Play"


def ayref() -> Path:
    exe = WORK / "bin/ayref"
    src = HERE / "ayref.c"
    if not exe.exists() or exe.stat().st_mtime < src.stat().st_mtime:
        exe.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["gcc", "-O2", "-std=gnu17", f"-I{Z80}", str(src), str(Z80 / "z80.c"),
                        "-o", str(exe)], check=True)
    return exe


def songs(fmt: str) -> list[str]:
    con = sqlite3.connect(f"file:{CATALOG}?mode=ro", uri=True)
    try:
        return [p for (p,) in con.execute(
            "SELECT path FROM files WHERE format = ? AND hidden = 0 ORDER BY path", (fmt,))]
    finally:
        con.close()


def fetch(path: str, to: Path) -> Path:
    mirror = os.environ.get("BBMP_MIRROR", "").rstrip("/")
    if not mirror:
        sys.exit("set BBMP_MIRROR to your mirror's address, e.g. http://192.168.1.10:8088")
    out = to / Path(path).name
    with urllib.request.urlopen(f"{mirror}/modland/pub/modules/{urllib.parse.quote(path)}",
                                timeout=30) as r:
        out.write_bytes(r.read())
    return out


def pt2_unsampled(data: bytes) -> set[int]:
    """The channels of a PT2 song that play a note before any sample is chosen for them
    (the codes as Pro Tracker 2's player reads them)."""
    pats = data[99] | data[100] << 8
    out = set()
    for ch in range(3):
        chosen = False
        for i in range(131, min(len(data), 131 + 256)):
            pos = data[i]
            if pos == 0xFF:
                break
            at = pats + pos * 6 + ch * 2
            if at + 1 >= len(data):
                break
            j = data[at] | data[at + 1] << 8
            while 0 <= j < len(data):
                b = data[j]
                j += 1
                if b == 0x00:
                    break
                if b > 0xE0:
                    chosen = True
                elif 0x80 <= b < 0xE0:
                    if not chosen:
                        out.add(ch)
                    break
                elif 0x71 <= b <= 0x7E:
                    j += 2
                elif b in (0x0F, 0x0E):
                    j += 1
                elif b == 0x0D:
                    j += 3
                elif b < 0x0C:
                    j += 1
            if chosen or ch in out:
                break
    return out


def check(key: str, song: Path, tmp: Path) -> dict:
    fmt, player, at, init, play = PLAYERS[key]
    data = song.read_bytes()
    if len(data) > 0xC000 - SONG_AT - 0x100:
        return {"state": "skipped", "why": "too big for the test machine's memory"}
    if b"02TS" in data[-4:] or b"PT3!" in data[16:]:
        return {"state": "skipped", "why": "Turbo Sound (two songs in one)"}
    if key == "pt3" and not data.startswith((b"ProTracker 3.", b"Vortex Tracker II")):
        return {"state": "skipped", "why": "not a plain module (a song inside a program)"}
    ours = subprocess.run([str(ZXTUNE), "--aydump", str(song), "0"], capture_output=True, timeout=120)
    if ours.returncode or not ours.stdout:
        return {"state": "skipped", "why": "ZXTune: " + ours.stderr.decode(errors="replace").strip()[:100]}
    n = len(ours.stdout) // 14
    past, beyond, outside = [], set(), set()

    def reference(*extra: str) -> bytes | None:
        ref = subprocess.run([str(ayref()), str(player), hex(at), str(song), hex(SONG_AT), hex(init),
                              hex(play), str(n), *extra], capture_output=True, timeout=300)
        said = ref.stderr.decode(errors="replace")
        for line in said.splitlines():
            if line.startswith("read past the song's end in frame"):
                past.append(int(line.rsplit(" ", 1)[1]))
            elif line.startswith("past an object's end: "):
                beyond.add(int(line.rsplit(" ", 1)[1]))
            elif line.startswith("outside the table: "):
                outside.add(int(line.rsplit(" ", 1)[1]))
        return None if ref.returncode else ref.stdout

    lead = max(0, data[0] - 1) if key == "pt2" else 0      # PT2: the first row waits
    ref = reference("pt3") if key == "pt3" else reference(*PT2_TABLE)
    if ref is None:
        return {"state": "skipped", "why": "the reference player failed"}
    ref = ref[14 * lead:]
    r = aycompare.compare(ours.stdout[:len(ours.stdout) - 14 * lead], ref)
    state = "same"
    if r["differ"] and key == "pt3":
        # a channel that plays before it chooses a sample: undefined in Pro Tracker 3;
        # the Z80 player makes it silent, Vortex Tracker, Ay_Emul and ZXTune use sample 1
        ref1 = reference("pt3-sample1", "pt3")
        if ref1 is not None:
            r1 = aycompare.compare(ours.stdout, ref1)
            if r1["differ"] < r["differ"]:
                r, state = r1, "same, sample 1"
    if r["differ"]:
        state = "differs"
        # the song needs bytes the file doesn't have: it was cut short
        if past and r["diffs"][0][0] >= min(past):
            state = "cut short"
        # a sample or ornament started past its end: the original plays whatever bytes
        # follow it for a frame, the editors silence or nothing
        # only where every frame that differs is one in which it happened: a note
        # past the end of the note table (the original reads whatever follows it, and
        # only the note's pitch differs), a sample or ornament started past its end
        elif all(f + lead in outside and set(regs) <= {0, 1, 2, 3, 4, 5}
                 for f, regs in r["diffs"]):
            state = "note past the table"
        elif all(f in beyond or f - 1 in beyond for f, _ in r["diffs"]):
            state = "offset past the end"
        # a channel that plays before it chooses a sample: the original players read
        # their sample pointer as they found it (zero: undefined), ZXTune uses sample 1
        elif key == "pt2" and pt2_unsampled(data) and all(
                set(regs) <= {r for ch in pt2_unsampled(data) for r in (2 * ch, 2 * ch + 1, 8 + ch)} | {6, 7}
                for _, regs in r["diffs"]):
            state = "no sample chosen"
        # Pro Tracker 2: S.V. Bulba's player mends faults of the original's (its tone
        # portamento); where ZXTune plays the song as his does, that is why
        elif key == "pt2":
            fixed = subprocess.run([str(ayref()), str(BULBA_PT2), "0xC000", str(song), hex(SONG_AT),
                                    "0xC003", "0xC005", str(n)], capture_output=True, timeout=300)
            if not fixed.returncode and not aycompare.compare(ours.stdout, fixed.stdout)["differ"]:
                state = "as the mended player"
    first = r["diffs"][0] if r["diffs"] else None
    return {"state": state, "frames": r["frames"],
            "differ": r["differ"],
            "first": None if first is None else {
                "frame": first[0],
                "regs": {aycompare.NAMES[x]: [r["a"][first[0]][x], r["b"][first[0]][x]] for x in first[1]}}}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--format", default="pt3", choices=sorted(PLAYERS))
    ap.add_argument("--songs", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--again", action="store_true",
                    help="only the songs that differed or were skipped last time")
    args = ap.parse_args(argv)
    out = WORK / f"{args.format}.json"
    every = songs(PLAYERS[args.format][0])
    pick = every if args.all else random.Random(args.seed).sample(every, min(args.songs, len(every)))
    if args.again:
        last = json.loads(out.read_text())
        pick = [p for p, r in last.items() if r["state"] in ("differs", "skipped", "cut short", "offset past the end",
                                                      "note past the table", "as the mended player",
                                                      "no sample chosen")]
    results = {}
    with tempfile.TemporaryDirectory(prefix="bbmp-verify-") as t:
        tmp = Path(t)
        for i, path in enumerate(pick, 1):
            try:
                res = check(args.format, fetch(path, tmp), tmp)
            except (OSError, subprocess.TimeoutExpired) as e:
                res = {"state": "skipped", "why": str(e)[:100]}
            results[path] = res
            extra = (f" {res['differ']} of {res['frames']} frames differ, first {res['first']}"
                     if res["state"] == "differs" else f" {res.get('why', '')}")
            print(f"[{i}/{len(pick)}] {res['state']}: {path}{extra if res['state'] not in ('same', 'same, sample 1') else ''}",
                  flush=True)
            for f in tmp.iterdir():
                f.unlink()
    if args.again:
        results = {**last, **results}
    out.write_text(json.dumps(results, indent=1, ensure_ascii=False) + "\n")
    states = [r["state"] for r in results.values()]
    print(f"{states.count('same')} the same, {states.count('same, sample 1')} the same with "
          f"sample 1 for a channel that chose none, {states.count('cut short')} files cut short, "
          f"{states.count('offset past the end')} with an offset past a sample's end, "
          f"{states.count('note past the table')} with a note past the note table, "
          f"{states.count('as the mended player')} as S.V. Bulba's mended player plays them, "
          f"{states.count('no sample chosen')} with a channel that plays before choosing a sample, "
          f"{states.count('differs')} differ, "
          f"{states.count('skipped')} skipped, of {len(states)} ({out})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
