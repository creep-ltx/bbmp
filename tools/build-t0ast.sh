#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-t0ast: songs of T0AST, "The 0ok Amazing Synth Tracker" (.t0ast), on a
# synthesizer written for this player from a description of how BeRo's tracker behaves
# (a clean room). The player's own code; nothing is fetched. The result goes to
# ~/.local/opt/bbmp-t0ast.
#
# The flags keep the tracker's arithmetic: -frounding-math so that the compiler does not
# work out sums at build time with the usual rounding (the engine rounds toward zero),
# -ffp-contract=off so that no multiply and add are fused. No -ffast-math.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
PREFIX="${1:-$BBMP_OPT/bbmp-t0ast}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MINE="$HERE/csrc/t0ast"
mkdir -p "$PREFIX/bin"

g++ -O2 -std=gnu++17 -Wall ${CXXFLAGS:-} -fno-fast-math -frounding-math -ffp-contract=off \
  -o "$PREFIX/bin/bbmp-t0ast" "$MINE/bbmp-t0ast.cpp" -I"$MINE" -lm
echo "installed $PREFIX/bin/bbmp-t0ast"
