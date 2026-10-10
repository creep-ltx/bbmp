#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-berotracker: BeRoTracker songs (.brt), on a replayer written for this player
# from a description of how BeRo's tracker behaves (a clean room). The player's own code;
# nothing is fetched. The result goes to ~/.local/opt/bbmp-berotracker.
#
# The flags keep the tracker's arithmetic: -ffp-contract=off so that no multiply and add
# are fused (the mixer rounds each step on its own). No -ffast-math.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
PREFIX="${1:-$BBMP_OPT/bbmp-berotracker}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MINE="$HERE/csrc/berotracker"
mkdir -p "$PREFIX/bin"

g++ -O2 -std=gnu++17 -Wall ${CXXFLAGS:-} -fno-fast-math -ffp-contract=off \
  -o "$PREFIX/bin/bbmp-berotracker" "$MINE/bbmp-berotracker.cpp" -I"$MINE" -lm
echo "installed $PREFIX/bin/bbmp-berotracker"
