#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-psgplay: Atari ST SNDH tunes, played by psgplay (Fredrik Noring). psgplay is
# under the GPL version 2 only (its files say "SPDX-License-Identifier: GPL-2.0"), so it
# can't be built into the player's GPL 3 code: this is a program of its own, whose own
# file (csrc/psgplay/bbmp-psgplay.cpp) is GPL-2.0-only too. Until 7 October 2026 it was
# part of bbmp-chip. Sources go to ~/.local/src, the result to ~/.local/opt/bbmp-psgplay.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
SRC="$BBMP_SRC"
PREFIX="${1:-$BBMP_OPT/bbmp-psgplay}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MINE="$HERE/csrc/psgplay"
mkdir -p "$PREFIX/bin" "$PREFIX/share/bbmp-psgplay"
cd "$SRC"
if kept; then
  kept_unpack psgplay-6f1c39f.tar.xz psgplay
else
  # the commit bbmp-sources keeps, which the player was tested with, and its submodules
  git_pin https://github.com/frno7/psgplay.git psgplay 6f1c39ffdacd4e568893c634033a571b2a34a01a
fi
grep -q 'SPDX-License-Identifier: GPL-2.0' psgplay/lib/psgplay/psgplay.c || {
  echo "psgplay's licence is not the one this build was checked against" >&2; exit 1; }
make -C psgplay -j"$JOBS" lib/psgplay/libpsgplay.a
g++ -O2 -std=gnu++17 -o "$PREFIX/bin/bbmp-psgplay" "$MINE/bbmp-psgplay.cpp" \
  -Ipsgplay/include psgplay/lib/psgplay/libpsgplay.a
# the licence and where each part comes from go with the program
cp psgplay/licence/GPL-2.0 "$PREFIX/share/bbmp-psgplay/GPL-2.0.txt"
cp psgplay/licence/MIT "$PREFIX/share/bbmp-psgplay/MIT.txt"
cp psgplay/licence/LGPL-2.1 "$PREFIX/share/bbmp-psgplay/LGPL-2.1.txt"
cp "$MINE/README.txt" "$PREFIX/share/bbmp-psgplay/"
echo "installed $PREFIX/bin/bbmp-psgplay"
