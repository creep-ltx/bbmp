#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-piece: a replayer for songs of Aquaplus' P/ECE music driver (.pmd,
# "PiECE music driver" on Modland), written for this player (csrc/piece).
#
# It is written anew from the SDK's documents of the format and from measurements of how
# the driver sounds; nothing of Aquaplus' driver is in it or fetched by this script (the
# driver's terms allow using it only in programs for the P/ECE).
# The result goes to ~/.local/opt/bbmp-piece.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
PREFIX="${1:-$BBMP_OPT/bbmp-piece}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$PREFIX/bin"
g++ -O2 -std=gnu++17 -Wall -Wextra -I"$HERE/csrc/piece" \
  -o "$PREFIX/bin/bbmp-piece" \
  "$HERE/csrc/piece/bbmp-piece.cpp" "$HERE/csrc/piece/piece.cpp" -lm $ICONV
echo "installed $PREFIX/bin/bbmp-piece"
