#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-ay: ZX Spectrum and Amstrad CPC formats that ZXTune doesn't play.
#   beepola  Beepola songs (.bbsong), ZX Spectrum beeper music: written for this
#            player in a clean room (beepola_engine.cpp), a timed step machine. The
#            Spectrum ROM, Phaser1's drum samples and the Savage table's tail are not
#            built in: the engine reads them from BBMP_BEEPOLA_DATA (the player
#            fetches them on first use).
#   fls fxm amad strc vt2   AY formats, as far as csrc/ay has an engine for them,
#            with the Ayumi chip emulation by Peter Sovietov (MIT). strc (stc_cr.cpp)
#            was written for this player in a clean room.
#   sks aks  Starkos and Arkos Tracker songs, by Arkos Tracker 3's own code
#            (Julien Nevo, MIT; it fetches JUCE 7.0.12 itself), built as a second
#            program, bbmp-ay-at3, which bbmp-ay starts for these engines.
# Sources go to ~/.local/src, the result to ~/.local/opt/bbmp-ay.
# Arkos Tracker 3 needs cmake and the headers JUCE wants (freetype, X11, ALSA);
# with SKIP_AT3=1 it is left out and only bbmp-ay is built.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
SRC="$BBMP_SRC"
PREFIX="${1:-$BBMP_OPT/bbmp-ay}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
AY="$HERE/csrc/ay"
AYUMI=07c08b4874c359169e4a028edf73f046d8b763e2
AT3=14d975bdf0787cde4a5fa9425abe793e4a3a3536
# at most JOBS compilers at once (default 4): Arkos Tracker 3 with JUCE and ZXTune are
# big builds, and one compiler per core took the desktop's memory
mkdir -p "$SRC" "$PREFIX/bin"
cd "$SRC"

# fetch <url> <folder> <commit>: the folder at exactly that commit
fetch() {
  if kept; then kept_unpack "$2-${3:0:7}.tar.xz" "$2"; return; fi
  if [ ! -d "$2/.git" ]; then
    rm -rf "$2"
    git init -q "$2"
    git -C "$2" remote add origin "$1"
  fi
  if [ "$(git -C "$2" rev-parse -q --verify HEAD || true)" != "$3" ]; then
    git -C "$2" fetch -q --depth 1 origin "$3"
    git -C "$2" checkout -q --detach FETCH_HEAD
  fi
}
fetch https://github.com/true-grue/ayumi.git ayumi "$AYUMI"

BUILD="$SRC/modland-ay-build"
rm -rf "$BUILD"
mkdir -p "$BUILD"
gcc -O2 -std=gnu17 -c ayumi/ayumi.c -o "$BUILD/ayumi.o"
DEFS=()
SRCS=()
AYCXX=g++
[ "$(uname -s)" = Darwin ] && AYCXX="$BREW/opt/llvm/bin/clang++"
# engine name and its source (beepola and strc: written for this player in a clean room)
for e in beepola:beepola_engine fls:fls_engine fxm:fxm_engine strc:stc_cr; do
  if [ -f "$AY/${e#*:}.cpp" ]; then
    DEFS+=("-DWITH_$(echo "${e%%:*}" | tr a-z A-Z)")
    if [ "${e%%:*}" = beepola ]; then
      # no fused multiply-add: its filter must round exactly as written
      $AYCXX -O2 -std=gnu++17 -Wall -ffp-contract=off -I"$AY" -c "$AY/${e#*:}.cpp" -o "$BUILD/beepola.o"
      SRCS+=("$BUILD/beepola.o")
    else
      SRCS+=("$AY/${e#*:}.cpp")
    fi
  fi
done
$AYCXX -O2 -std=gnu++17 -Wall "${DEFS[@]}" -I"$AY" -Iayumi -o "$PREFIX/bin/bbmp-ay" \
  "$AY/bbmp-ay.cpp" "$AY/aychip.cpp" "${SRCS[@]}" "$BUILD/ayumi.o" -lm -pthread
echo "installed $PREFIX/bin/bbmp-ay"

