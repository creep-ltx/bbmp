#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Does the bbmp-mac package make sound? Render real songs from Modland to WAV with it, in
# an environment with nothing else (no Homebrew, no Python of the system's), and say how
# loud each came out. Usage: tools/mac/sound-check.sh <unpacked bbmp-mac folder> <out folder>
set -uo pipefail
PKG="$1"; OUT="$2"
mkdir -p "$OUT/songs"
SONGS=(
  "Protracker/Tim Wright/lemmings.mod"
  "HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid"
  "AHX/Jazz (NL)/ocean loader.ahx"
  "Nintendo SPC/Koji Kondo/Super Mario World/11a - overworld.spc"
  "Video Game Music/Sega Megadrive/Masato Nakamura/Sonic The Hedgehog/02 green hill zone.vgz"
  "Video Game Music/MSX1/Miki Higashino/Nemesis 1 (PSG)/space graveyard.vgz"
  "S98/Meiko Ishikawa/Brandish/06-br1shop2.s98"
  "Fasttracker 2/Mango/tetris.xm"
  "Ad Lib/ADL/Frank Klepacki/legend of kyrandia - misc.adl"
  "Ad Lib/Reality AdLib Tracker/Purple Motion/starport bbs introtune.rad"
  "YM/Rob Hubbard/commando.ym"
  "SNDH/Hubbard_Rob/Populous.sndh"
)
for s in "${SONGS[@]}"; do
  url="https://ftp.modland.com/pub/modules/$(python3 -c 'import sys,urllib.parse;print(urllib.parse.quote(sys.argv[1]))' "$s")"
  curl -sfL --retry 3 -o "$OUT/songs/$(basename "$s")" "$url" || echo "can't download $s"
done
# the chip helpers by themselves on AdLib and SNDH files (another Mac played none of them):
# what each file is, and what the helper says (SNDH: bbmp-psgplay, since 7 Oct 2026)
for f in "$OUT/songs/"*.adl "$OUT/songs/"*.rad "$OUT/songs/"*.sndh; do
  cmd=("$PKG/opt/bbmp-chip/bin/bbmp-chip" adplug)
  case "$f" in *.sndh) cmd=("$PKG/opt/bbmp-psgplay/bin/bbmp-psgplay") ;; esac
  echo "== $(basename "${cmd[0]}") $(basename "$f"): $(wc -c < "$f") bytes, starts $(head -c 16 "$f" | od -An -tx1 | tr -s ' ')"
  "${cmd[@]}" "$f" 0 48000 2>"$OUT/err.txt" | head -c 400 | strings | head -2
  head -3 "$OUT/err.txt"
done
# the same, shut in as the player shuts it in: how it ends, what it says, and what the
# sandbox refused it
echo "== shut in:"
BBMP_OPT="$PKG/opt" BBMP_BUNDLE="$PKG" PYTHONPATH="$PKG/app" "$PKG/python/bin/python3" - "$PKG" "$OUT"/songs/*.ym "$OUT"/songs/*.adl "$OUT"/songs/*.sndh <<'PY'
import subprocess, sys
from pathlib import Path
from bbmp import sandbox
pkg = Path(sys.argv[1])
for f in map(Path, sys.argv[2:]):
    kind = {".ym": "ym", ".adl": "adplug", ".sndh": "sndh"}[f.suffix]
    exe = ([str(pkg / "opt/bbmp-psgplay/bin/bbmp-psgplay")] if kind == "sndh"
           else [str(pkg / "opt/bbmp-chip/bin/bbmp-chip"), kind])
    cmd = sandbox.wrap([*exe, str(f), "0", "48000", "0"], f)
    done = subprocess.run(cmd, capture_output=True, timeout=60)
    print(f"{kind} {f.name}: exit {done.returncode}, {len(done.stdout)} bytes out;",
          done.stderr.decode(errors="replace").strip()[-300:])
PY
sleep 2
log show --last 2m --style compact --predicate 'eventMessage CONTAINS "deny("' 2>/dev/null \
  | grep -iE "bbmp-chip|bbmp-psgplay" | sed 's/.*Sandbox: //' | sort | uniq -c | sort -rn | head -20
env -i HOME="$HOME" PATH=/usr/bin:/bin TERM=dumb "$PKG/bbmp" export --all --one --wav --no-level -o "$OUT/wav" "$OUT"/songs/* 2>&1
"$PKG/python/bin/python3" - "$OUT/wav" <<'PY'
import sys, wave, pathlib
import numpy as np
files = sorted(pathlib.Path(sys.argv[1]).rglob("*.wav"))
loud = 0
for f in files:
    with wave.open(str(f)) as w:
        a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    peak = int(np.abs(a).max()) if a.size else 0
    loud += peak > 1000
    print(f"{peak:6d} peak  {a.size / 2 / 48000:6.1f} s  {f.name}")
print(f"{loud} of {len(files)} rendered files have sound")
PY
