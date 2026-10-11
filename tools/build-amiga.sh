#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-amiga: Amiga formats that UADE has no player for, or plays only when
# the files are laid out in another way than Modland has them.
#   maxtrax       MaxTrax (.mxtx): ScummVM's MaxTrax player and Paula. GPL 3 or later.
#   stonetracker  StoneTracker (.spm + .sps): StoneTracker's own replay routine
#                 ("StonePlayer_Hard.Bin" 1.98 from the free edition of StoneTracker
#                 1.26 on Aminet), run by UADE with the glue of
#                 csrc/amiga/stonetracker_player.asm. The whole archive and its readme
#                 are installed with it, as its terms ask.
#   jesperolsen   Jesper Olsen (.jo): csrc/amiga/jo_engine.cpp, written for this player
#                 (a clean room: the games' routines may not be shipped), on ScummVM's
#                 Paula. The songs that carry their own routine play through UADE.
#   zoundmonitor  ZoundMonitor: UADE's player, given the samples where it looks for them
#                 (the player is downloaded on request: BBMP_UADE_PLAYERS).
# UADE itself (tools/build-uade.sh) has to be installed already: bbmp-amiga uses
# its library, its players and its files, and keeps a base folder of its own for it,
# <prefix>/share/uade, which is UADE's with the StoneTracker player added.
# Sources go to ~/.local/src, the result to ~/.local/opt/bbmp-amiga.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
SRC="$BBMP_SRC"
PREFIX="${1:-$BBMP_OPT/bbmp-amiga}"
UADE="${UADE_PREFIX:-$BBMP_OPT/uade}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
FILES="$SRC/modland-amiga-files"
UADE_COMMIT=8d77cd773e10d33de44f2b6c52201bb54732b701
SCUMMVM_COMMIT=e6c3b103c96822eb9212f743be63c998383f7c4f
AMITOOLS=0.8.1
VASM=vasm2_0f

[ -f "$UADE/lib/libuade$SO" ] && [ -f "$UADE/share/uade/eagleplayer.conf" ] || {
  echo "UADE is not installed in $UADE: run tools/build-uade.sh first" >&2; exit 1; }
mkdir -p "$SRC" "$FILES" "$PREFIX/bin"
rm -rf "$PREFIX/share/jesperolsen"   # the games' routines, used until 1 October 2026
rm -f "$PREFIX/left-out.txt"
cd "$SRC"

# fetch <file> <url> <sha256>: downloads once, and checks what is there
fetch() {
  if kept && [ ! -f "$1" ]; then kept_copy "$(basename "$1")" "$1"; fi
  [ -f "$1" ] || curl -fsSL -o "$1" "$2"
  echo "$3  $1" | sha256_check || { echo "$1 is not the file that was expected; remove it and run again" >&2; exit 1; }
}

# pyinstall <package...>: into the venv that runs StoneTracker's unpacker; from
# bbmp-sources, without the internet, when it is set (Cython and setuptools: the SDK's, else its wheels, below)
VENV="$SRC/amitools-venv"
pyinstall() {
  if kept; then
    [ -x "$VENV/bin/python" ] || python3 -m venv --system-site-packages "$VENV"
    if ! "$VENV/bin/python" -c "import setuptools, Cython" 2>/dev/null; then
      # setuptools and Cython build the packages: the Flatpak SDK's, else (macOS)
      # bbmp-sources' wheels
      kept_file setuptools-84.0.0-py3-none-any.whl >/dev/null
      kept_file cython-3.3.0-py3-none-any.whl >/dev/null
      "$VENV/bin/pip" install -q --no-index --find-links "$BBMP_SOURCES/archives" setuptools cython
    fi
    "$VENV/bin/pip" install -q --no-index --no-build-isolation --find-links "$BBMP_SOURCES/archives" "$@"
  else
    [ -x "$VENV/bin/python" ] || python3 -m venv "$VENV"
    "$VENV/bin/pip" install -q "$@"
  fi
}

# ---- UADE's source, for the include files the StoneTracker player is assembled with
if kept; then
  kept_unpack "uade-${UADE_COMMIT:0:7}.tar.xz" uade
