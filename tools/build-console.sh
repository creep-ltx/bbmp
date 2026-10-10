#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-console: rips of console sound hardware in one helper binary.
#   wsr  Bandai WonderSwan (.wsr): Mednafen's WonderSwan emulation, in the form
#        foo_input_wsr has it (no video), taken from rePlayer's source tree. GPL 2.
#   spu  PlayStation SPU logs (.spu): the P.E.Op.S. SPU of the Audio Overload SDK.
#        GPL 2 or later. Only the SPU is taken, not the SDK's player for the format.
# Sources go to ~/.local/src, the result to ~/.local/opt/bbmp-console.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
SRC="$BBMP_SRC"
PREFIX="${1:-$BBMP_OPT/bbmp-console}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
REPLAYER=e3bf02cb41a40cdaebf8dc80efc0ae5ac488dc38
AOSDK=e359a6e5154b2ba8499fb1f24a1f5f8a18538a61
mkdir -p "$SRC" "$PREFIX/bin"
cd "$SRC"

# rePlayer is large; only its WonderSwan replay is fetched
if kept; then
  kept_unpack "rePlayer-${REPLAYER:0:7}.tar.xz" replayer-wonderswan
  kept_unpack "aosdk-${AOSDK:0:7}.tar.xz" aosdk
else
if [ ! -d replayer-wonderswan/.git ]; then
  rm -rf replayer-wonderswan
  git init -q replayer-wonderswan
  git -C replayer-wonderswan remote add origin https://github.com/arnaud-neny/rePlayer.git
  git -C replayer-wonderswan sparse-checkout set source/Replays/WonderSwan
fi
if [ "$(git -C replayer-wonderswan rev-parse -q --verify HEAD || true)" != "$REPLAYER" ]; then
  git -C replayer-wonderswan fetch -q --depth 1 --filter=blob:none origin "$REPLAYER"
  git -C replayer-wonderswan checkout -q --detach FETCH_HEAD
fi
if [ ! -d aosdk/.git ]; then
  rm -rf aosdk
  git init -q aosdk
  git -C aosdk remote add origin https://github.com/nmlgc/aosdk.git
fi
if [ "$(git -C aosdk rev-parse -q --verify HEAD || true)" != "$AOSDK" ]; then
  git -C aosdk fetch -q --depth 1 origin "$AOSDK"
  git -C aosdk checkout -q --detach FETCH_HEAD
fi
fi

MDFN=replayer-wonderswan/source/Replays/WonderSwan/mednafen
BUILD="$SRC/modland-console-build"
rm -rf "$BUILD"
mkdir -p "$BUILD"
CXXFLAGS="-O2 -std=gnu++17 -w -DLSB_FIRST -DMDFN_DISABLE_PICPIE_ERRWARN"
for f in error sound/Blip_Buffer wswan/interrupt wswan/main wswan/memory wswan/sound wswan/v30mz wswan/tcache; do
  g++ $CXXFLAGS -c "$MDFN/$f.cpp" -o "$BUILD/mdfn-$(basename "$f").o"
done
# gfx.cpp is what runs the CPU, line by line. Its calls of v30mz_execute are sent to
# the engine, which skips the time that a song waits for the next interrupt.
g++ $CXXFLAGS -Dv30mz_execute=v30mz_execute_hooked -c "$MDFN/wswan/gfx.cpp" -o "$BUILD/mdfn-gfx.o"
g++ $CXXFLAGS -I"$MDFN" -I"$HERE/csrc/console" -c "$HERE/csrc/console/wsr_engine.cpp" -o "$BUILD/wsr_engine.o"

gcc -O2 -std=gnu17 -w -DLSB_FIRST=1 -Iaosdk -Iaosdk/eng_psf -c aosdk/eng_psf/peops/spu.c -o "$BUILD/peops-spu.o"

g++ -O2 -std=gnu++17 -Wall -o "$PREFIX/bin/bbmp-console" -I"$HERE/csrc/console" \
  "$HERE/csrc/console/bbmp-console.cpp" "$HERE/csrc/console/spu_engine.cpp" \
  "$BUILD"/wsr_engine.o "$BUILD"/mdfn-*.o "$BUILD/peops-spu.o" -lm $ICONV
# the licences, the credits (README.txt: where each part comes from, with the V30MZ's
# credit to Bryan McPhail) go with the program: the GPL 2 (the whole), the LGPL 2.1
# (Blip_Buffer), the P.E.Op.S. SPU's License.txt and the Audio Overload SDK's licence (ao.h,
# corlett.h)
mkdir -p "$PREFIX/share/bbmp-console"
cp "$HERE/csrc/console/README.txt" "$HERE/licences/GPL-2.0.txt" "$HERE/licences/LGPL-2.1.txt" \
  "$PREFIX/share/bbmp-console/"
cp aosdk/eng_psf/peops/License.txt "$PREFIX/share/bbmp-console/peops-License.txt"
cp aosdk/license.txt "$PREFIX/share/bbmp-console/aosdk-license.txt"
echo "installed $PREFIX/bin/bbmp-console"
