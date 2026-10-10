#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-misc: EdLib songs (.edl, by way of AdPlug's D00 player), GoatTracker
# songs (.sng, GoatTracker's own play routines and reSID) and Sound Club songs (.sn,
# .sn2) in one helper binary.
# Sources go to ~/.local/src, the result to ~/.local/opt/bbmp-misc.
#
#   AdPlug, libbinio   LGPL 2.1   shared with bbmp-chip (~/.local/opt/adplug)
#   blast (zlib 1.3.1) zlib       unpacks PKWARE's "implode", which EdLib packs with
#   GoatTracker 2.77   GPL 2      gplay.c gsong.c gtable.c ginstr.c gsid.cpp, reSID
#   GoatTracker 1.53   GPL 2      the play routine out of goattrk.c (changed by gt1_patch.py
#                                 to write what its C64 routine writes), and the one of
#                                 GoatTracker 1.53 Stereo out of gstereo.c
#   Sound Club         own        sclub_cr.cpp, written for this player in a clean room;
#   Sound Club 2.03    freeware   no code of it; two data files out of its package (the
#                                 instrument bank, the FM waveforms) go to share/soundclub.
#                                 The package may be passed on only whole and unchanged,
#                                 so it is fetched from its publisher and the two files
#                                 are taken out of it here.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
SRC="$BBMP_SRC"
PREFIX="${1:-$BBMP_OPT/bbmp-misc}"
ADPLUG="$BBMP_OPT/adplug"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MINE="$HERE/csrc/misc"
BUILD="${BBMP_MISC_BUILD:-$SRC/modland-misc-build}"
mkdir -p "$SRC" "$PREFIX/bin" "$BUILD"
cd "$SRC"

# fetch <url> <file> <sha256>: download once, and only what was downloaded before
fetch() {
  if kept && [ ! -f "$2" ]; then kept_copy "$(basename "$2")" "$2"; fi    # the zips
  [ -f "$2" ] || curl -sSL --fail -o "$2.part" "$1"
  [ -f "$2" ] || mv "$2.part" "$2"
  echo "$3  $2" | sha256_check || { echo "$2 is not the file this script was written for" >&2; exit 1; }
}
# clone_at <repo> <folder> <commit>
clone_at() {
  if kept; then kept_unpack "$2-${3:0:7}.tar.xz" "$2"; return; fi
  [ -d "$2" ] && return
  git init -q "$2"
  git -C "$2" remote add origin "https://github.com/$1.git"
  git -C "$2" fetch -q --depth 1 origin "$3"
  git -C "$2" checkout -q FETCH_HEAD
}

# --- AdPlug, as bbmp-chip has it (build-chip.sh builds the same into the same place)
if [ ! -f "$ADPLUG/lib/libadplug$SO" ]; then
  clone_at adplug/libbinio libbinio 3baed731da6b44883c6820132fb1d2a44abc0939
  clone_at adplug/adplug adplug 1e6133f94abd99625c628fd01d7a9c8b681be7de
  for p in libbinio adplug; do
    cmake -S "$p" -B "$p/build" -DCMAKE_INSTALL_PREFIX="$ADPLUG" -DCMAKE_PREFIX_PATH="$ADPLUG" -DCMAKE_INSTALL_LIBDIR=lib \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
    cmake --build "$p/build" -j"$JOBS"
    cmake --install "$p/build"
  done
fi

# --- blast, from zlib's contrib
if kept; then kept_unpack zlib-blast-1.3.1.tar.xz zlib-blast-1.3.1; fi
mkdir -p zlib-blast-1.3.1
fetch https://raw.githubusercontent.com/madler/zlib/v1.3.1/contrib/blast/blast.c zlib-blast-1.3.1/blast.c \
  1ab3e479d342bfc144167b808fb00142264bc50f24a110ca88cc774e351c218e
fetch https://raw.githubusercontent.com/madler/zlib/v1.3.1/contrib/blast/blast.h zlib-blast-1.3.1/blast.h \
  9c1c422b76311d4cb06863ffc056668b6240f3dd998bc02e89ee590d482bfdc2

# --- GoatTracker 2.77 and 1.53
fetch "https://downloads.sourceforge.net/project/goattracker2/GoatTracker%202/2.77/GoatTracker_2.77.zip" \
  GoatTracker_2.77.zip 96c2bd6a6ab3aca2f5bb18b1c764ac6ea69ac245cae14002a72cd87c554561ef
fetch https://cadaver.github.io/tools/goattrk.zip GoatTracker_1.53.zip \
  f12df334df5b1749fa0660575d63520a5e064ed254e4390cede6b03f9808ecef