elif [ ! -d uade/.git ]; then
  rm -rf uade
  git init -q uade
  git -C uade remote add origin https://gitlab.com/uade-music-player/uade.git
fi
if [ ! -f uade/amigasrc/score/misc/DeliPlayer.i ]; then
  git -C uade fetch -q --depth 1 origin "$UADE_COMMIT"
  git -C uade checkout -q --detach FETCH_HEAD
fi

# ---- ScummVM's MaxTrax player and Paula
mkdir -p scummvm-maxtrax/audio/mods
if kept; then
  kept_unpack "scummvm-maxtrax-${SCUMMVM_COMMIT:0:7}.tar.xz" scummvm-maxtrax-files
  cp scummvm-maxtrax-files/scummvm-* "$FILES/"
fi
for f in "maxtrax.cpp af1ff4f1ea8b04d3b5062234f2b635a39309f27871660c31b225c839542eeedd" \
         "maxtrax.h 7ab31fbcb0cb70c2e88c0ffe9d91e497b51d94de550cc645c94c257565e7d969" \
         "paula.cpp e34e9a744e188cde8cecf3411e8f81486b32a97bfbb03a88efd230021994198b" \
         "paula.h 9233cf55066d311a7c8395167cc1e365d5b120c3071468ddcb56b3465833883c"; do
  set -- $f
  fetch "$FILES/scummvm-$1" "https://raw.githubusercontent.com/scummvm/scummvm/$SCUMMVM_COMMIT/audio/mods/$1" "$2"
  cp "$FILES/scummvm-$1" "scummvm-maxtrax/audio/mods/$1"
done
# The original driver remembers the tempo whenever it sets it (SetTempo in max.asm),
# and a tempo that changes gradually starts from the remembered one. ScummVM's never
# remembers it, so every gradual change starts from its first value, 120, which
# stands for 7.5 beats a minute: songs with such changes crawl.
sed -i -E 's|_playerCtx\.tickUnit = calcTempo\(([^,]+), _playerCtx\.vBlankFreq\);|_playerCtx.tickUnit = calcTempo(_playerCtx.tempo = (\1), _playerCtx.vBlankFreq);  // modland: keep the tempo|' \
  scummvm-maxtrax/audio/mods/maxtrax.cpp
[ "$(grep -c 'modland: keep the tempo' scummvm-maxtrax/audio/mods/maxtrax.cpp)" = 3 ] || {
  echo "the tempo fix does not fit ScummVM's maxtrax.cpp" >&2; exit 1; }
# Four things a damaged or unusual file makes ScummVM's player crash on (none of
# Modland's songs has them; crafted files show them):
#  - a portamento time of 0 divides by zero when the note starts (the slide is over
#    at once then: its share of the pitch is 0);
#  - a mono channel looks for its voice from one past the last voice;
#  - a patch loaded twice frees its arrays with delete, not delete[];
#  - its Paula refuses a sample longer than 32 KB (an assert), which it plays fine.
sed -i -E \
  -e 's|/ channel\.portamentoTime;|/ MAX<int>(1, channel.portamentoTime);  // modland: time 0|' \
  -e 's|VoiceContext \*voice = ARRAYEND\(_voiceCtx\);|VoiceContext *voice = ARRAYEND(_voiceCtx) - 1;  // modland: the last voice|' \
  -e 's|for \(voiceNum = ARRAYSIZE\(_voiceCtx\); |for (voiceNum = ARRAYSIZE(_voiceCtx) - 1; |' \
  -e 's|delete (curPatch\.(attackPtr\|samplePtr));|delete[] \1;  // modland: delete[]|' \
  scummvm-maxtrax/audio/mods/maxtrax.cpp
sed -i -E 's|assert\(length < 32768/2\);|// modland: any length|' scummvm-maxtrax/audio/mods/paula.h
[ "$(grep -c 'modland: time 0\|modland: the last voice\|ARRAYSIZE(_voiceCtx) - 1; \|modland: delete\[\]' \
      scummvm-maxtrax/audio/mods/maxtrax.cpp)" = 7 ] && grep -q 'modland: any length' scummvm-maxtrax/audio/mods/paula.h || {
  echo "the fixes for damaged files do not fit ScummVM's maxtrax.cpp and paula.h" >&2; exit 1; }

