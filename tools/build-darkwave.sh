#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-darkwave: DarkWave Studio projects (.dwp) played by DarkWave Studio's own
# engine. DarkWave Studio 4.1.6 (Daniel Werner, experimentalscene.com, 2012) was published
# with its source under the GPL 3; that source is kept by the Wayback Machine. Taken from
# it: the machines of ES-CoreMachines (BassHead, Stomp, DGenR8, Mixer, SpatialVerb,
# QuadDelay and what they are made of) and DarkPlug, the machines' interface, with their
# windows taken out (csrc/darkwave/darkwave-4.1.6-headless.patch). The studio itself (the
# project loader, the cables, patterns and sequence) is ported in csrc/darkwave/studio.cpp.
# No windows, audio devices or VST plug-ins. The result is GPL 3 or later.
#
# Sources go to ~/.local/src/darkwave-4.1.6, the result to ~/.local/opt/bbmp-darkwave.
# Needs curl, unzip, patch, g++.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
URL="https://web.archive.org/web/20130121175233id_/http://www.experimentalscene.com/downloads/DarkWave-Studio-4.1.6-Source.zip"
SHA256=db42a257cf11da396a0943551e89b9fbe07e270c7fd877d3dd489e5941cd2352
SRC="$BBMP_SRC/darkwave-4.1.6"
PREFIX="${1:-$BBMP_OPT/bbmp-darkwave}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MY="$HERE/csrc/darkwave"
mkdir -p "$SRC" "$PREFIX/bin"

ZIP="$SRC/DarkWave-Studio-4.1.6-Source.zip"
if kept; then
  kept_copy DarkWave-Studio-4.1.6-Source.zip "$ZIP"
elif ! echo "$SHA256  $ZIP" | sha256_check >/dev/null 2>&1; then
  curl -fsSL --retry 3 -o "$ZIP.part" "$URL"
  echo "$SHA256  $ZIP.part" | sha256_check \
    || { echo "the download is not the source zip this script was written for" >&2; exit 1; }
  mv "$ZIP.part" "$ZIP"
fi

# ---- the files used, with Linux line ends and includes by base name, then our changes
W="$SRC/build"
rm -rf "$W" "$SRC/unpacked"
mkdir -p "$W/src" "$W/obj" "$SRC/unpacked"
unzip -q "$ZIP" -d "$SRC/unpacked"
D="$SRC/unpacked/DarkWave-Studio"
MACHINES="BassHead Kick DrumKit Mixer SpatialVerb Echo VCO ADSR AD VCA MultiFilter Slide FormantFilter
  CombFilter EQ FIR FDN ImageSource DSP MinBLEP MinimumPhase DFT Blit Core"
SHARED="DarkPlug Util Wave FFT Error Array ListNode Complex PI RayTrace"
for m in $MACHINES Sinc FilterDesign; do
  for e in cpp h; do
    [ -f "$D/CoreMachines/Source/$m.$e" ] && sed 's/\r$//' "$D/CoreMachines/Source/$m.$e" > "$W/src/$m.$e"
  done
done
sed 's/\r$//' "$D/CoreMachines/Resource.h" > "$W/src/Resource.h"
for s in $SHARED; do
  for e in cpp h; do
    [ -f "$D/Shared/$s.$e" ] && sed 's/\r$//' "$D/Shared/$s.$e" > "$W/src/$s.$e"
  done
done
sed -i -E 's@^(#include +")([^"]*[/\\])?([^"/\\]+)"@\1\3"@' "$W"/src/*.cpp "$W"/src/*.h
patch -s -d "$W/src" -p1 < "$MY/darkwave-4.1.6-headless.patch"
grep -q DW_HEADLESS "$W/src/SpatialVerb.cpp" || { echo "the patch did not apply" >&2; exit 1; }

# the MinBLEP table was a resource of the DLL: linked in as it is
cp "$D/Resource/MinBLEP[32ZC,256xOS,64].dat" "$W/minblep.dat"
if [ "$(uname -s)" = Darwin ]; then        # Mach-O: its own section names, C names with _
  printf '.section __TEXT,__const\n.globl _DarkWaveMinBLEP\n.p2align 4\n_DarkWaveMinBLEP:\n.incbin "%s"\n' \
    "$W/minblep.dat" > "$W/minblep.s"
else
  printf '.section .rodata\n.globl DarkWaveMinBLEP\n.balign 16\nDarkWaveMinBLEP:\n.incbin "%s"\n.section .note.GNU-stack,"",@progbits\n' \
    "$W/minblep.dat" > "$W/minblep.s"
fi

# ---- compile, at most $JOBS at once
CXX="g++ -std=gnu++14 -O2 -w -fpermissive -DNDEBUG -DDW_HEADLESS -DCOREMACHINES"
INC="-include $MY/compat/windows.h -I$MY/compat -I$W/src -I$MY"
# on an ARM processor (Apple's) the few SSE2 instructions it uses come from sse2neon
# (MIT), which makes them NEON: xmmintrin.h and emmintrin.h lead to it
if [ "$(uname -m)" = arm64 ] || [ "$(uname -m)" = aarch64 ]; then
  NEON="$BBMP_SRC/sse2neon-1.8.0"
  mkdir -p "$NEON"
  if kept; then kept_copy sse2neon-1.8.0.h "$NEON/sse2neon.h"; fi
  [ -f "$NEON/sse2neon.h" ] || curl -sfL -o "$NEON/sse2neon.h" \
    https://raw.githubusercontent.com/DLTcollab/sse2neon/v1.8.0/sse2neon.h
  echo "07723c9f9457dd4316f1fde3dd4eb6f31dd67d9955f6c21f4e609ac1698be48a  $NEON/sse2neon.h" | sha256_check \
    || { echo "$NEON/sse2neon.h is not sse2neon 1.8.0" >&2; exit 1; }
  for h in xmmintrin.h emmintrin.h pmmintrin.h tmmintrin.h smmintrin.h mm_malloc.h; do printf '#include "sse2neon.h"\n' > "$NEON/$h"; done
  INC="-I$NEON $INC"
fi
# macOS has no malloc.h (what it would give is in stdlib.h)
if [ "$(uname -s)" = Darwin ]; then
  mkdir -p "$W/darwin"
  printf '#include <stdlib.h>\n' > "$W/darwin/malloc.h"
  INC="-I$W/darwin $INC"
fi
SOURCES=""
for m in $MACHINES; do [ -f "$W/src/$m.cpp" ] && SOURCES="$SOURCES $W/src/$m.cpp"; done
for s in DarkPlug Util Wave FFT Error RayTrace; do SOURCES="$SOURCES $W/src/$s.cpp"; done
SOURCES="$SOURCES $MY/studio.cpp $MY/bbmp-darkwave.cpp"
printf '%s\n' $SOURCES | xargs -P "$JOBS" -I{} bash -c '
  o="$0/$(basename "${2%.cpp}").o"
  $1 -c "$2" -o "$o" || { echo "failed: $2" >&2; exit 255; }' "$W/obj" "$CXX $INC" {}
g++ -c "$W/minblep.s" -o "$W/obj/minblep-table.o"   # not minblep.o: a Mac's disk takes it for MinBLEP.o
g++ -o "$PREFIX/bin/bbmp-darkwave" "$W"/obj/*.o -lm
sed 's/\r$//' "$D/LICENSE.txt" > "$PREFIX/COPYING.darkwave"
echo "installed $PREFIX/bin/bbmp-darkwave"