if [ "${SKIP_AT3:-0}" != 1 ]; then
  fetch https://bitbucket.org/JulienNevo/arkostracker3.git arkostracker3 "$AT3"
  # the helper is built as one more command line tool inside Arkos Tracker's tree
  rm -rf arkostracker3/source/commandLineTools/modlandAt3
  mkdir -p arkostracker3/source/commandLineTools/modlandAt3
  cp "$AY/at3/CMakeLists.txt" "$AY/at3/bbmp-ay-at3.cpp" arkostracker3/source/commandLineTools/modlandAt3/
  grep -q modlandAt3 arkostracker3/CMakeLists.txt || \
    sed -i 's|^add_subdirectory(source/commandLineTools/songToWav)|&\nadd_subdirectory(source/commandLineTools/modlandAt3)|' \
      arkostracker3/CMakeLists.txt
  # Damaged Starkos songs (found by fuzzing): a track list that runs past the end of the
  # file is read for ever (a read past the end gives 0, a valid track number), and an
  # instrument numbered 0 or twice is refused by the song builder, which leaves no
  # instrument for the reader to fill (a null pointer). Each patch is checked to apply.
  python3 - arkostracker3/source/import/starkos <<'PY'
import sys
d = sys.argv[1]
def patch(name, old, new):
    p = d + '/' + name
    s = open(p).read()
    if new in s:
        return
    if s.count(old) != 1:
        sys.exit('build-ay: the patch of %s does not apply' % name)
    open(p, 'w').write(s.replace(old, new))
patch('TrackReader.cpp', '    while (trackIndex < 0xffff) {',
      '    while (readSuccess && trackIndex < 0xffff) {      // modland: not past the end')
patch('TrackReader.cpp', '    while (specialTrackIndex < 0xffff) {',
      '    while (success && specialTrackIndex < 0xffff) {      // modland: not past the end')
patch('InstrumentReader.cpp', '#include "InstrumentReader.h"\n',
      '#include "InstrumentReader.h"\n\n#include <set>\n')
patch('InstrumentReader.cpp',
      '    while (instrumentIndex < 0xffff) {          // 0xffff marks the end of the Instrument section.\n',
      '    std::set<int> modlandSeen;\n'
      '    while (instrumentIndex < 0xffff) {          // 0xffff marks the end of the Instrument section.\n'
      '        // modland: the builder refuses 0 and a number met before, and has no instrument then\n'
      '        if (instrumentIndex == 0 || !modlandSeen.insert(instrumentIndex).second) {\n'
      '            builder.addError("Invalid PSG instrument number " + juce::String(instrumentIndex));\n'
      '            return;\n'
      '        }\n')
PY
  AT3_OPTS=()
  if kept; then
    # JUCE and CPM.cmake, which its CMake would download, from bbmp-sources (CMake
    # doesn't download a file that is there with the right checksum)
    kept_unpack JUCE-2d14a1f.tar.xz JUCE
    kept_copy CPM-0.40.8.cmake arkostracker3/build-modland/cmake/CPM.cmake
    AT3_OPTS=(-DCPM_JUCE_SOURCE="$SRC/JUCE")
  fi
  # on macOS its CMake asks for both processors in one (and garbles the list) unless
  # told by its own switch to build for ARM alone
  [ "$(uname -s)" = Darwin ] && export USE_ONLY_ARM64=1
  cmake -S arkostracker3 -B arkostracker3/build-modland -DCMAKE_BUILD_TYPE=Release "${AT3_OPTS[@]}" \
    > "$BUILD/at3-cmake.log" || { tail -30 "$BUILD/at3-cmake.log"; exit 1; }
  cmake --build arkostracker3/build-modland --target ModlandAt3 -j"$JOBS" > "$BUILD/at3-build.log" 2>&1 \
    || { tail -30 "$BUILD/at3-build.log"; exit 1; }
  install -m755 arkostracker3/build-modland/source/commandLineTools/modlandAt3/ModlandAt3_artefacts/Release/ModlandAt3 \
    "$PREFIX/bin/bbmp-ay-at3"
  echo "installed $PREFIX/bin/bbmp-ay-at3"
fi