# ---- StoneTracker's replay routine. It is in an archive of StoneTracker's own kind,
# which its own unpacker opens: an Amiga program, run with vamos (amitools).
# Its terms (the archive's readme on Aminet): "Feel free to provide these archives for
# download on your own site, as long as you retain this document." So the archive goes
# into the package whole and unchanged, with the readme, beside the routine taken out of it.
fetch "$FILES/stonefree2.lha" "https://aminet.net/mus/edit/stonefree2.lha" \
  3be292345dcb530bb923926e26dc873980570a5d450b5ef6f0bcf5c97a93f071
fetch "$FILES/stonefree2.readme" "https://aminet.net/mus/edit/stonefree2.readme" \
  d425e7739971299d86222fa86da8eb69af13667ce280b4646236421f1803b7eb
mkdir -p "$PREFIX/share/stonetracker"
cp "$FILES/stonefree2.lha" "$FILES/stonefree2.readme" "$PREFIX/share/stonetracker/"
cat > "$PREFIX/share/stonetracker/README.txt" <<'TXT'
StoneTracker songs are played by StoneTracker's own replay routine (StonePlayer_Hard.Bin,
version 1.98), which is in Players.LHS in this archive: stonefree2.lha, the free edition of
StoneTracker 1.26 by Emmanuel Marty and Michael Lavaire, as it is on Aminet
(https://aminet.net/mus/edit/stonefree2.lha). Its readme, stonefree2.readme, gives the terms:
the archive may be passed on as long as the readme goes with it.
TXT
STONE="$FILES/stonetracker"
if ! echo "16d70adb7f09fdf3fc7786f4b7cc7c988073c160ce9879e86c98b61e89f0c15c  $STONE/StonePlayer_Hard.bin" | sha256_check >/dev/null 2>&1; then
  rm -rf "$STONE"
  mkdir -p "$STONE/disk" "$STONE/out"
  if command -v lha >/dev/null; then
    lha xqw="$STONE/disk" "$FILES/stonefree2.lha" StoneEx Players.LHS
  else      # no lha: a reader for Python does it
    if kept; then kept_file lhafile-0.3.1.tar.gz >/dev/null; fi
    pyinstall "lhafile==0.3.1"
    "$SRC/amitools-venv/bin/python" - "$FILES/stonefree2.lha" "$STONE/disk" <<'PY'
import sys, lhafile
a = lhafile.Lhafile(sys.argv[1])
for name in ("StoneEx", "Players.LHS"):
    open(sys.argv[2] + "/" + name, "wb").write(a.read(name))
PY
  fi
  VAMOS="$(command -v vamos || true)"
  if [ -z "$VAMOS" ]; then
    if [ ! -x "$VENV/bin/vamos" ]; then
      if kept; then         # checked, and what building them needs (setuptools-scm)
        for f in "amitools-$AMITOOLS.tar.gz" machine68k-0.3.0.tar.gz setuptools_scm-10.3.4-py3-none-any.whl \
                 vcs_versioning-2.5.0-py3-none-any.whl packaging-26.3-py3-none-any.whl; do
          kept_file "$f" >/dev/null
        done
        pyinstall setuptools-scm==10.3.4
      fi
      pyinstall "machine68k==0.3.0"
      pyinstall "amitools==$AMITOOLS"
    fi
    VAMOS="$SRC/amitools-venv/bin/vamos"
  fi
  "$VAMOS" -q -C 68020 -s 64 -V "w:$STONE/disk" -V "o:$STONE/out" -- w:StoneEx FILE w:Players.LHS TO o: >/dev/null 2>&1 || true
  cp "$STONE/out/StonePlayer_Hard.bin" "$STONE/StonePlayer_Hard.bin"
  echo "16d70adb7f09fdf3fc7786f4b7cc7c988073c160ce9879e86c98b61e89f0c15c  $STONE/StonePlayer_Hard.bin" | sha256_check
fi

# ---- the assembler
ASM="$(command -v vasmm68k_mot || true)"
if [ -z "$ASM" ]; then
  if [ ! -x "$SRC/vasm/vasmm68k_mot" ]; then
    fetch "$FILES/$VASM.tar.gz" "http://phoenix.owl.de/tags/$VASM.tar.gz" \
      c84b2de1cbb87831795fe64a85c5d9a7002a766e3a7c30b0a2d7d5e99d878f49
    rm -rf "$SRC/vasm"
    tar xzf "$FILES/$VASM.tar.gz" -C "$SRC"
    make -C "$SRC/vasm" -j"$JOBS" CPU=m68k SYNTAX=mot >/dev/null
  fi
  ASM="$SRC/vasm/vasmm68k_mot"
fi

# ---- UADE's base folder with the StoneTracker player added. UADE's own files are
# linked, not copied, so that they stay UADE's when UADE is built again.
BASE="$PREFIX/share/uade"
rm -rf "$BASE"
mkdir -p "$BASE/players"
for f in score uaerc uade.conf; do ln -s "$UADE/share/uade/$f" "$BASE/$f"; done
for f in "$UADE"/share/uade/players/*; do ln -s "$f" "$BASE/players/$(basename "$f")"; done
rm -f "$BASE/players/StoneTracker"
"$ASM" -Fhunkexe -kick1hunks -nosym -m68020 -quiet -I "$SRC/uade/amigasrc/score" -I "$STONE" \
  -o "$BASE/players/StoneTracker" "$HERE/csrc/amiga/stonetracker_player.asm"
# eight voices are mixed by the 68020: UADE has to give it the speed ("speed_hack")
{ grep -v '^StoneTracker' "$UADE/share/uade/eagleplayer.conf"
  printf 'StoneTracker\t\tprefixes=spm\tspeed_hack\n'; } > "$BASE/eagleplayer.conf"
# Only players that may be passed on are in the base folder: UADE's that its build keeps
# (tools/build-uade.sh) and StoneTracker's. The others (ZoundMonitor's, Jesper Olsen's
# for the songs with their own routine, ...) are downloaded on request and found through
# BBMP_UADE_PLAYERS.
FINE_PLAYERS=(ArtOfNoise-4V ArtOfNoise-8V Fred JochenHippel_UADE ManiacsOfNoise MusicAssembler
  MED Octa-MED Octa-MED-Ss Medley MusiclineEditor MusicMaker-4V MusicMaker-8V Pokeynoise
  PreTracker SIDMon1.0 SoundMon2.0 SoundMon2.2 StoneTracker S)
left="$(cd "$BASE/players" && find . -mindepth 1 -maxdepth 1 | sed 's|^\./||' | LC_ALL=C sort | tr '\n' ' ')"
want="$(printf '%s\n' "${FINE_PLAYERS[@]}" | LC_ALL=C sort | tr '\n' ' ')"
[ "$left" = "$want" ] || {
  echo "bbmp-amiga's players folder holds more (or less) than the players that may be passed on" >&2
  echo "  there: $left" >&2; echo "  wanted: $want (run tools/build-uade.sh again?)" >&2; exit 1; }

# ---- the helper
BUILD="$SRC/modland-amiga-build"
rm -rf "$BUILD"
mkdir -p "$BUILD"
SHIM="$HERE/csrc/amiga/maxtrax_shim"
for f in maxtrax paula; do
  g++ -O2 -std=gnu++17 -w -DENABLE_KYRA -I"$SHIM" -I"$SRC/scummvm-maxtrax" \
    -c "$SRC/scummvm-maxtrax/audio/mods/$f.cpp" -o "$BUILD/scummvm-$f.o"
done
g++ -O2 -std=gnu++17 -Wall -o "$PREFIX/bin/bbmp-amiga" \
  -DENABLE_KYRA -I"$SHIM" -I"$SRC/scummvm-maxtrax" -I"$UADE/include" \
  "$HERE/csrc/amiga/bbmp-amiga.cpp" "$HERE/csrc/amiga/uade_engine.cpp" \
  "$HERE/csrc/amiga/maxtrax_engine.cpp" "$HERE/csrc/amiga/jo_engine.cpp" "$BUILD"/scummvm-*.o \
  -L"$UADE/lib" -Wl,-rpath,"$UADE/lib" -luade
echo "installed $PREFIX/bin/bbmp-amiga"
