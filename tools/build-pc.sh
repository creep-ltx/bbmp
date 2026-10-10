#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-pc: replayers of formats from PC (and Mac) trackers in one helper.
#   unitrk     MikMod UNITRK (*.uni), played by libmikmod 3.3.14 (LGPL 2.1)
#   playerpro  PlayerPRO MAD (*.mad), played by PlayerPROCore (public domain)
#   buzz       Buzz songs (*.bmx) that use only BmxPlay's machines, played by BmxPlay
#              (public domain)
#   dsm        DSIK's first DSM format, rewritten as RIFF DSMF for libopenmpt
#   mxm        Cubic Tiny XM (*.mxm), rewritten as XM for libopenmpt
# Sources go to ~/.local/src, the result to ~/.local/opt/bbmp-pc.
# Needs the system's libopenmpt and libsamplerate (as the player itself does).
set -euo pipefail
. "$(dirname "$0")/sources.sh"
SRC="$BBMP_SRC"
PREFIX="${1:-$BBMP_OPT/bbmp-pc}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
PC="$HERE/csrc/pc"
OBJ="$SRC/modland-pc-build"
mkdir -p "$SRC" "$PREFIX/bin" "$OBJ"
cd "$SRC"

# fetch <github owner/repo> <folder> <commit>: that commit and nothing else
fetch() {
  if kept; then kept_unpack "$2-${3:0:7}.tar.xz" "$2"; return; fi
  if [ ! -d "$2/.git" ]; then
    git init -q "$2"
    git -C "$2" remote add origin "https://github.com/$1.git"
  fi
  if [ "$(git -C "$2" rev-parse -q --verify HEAD 2>/dev/null || true)" != "$3" ]; then
    git -C "$2" fetch -q --depth 1 origin "$3"
    git -C "$2" checkout -q --detach FETCH_HEAD
  fi
}

ENGINES=""
OBJS=""
LIBS=""
cc1() {   # cc1 <source> [flags]: compile one of our C sources
  local o="$OBJ/$(basename "${1%.*}").o"
  gcc -O2 -std=gnu17 -Wall -I"$PC" "${@:2}" -c "$1" -o "$o"
  OBJS="$OBJS $o"
}
cxx1() {  # the same for C++
  local o="$OBJ/$(basename "${1%.*}").o"
  g++ -O2 -std=gnu++17 -Wall -I"$PC" "${@:2}" -c "$1" -o "$o"
  OBJS="$OBJS $o"
}
# bundle <engine> <objects...>: one object whose only visible names are the
# engine's pcw_<engine>_*, so that the replayers' global names can't collide
bundle() {
  local name="$1"; shift
  rm -f "$OBJ/$name.ro"
  one_object "$OBJ/$name.ro" "pcw_${name}_*" -- "$@"
  OBJS="$OBJS $OBJ/$name.ro"
}

cxx1 "$PC/openmpt.cpp"

# --- unitrk: libmikmod 3.3.14, static, with no sound drivers but "nosound"
fetch sezero/mikmod mikmod 8de19eef766d9ba30a74c19e566edfcd61851dba
if [ ! -f "$OBJ/mikmod/libmikmod.a" ]; then
  cmake -S mikmod/libmikmod -B "$OBJ/mikmod" -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DENABLE_DOC=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DENABLE_ALSA=OFF -DENABLE_PULSE=OFF -DENABLE_OSS=OFF -DENABLE_SDL=OFF \
    -DENABLE_OPENAL=OFF -DENABLE_ESD=OFF -DENABLE_NAS=OFF -DENABLE_SNDIO=OFF -DENABLE_AIFF=OFF \
    -DENABLE_WAV=OFF -DENABLE_RAW=OFF -DENABLE_PIPE=OFF -DENABLE_STDOUT=OFF \
    -DCMAKE_C_FLAGS="-std=gnu17 -w" >/dev/null
  cmake --build "$OBJ/mikmod" -j"$JOBS" >/dev/null
fi
cc1 "$PC/unitrk_engine.c" -I"$OBJ/mikmod" -I"$SRC/mikmod/libmikmod/include"
LIBS="$LIBS $OBJ/mikmod/libmikmod.a"
ENGINES="$ENGINES UNITRK"

# --- playerpro: PlayerPROCore's driver and its importers of the older MAD formats,
# without a sound output and without its loader of plug-ins (playerpro_engine.c has that)
fetch MaddTheSane/PlayerPRO PlayerPRO 0a07010443053f54636adcd2d0460d1196a6fd88
PPRO="$SRC/PlayerPRO/MADDriver.source"
# a variable declared twice, in a part that only little-endian machines compile
sed -i '/^\t\t\t\tshort \*shortPtr = (short\*)dataCopy;$/d' "$PPRO/MainDriver.c"
# The importer of MAD-F and MAD-G looks a sample's finetune up in a table of 16 on
# its stack with the byte as the file has it; a song on Modland has 32 there and
# got a rate that changed from run to run. The low four bits, as in a MOD.
sed -i 's/finetune\[oldMAD->fid\[i\]\.fineTune\];$/finetune[oldMAD->fid[i].fineTune \& 15]; \/* modland *\//' \
  "$PPRO/Import-Export/MADfg/MADfg.c"