# bbmp-ay-vt2 is built as one more program inside ZXTune's source tree, as
# bbmp-zxtune is (tools/build-zxtune.sh, which also finds the Boost headers).
# ZXTune is taken as that script left it; when it has to be fetched here, this is
# the commit the helper was made with.
if [ "${SKIP_VT2:-0}" != 1 ]; then
  ZXTUNE="${ZXTUNE_SRC:-$SRC/zxtune}"
  ZXTUNE_COMMIT=e36902fe1cc06d9e73116db644b688708a7745d4
  if kept; then
    kept_unpack "zxtune-${ZXTUNE_COMMIT:0:7}.tar.xz" "$ZXTUNE"
  elif [ ! -d "$ZXTUNE" ]; then
    git init -q "$ZXTUNE"
    git -C "$ZXTUNE" remote add origin https://github.com/vitamin-caig/zxtune.git
    git -C "$ZXTUNE" fetch -q --depth 1 origin "$ZXTUNE_COMMIT"
    git -C "$ZXTUNE" checkout -q --detach FETCH_HEAD
  fi
  BOOST_INC=/usr/include
  if kept; then
    BOOST_INC="$SRC/boost-1.92.0"
    [ -d "$BOOST_INC/boost" ] || tar -xf "$(kept_file boost-1.92.0-b2-nodocs.tar.xz)" -C "$SRC" boost-1.92.0/boost
  elif [ "$(uname -s)" = Darwin ]; then
    BOOST_INC="$BREW/include"                    # Homebrew's (brew install boost)
  elif [ ! -d /usr/include/boost ]; then
    BOOST_INC="$BBMP_OPT/boost/usr/include"
    [ -d "$BOOST_INC/boost" ] || { echo "no Boost headers: run tools/build-zxtune.sh first" >&2; exit 1; }
  fi
  # the same changes to ZXTune as tools/build-zxtune.sh makes (Pro Tracker 2 and 3 as
  # their own players play them; no UnRAR and no vgmstream, whose terms can't go with the GPL,
  # and no AY Emul player, whose Z80~Ex is GPL 2 only, nor libvgm, GPL 2 only in part; and
  # without Sony's PlayStation 2 BIOS, which the user provides)
  for P in "$HERE"/csrc/zxtune/*.patch; do
    if ! patch -R -p1 -s -f --dry-run -d "$ZXTUNE" < "$P" >/dev/null 2>&1; then
      patch -p1 -s -d "$ZXTUNE" < "$P"
    fi
  done
  rm -f "$ZXTUNE/src/module/players/xsf/scph10000_he.inc"
  # nor ZXTune's wrappers around UnRAR, vgmstream and libvgm (as tools/build-zxtune.sh)
  rm -f "$ZXTUNE/src/formats/archived/generic/rar.cpp" "$ZXTUNE/src/module/players/external/vgmstream.cpp" \
    "$ZXTUNE/src/module/players/external/libvgm.cpp"
  # an object built before still names it as a dependency: built again
  grep -rl --include='*.d' scph10000_he.inc "$ZXTUNE/obj" 2>/dev/null | while read -r D; do rm -f "$D" "${D%.d}.o"; done || true
  mkdir -p "$ZXTUNE/apps/bbmp-ay-vt2"
  cp "$AY/vt2/main.cpp" "$AY/vt2/Makefile" "$ZXTUNE/apps/bbmp-ay-vt2/"
  ZXPLATFORM=linux
  [ "$(uname -s)" = Darwin ] && ZXPLATFORM=darwin   # ZXTune's own macOS settings (Apple's linker)
  make -C "$ZXTUNE/apps/bbmp-ay-vt2" -j"$JOBS" platform=$ZXPLATFORM release=1 \
    system.zlib=1 cxx_flags="-isystem $BOOST_INC -include $HERE/csrc/zxtune/stdint-both.h" > "$BUILD/vt2-build.log" 2>&1 \
    || { tail -30 "$BUILD/vt2-build.log"; exit 1; }
  install -m755 "$(find "$ZXTUNE/bin" -name bbmp-ay-vt2 -type f | head -1)" "$PREFIX/bin/"
  echo "installed $PREFIX/bin/bbmp-ay-vt2"
fi