fetch https://cadaver.github.io/tools/gstereo.zip GoatTracker_1.53_Stereo.zip \
  cf3344ab270343d213c8fbd87adae5ca1e47bd54b2be9764ca07a10a5fc3c640
[ -f goattracker-1.53-stereo/src/gstereo.c ] || unzip -oq GoatTracker_1.53_Stereo.zip -d goattracker-1.53-stereo
[ -f goattracker-2.77/src/gplay.c ] || unzip -oq GoatTracker_2.77.zip -d goattracker-2.77
[ -f goattracker-1.53/src/goattrk.c ] || unzip -oq GoatTracker_1.53.zip -d goattracker-1.53
GT2="$SRC/goattracker-2.77/src"
GT1="$SRC/goattracker-1.53/src"
GT1S="$SRC/goattracker-1.53-stereo/src"

rm -rf "$BUILD"
mkdir -p "$BUILD/SDL" "$BUILD/gt" "$BUILD/obj"

# --- Sound Club's instrument bank and FM waveforms are not fetched here: the player
# offers to download them (Downloads in Settings, `bbmp download soundclub`)
rm -rf "$PREFIX/share/soundclub"

# GoatTracker's headers include BME's, and those SDL's, for types and key names only
{
  echo "/* made by build-misc.sh: what BME's headers need of SDL */"
  echo "#include <stdint.h>"
  echo "typedef uint8_t Uint8; typedef int8_t Sint8; typedef uint16_t Uint16; typedef int16_t Sint16;"
  echo "typedef uint32_t Uint32; typedef int32_t Sint32;"
  echo "typedef struct SDL_Surface SDL_Surface; typedef struct _SDL_Joystick SDL_Joystick;"
  echo "#define KMOD_SHIFT 1"
  echo "enum {"
  grep -oh "SDLK_[A-Za-z0-9_]*" "$GT2"/bme/*.h "$GT2"/*.c "$GT2"/*.h | sort -u | awk '{print "  " $1 " = " NR+1000 ","}'
  echo "};"
} > "$BUILD/SDL/SDL.h"

# version 1's play routine, its filter function and its frequency tables
# (the files have DOS line ends; a definition's line ends after the ")", a prototype's doesn't)
cut_player() {
  echo "/* made by build-misc.sh out of GoatTracker 1.53's $(basename "$1") (GPL 2) */"
  tr -d '\r' < "$1" | awk '
    /^unsigned char freqtbllo\[\] = \{$/ {on=1}  /^INSTRUCTION asmtable/ {on=0}
    /^unsigned char swapnybbles\(unsigned char n\)$/ {on=1}  /^void clearsong\(/ {on=0}
    /^void playroutine\(void\)$/ {on=2}  on==2 && /^unsigned char swapnybbles/ {on=1}
    /^void setfilter\(unsigned char filternum(, int c)?\)$/ {on=1}  /^int getfilteramount\(void\)$/ {on=0}
    on'
}
cut_player "$GT1/goattrk.c" > "$BUILD/gt1_play.inc"
cut_player "$GT1S/gstereo.c" > "$BUILD/gt1s_play.inc"
for f in gt1_play.inc gt1s_play.inc; do
  grep -q "^void playroutine" "$BUILD/$f" && grep -q "^void setfilter" "$BUILD/$f" \
    && grep -q "^unsigned char swapnybbles" "$BUILD/$f" && grep -q "freqtblhi" "$BUILD/$f" \
    || { echo "GoatTracker 1's source isn't what it was ($f)" >&2; exit 1; }
done
# ... changed to write what the C64 play routine writes (see gt1_patch.py)
python3 "$MINE/gt1_patch.py" "$BUILD/gt1_play.inc" "$BUILD/gt1s_play.inc"

CC="gcc -O2 -std=gnu17 -fPIC -w"
CXX="g++ -O2 -std=gnu++17 -fPIC -w"
# gsong.c's import of GTS2 songs converts the commands of a pattern's rows * 4 rows, past
# its end, and past the last pattern into the next array (ASan: global-buffer-overflow
# with 208 patterns). The rows past a pattern's end are still cleared (command 0) when
# it is converted, so converting only its own rows changes nothing else.
mkdir -p "$BUILD/gt2src"
tr -d '\r' < "$GT2/gsong.c" > "$BUILD/gt2src/gsong.c"
[ "$(grep -c '^        for (d = 0; d < length; d++)$' "$BUILD/gt2src/gsong.c")" = 1 ] \
  || { echo "GoatTracker 2's gsong.c isn't what it was" >&2; exit 1; }
sed -i 's|^        for (d = 0; d < length; d++)$|        for (d = 0; d < length / 4; d++)|' "$BUILD/gt2src/gsong.c"
# (and its conversions of old pulse speeds, of songs and instruments, double a negative
# number by shifting it: the same value, without the undefined behaviour)
[ "$(grep -c '^          speed <<= 1;$' "$BUILD/gt2src/gsong.c")" = 2 ] \
  || { echo "GoatTracker 2's gsong.c isn't what it was" >&2; exit 1; }
sed -i 's|^          speed <<= 1;$|          speed *= 2;|' "$BUILD/gt2src/gsong.c"
# gplay.c: a wavetable that runs on past its 255th entry wraps its pointer to 0 and reads
# the byte before the table (ASan); 0 is "no wavetable", so the jump test is left out there
tr -d '\r' < "$GT2/gplay.c" > "$BUILD/gt2src/gplay.c"
[ "$(grep -c '^        if (ltable\[WTBL\]\[cptr->ptr\[WTBL\]-1\] == 0xff)$' "$BUILD/gt2src/gplay.c")" = 1 ] \
  || { echo "GoatTracker 2's gplay.c isn't what it was" >&2; exit 1; }
sed -i 's|^        if (ltable\[WTBL\]\[cptr->ptr\[WTBL\]-1\] == 0xff)$|        if (cptr->ptr[WTBL] \&\& ltable[WTBL][cptr->ptr[WTBL]-1] == 0xff)|' "$BUILD/gt2src/gplay.c"
grep -q '^        if (cptr->ptr\[WTBL\] && ltable' "$BUILD/gt2src/gplay.c" \
  || { echo "the patch of gplay.c didn't apply" >&2; exit 1; }
# gsong.c reads the song through gtw_fopen (the song is in memory)
for f in gplay gsong gtable ginstr; do
  src="$GT2/$f.c"
  [ -f "$BUILD/gt2src/$f.c" ] && src="$BUILD/gt2src/$f.c"
  $CC -Dfopen=gtw_fopen -I"$BUILD" -I"$GT2/bme" -I"$GT2" -c "$src" -o "$BUILD/gt/$f.o"
done
$CC -I"$BUILD" -I"$GT2/bme" -I"$GT2" -I"$MINE" -c "$MINE/gt2_engine.c" -o "$BUILD/gt/gt2_engine.o"
$CC -I"$BUILD" -I"$GT1" -I"$MINE" -c "$MINE/gt1_engine.c" -o "$BUILD/gt/gt1_engine.o"
$CC -DGT1_STEREO -I"$BUILD" -I"$GT1S" -I"$MINE" -c "$MINE/gt1_engine.c" -o "$BUILD/gt/gt1s_engine.o"
$CXX -fpermissive -I"$GT2" -I"$MINE" -c "$GT2/gsid.cpp" -o "$BUILD/gt/gsid.o"
$CXX -I"$GT2" -I"$MINE" -c "$MINE/gt_sid.cpp" -o "$BUILD/gt/gt_sid.o"
for f in "$GT2"/resid/*.cpp "$GT2"/resid-fp/*.cpp; do
  case "$f" in */version.cpp|*/versionfp.cpp) continue ;; esac
  $CXX -fpermissive -c "$f" -o "$BUILD/gt/$(basename "${f%.cpp}").o"
done
# One object whose only public names are gt1w_*, gt1sw_*, gt2w_* and gtw_*: the editor's
# globals (chn, instr, pattern, key...) stay inside.
one_object "$BUILD/obj/gt.o" 'gt1w_*' 'gt1sw_*' 'gt2w_*' 'gtw_*' -- "$BUILD"/gt/*.o

gcc -O2 -std=gnu17 -fPIC -c zlib-blast-1.3.1/blast.c -o "$BUILD/obj/blast.o"

# EdLib's songs: the converter to D00, and Sound Club's songs (both written for this player
# in a clean room)
EDLIB=(-DBBMP_EDLIB "$MINE/edl_cr.cpp")
g++ -O2 -std=gnu++17 -Wall -o "$PREFIX/bin/bbmp-misc" \
  "$MINE/bbmp-misc.cpp" "${EDLIB[@]}" "$MINE/goattracker.cpp" "$MINE/sclub_cr.cpp" \
  -I"$MINE" -Izlib-blast-1.3.1 "$BUILD/obj/blast.o" "$BUILD/obj/gt.o" \
  -I"$ADPLUG/include" -L"$ADPLUG/lib" -Wl,-rpath,"$ADPLUG/lib" -ladplug -llibbinio -lm $ICONV
echo "installed $PREFIX/bin/bbmp-misc"