grep -q 'fineTune & 15]; /\* modland' "$PPRO/Import-Export/MADfg/MADfg.c"
mkdir -p "$OBJ/playerpro/inc"
ln -sfn "$PPRO" "$OBJ/playerpro/inc/PlayerPROCore"
# MADCFReadStreamType is declared for macOS only; the endianness macros are Apple's
PPRO_FLAGS="-O2 -std=gnu17 -fPIC -w -D__UNIX__ -D__LITTLE_ENDIAN__=1 -DBUILDINGPPRO -DEMBEDPLUGS=1
  -I$PPRO -I$OBJ/playerpro/inc"
[ "$(uname -s)" = Darwin ] || PPRO_FLAGS="$PPRO_FLAGS -DMADCFReadStreamType=0x7fff"
for f in DelayOutPut Effects MainDriver FileUtils Interrupt MADDriver MyDebugStr OutPut8 realft \
         TickRemover stub-VSTPlugIn MIDI-Hardware-Stub; do
  # shellcheck disable=SC2086
  gcc $PPRO_FLAGS -c "$PPRO/$f.c" -o "$OBJ/playerpro/$f.o"
done
for p in MADfg MADH MADI; do
  # shellcheck disable=SC2086
  gcc $PPRO_FLAGS "-DFillPlug=FillPlug_$p" -c "$PPRO/Import-Export/$p/$p.c" -o "$OBJ/playerpro/$p.o"
done
# shellcheck disable=SC2086
gcc $PPRO_FLAGS -I"$PC" -c "$PC/playerpro_engine.c" -o "$OBJ/playerpro/playerpro_engine.o"
if [ "$(uname -s)" = Darwin ]; then       # what its macOS build asks for and never uses here
  # shellcheck disable=SC2086
  gcc $PPRO_FLAGS -c "$PC/playerpro_mac.c" -o "$OBJ/playerpro/playerpro_mac.o"
fi
bundle playerpro "$OBJ"/playerpro/*.o
ENGINES="$ENGINES PLAYERPRO"

# --- buzz: BmxPlay, without its sound output (which would open /dev/dsp)
fetch joric/bmxplay bmxplay e77f921f35caf075876d676a87cc84161b132416
mkdir -p "$OBJ/buzz/inc"
sed 's|#include "waveout_linux.h"|#define BUFSIZE 4096|' bmxplay/src/bmxplay.h > "$OBJ/buzz/inc/bmxplay.h"
grep -q '#define BUFSIZE 4096' "$OBJ/buzz/inc/bmxplay.h"
# and three of its headers patched, found in inc/ before src/ (bmxmlist.h is copied
# there so that its machines are looked for there first):
# - bmx_alloc: zeroed and with room behind it: _xi reads a byte past its samples
#   of 4 bits, and _filter uses its state before a song has set all of it
# - _xi: a sample that plays backwards (ping-pong) past its start
# - _delay: a delay of more than 128/128 of its buffer (a byte in the file)
cp bmxplay/src/bmxmlist.h "$OBJ/buzz/inc/bmxmlist.h"
sed 's|return malloc(bytes);|return calloc((size_t)bytes + 16, 1); /* modland */|' \
  bmxplay/src/bmxmi.h > "$OBJ/buzz/inc/bmxmi.h"
# a machine's header (its name and three functions) is packed with the file structures,
# so its pointers sit at odd addresses, which Apple's ARM linker can't fill in: there it
# is laid out as usual (it is never read from a file)
if [ "$(uname -s)" = Darwin ]; then
  python3 - "$OBJ/buzz/inc/bmxmi.h" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
head = "typedef struct\n{\n\tconst char *dllname;"
assert head in s and "} BmxMachineHeader;" in s, "bmxmi.h is not as expected"
s = s.replace(head, "#pragma pack(push, 8)\n" + head, 1)
s = s.replace("} BmxMachineHeader;", "} BmxMachineHeader;\n#pragma pack(pop)", 1)
open(p, "w").write(s)
PY
fi
sed 's|if (index < length && t->play)|if (index >= 0 \&\& index < length \&\& t->play) /* modland */|' \
  bmxplay/src/_xi.h > "$OBJ/buzz/inc/_xi.h"
sed 's|m->length = gp->length/128.0f;|m->length = (gp->length > 0x80 ? 0x80 : gp->length)/128.0f; /* modland */|' \
  bmxplay/src/_delay.h > "$OBJ/buzz/inc/_delay.h"
for h in bmxmi.h _xi.h _delay.h; do grep -q 'modland' "$OBJ/buzz/inc/$h"; done
gcc -O2 -std=gnu17 -fPIC -w -I"$OBJ/buzz/inc" -I"$SRC/bmxplay/src" -I"$PC" \
  -c "$PC/buzz_engine.c" -o "$OBJ/buzz/buzz_engine.o"
bundle buzz "$OBJ/buzz/buzz_engine.o"
ENGINES="$ENGINES BUZZ"

# --- dsm, mxm: our own converters, played by the system's libopenmpt
cxx1 "$PC/dsm_engine.cpp"
cxx1 "$PC/mxm_engine.cpp"
ENGINES="$ENGINES DSM MXM"

DEFS=""
for e in $ENGINES; do DEFS="$DEFS -DWITH_$e"; done
# shellcheck disable=SC2086
g++ -O2 -std=gnu++17 -Wall $DEFS -I"$PC" -o "$PREFIX/bin/bbmp-pc" "$PC/bbmp-pc.cpp" \
  $OBJS $LIBS -lopenmpt -lsamplerate -lm -lpthread \
  $( [ "$(uname -s)" = Darwin ] && echo -framework CoreFoundation)   # PlayerPRO uses it on a Mac
echo "installed $PREFIX/bin/bbmp-pc"
