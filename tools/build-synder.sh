#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-synder: Synder SNG files (Modland's "Synder SNG-Player" and "Synder
# SNG-Player Stereo"), on a synthesizer written for this player from a description of
# how DaNyL's SNG-Player behaves (a clean room). The player's own code; nothing is
# fetched. The result goes to ~/.local/opt/bbmp-synder.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
PREFIX="${1:-$BBMP_OPT/bbmp-synder}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MINE="$HERE/csrc/synder"
mkdir -p "$PREFIX/bin"

g++ -O2 -std=gnu++17 -Wall ${CXXFLAGS:-} -o "$PREFIX/bin/bbmp-synder" "$MINE/bbmp-synder.cpp" -I"$MINE" -lm
echo "installed $PREFIX/bin/bbmp-synder"
