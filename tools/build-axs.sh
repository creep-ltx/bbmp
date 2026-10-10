#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-axs: songs of AXS (.axs), Resolution Audio's synthesizer and tracker, on an
# engine written for this player from a description of how AXS's Winamp plug-in sounds
# (a clean room). The player's own code; nothing is fetched. The result goes to
# ~/.local/opt/bbmp-axs.
#
# -ffp-contract=off so that no multiply and add are fused (the filters' sums must be
# rounded step by step as the original's were). No -ffast-math.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
PREFIX="${1:-$BBMP_OPT/bbmp-axs}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MINE="$HERE/csrc/axs"
mkdir -p "$PREFIX/bin"

g++ -O2 -std=gnu++17 -Wall ${CXXFLAGS:-} -fno-fast-math -ffp-contract=off \
  -o "$PREFIX/bin/bbmp-axs" "$MINE/bbmp-axs.cpp" -I"$MINE" -lm
echo "installed $PREFIX/bin/bbmp-axs"
