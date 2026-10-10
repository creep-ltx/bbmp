#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-pcmusic: MONOTONE songs (.mon) and DigiTrekker modules (.dtm) in one
# helper binary. Both replayers are the player's own code; nothing is fetched.
# The result goes to ~/.local/opt/bbmp-pcmusic.
#
#   monotone     a port of MONOTONE's player and PC speaker output (Jim Leonard,
#                github.com/MobyGamer/MONOTONE, The Unlicense)
#   digitrekker  written for this player from DigiTrekker's format document
#                (dtm-1.0.txt, Horst Beham, as libxmp's docs have it)
set -euo pipefail
. "$(dirname "$0")/sources.sh"
PREFIX="${1:-$BBMP_OPT/bbmp-pcmusic}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MINE="$HERE/csrc/pcmusic"
mkdir -p "$PREFIX/bin"

g++ -O2 -std=gnu++17 -Wall -o "$PREFIX/bin/bbmp-pcmusic" \
  "$MINE/bbmp-pcmusic.cpp" "$MINE/monotone.cpp" "$MINE/digitrekker.cpp" -I"$MINE" -lm $ICONV
echo "installed $PREFIX/bin/bbmp-pcmusic"
