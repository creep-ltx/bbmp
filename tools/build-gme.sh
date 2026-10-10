#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build Game Music Emu with the player's fix, into ~/.local/opt/gme (the player uses it
# before the system's libgme). The fix (csrc/gme/hes-pc-wrap.patch): the PC Engine's
# CPU address wraps at 64 KB; in libgme it didn't on 64-bit machines, and some HES rips
# crash the player's replayer process at a track (Oboccha Makun, track 199).
#
# The commit is the one the player was built with (after 0.6.5, "0.6.6" in its CMake;
# the script asked for the tag 0.6.5 before, but the copy in use was this one).
set -euo pipefail
. "$(dirname "$0")/sources.sh"
SRC="$BBMP_SRC/game-music-emu"
PREFIX="${1:-$BBMP_OPT/gme}"
COMMIT=f68963b1de0633149b05732113d3c3113c6f63a1
PATCH="$(cd "$(dirname "$0")/.." && pwd)/csrc/gme/hes-pc-wrap.patch"
if kept; then
  kept_unpack "game-music-emu-${COMMIT:0:7}.tar.xz" "$SRC"
  # without git, the patch is applied the way patch does it, once
  if ! grep -q bbmp-hes "$SRC/.bbmp-sources"; then
    patch -s -d "$SRC" -p1 < "$PATCH"
    echo bbmp-hes >> "$SRC/.bbmp-sources"
  fi
else
  if [ ! -d "$SRC" ]; then
    git init -q "$SRC"
    git -C "$SRC" remote add origin https://github.com/libgme/game-music-emu.git
    git -C "$SRC" fetch -q --depth 1 origin "$COMMIT"
    git -C "$SRC" checkout -q --detach FETCH_HEAD
  fi
  git -C "$SRC" apply --check "$PATCH" 2>/dev/null && git -C "$SRC" apply "$PATCH"
fi
cmake -S "$SRC" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  -DGME_BUILD_EXAMPLES=OFF -DGME_BUILD_TESTING=OFF -DENABLE_UBSAN=OFF \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib
cmake --build "$SRC/build" -j"$JOBS"
cmake --install "$SRC/build"
echo "installed $PREFIX/lib/libgme.so"
