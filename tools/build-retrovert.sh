#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-rv, a host for Retrovert's playback plugins with the player's own iXalance
# engine built in (.ixs, csrc/retrovert/ixs_engine.cpp), and the plugin the player uses:
# Klystrack (.kt). Retrovert is Daniel Collin's player, https://github.com/RetrovertApp,
# with one repository per replayer.
# Sources go to ~/.local/src/retrovert, the result to ~/.local/opt/bbmp-rv.
#
# Licences: the host interface and Klystrack are MIT, and so are bbmp-rv's own files
# (csrc/retrovert: the host and the iXalance engine, written for this player). bbmp-rv
# is a program of its own with nothing under the GPL in it; the build stops if a GPL
# file turns up among its sources. It links zlib (zlib licence). The licences and
# README.txt go to share/bbmp-rv.
#
# The iXalance engine is exact only without fused multiply-add (-ffp-contract=off) and
# with the C library's sin, pow and fmod as glibc has them. Three samples a few songs
# use are not part of it: it reads them from the folder BBMP_IXS_DATA names.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
SRC="$BBMP_SRC/retrovert"
PREFIX="${1:-$BBMP_OPT/bbmp-rv}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$SRC" "$PREFIX/bin" "$PREFIX/plugins"
cd "$SRC"

# name, commit
PLUGINS=(
  "klystrack aad080d91322f1b6709401e30742a4ba3bbc1e40"
)

for entry in "${PLUGINS[@]}"; do
  read -r name commit <<<"$entry"
  dir="playback-$name"
  if kept; then
    kept_unpack "retrovert-$dir-${commit:0:7}.tar.xz" "$dir"
  else
  if [ ! -d "$dir" ]; then
    git clone --recurse-submodules "https://github.com/RetrovertApp/$dir.git" "$dir"
  fi
  if [ "$(git -C "$dir" rev-parse HEAD)" != "$commit" ]; then
    git -C "$dir" fetch --quiet origin "$commit"
    git -C "$dir" checkout --quiet "$commit"
    git -C "$dir" submodule update --init --recursive --quiet
  fi
  fi
  # klystron (Klystrack's engine) checks what a damaged file gives it
  # (csrc/retrovert/klystron-damaged-files.patch, the final audit of 7 October 2026): the
  # plugin's CMake applies it with its own two after it unpacks klystron
  if [ "$name" = klystrack ]; then
    cp "$HERE/csrc/retrovert/klystron-damaged-files.patch" "$dir/patches/"
    grep -q 'patches/klystron-damaged-files.patch' "$dir/CMakeLists.txt" || \
      sed -i 's|^\(    PATCHES .*patches/scope-capture.patch\)$|\1 patches/klystron-damaged-files.patch|' "$dir/CMakeLists.txt"
    grep -q 'patches/klystron-damaged-files.patch' "$dir/CMakeLists.txt" || {
      echo "klystron's patch could not be added to $dir/CMakeLists.txt" >&2; exit 1; }
  fi
  cmake -S "$dir" -B "$dir/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF >/dev/null
  cmake --build "$dir/build" -j"$JOBS" >/dev/null
  cp "$dir/build/plugins/${name}_playback.so" "$PREFIX/plugins/"
done

# nothing under the GPL (or LGPL) in what the plugins and the host are built from
if grep -rlI -E "General Public|GPL-[0-9]|Lesser" --include='*.c' --include='*.cpp' \
     --include='*.h' --include='*.hpp' playback-klystrack \
     "$HERE/csrc/retrovert" >&2; then
  echo "a GPL file is among bbmp-rv's sources" >&2; exit 1
fi
g++ -O2 -std=c++17 -Wall -Wextra -ffp-contract=off -I playback-klystrack/include \
  -o "$PREFIX/bin/bbmp-rv" "$HERE/csrc/retrovert/bbmp-rv.cpp" "$HERE/csrc/retrovert/ixs_engine.cpp" \
  -ldl -lz
# the iXalance plugin of earlier builds is not used any more
rm -f "$PREFIX/plugins/ixalance_playback.so" "$PREFIX/share/bbmp-rv/ixalance-LICENSE.txt"
# the licences and where each part comes from go with the program
mkdir -p "$PREFIX/share/bbmp-rv"
cp playback-klystrack/LICENSES "$PREFIX/share/bbmp-rv/klystrack-LICENSE.txt"
cp "$HERE/csrc/retrovert/LICENSE.retrovert-api" "$PREFIX/share/bbmp-rv/retrovert-LICENSE.txt"
cp "$HERE/csrc/retrovert/LICENSE" "$HERE/csrc/retrovert/README.txt" "$PREFIX/share/bbmp-rv/"
echo "built $PREFIX/bin/bbmp-rv with $(ls "$PREFIX/plugins" | tr '\n' ' ')"
