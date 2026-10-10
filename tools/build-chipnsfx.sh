#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-chipnsfx: CHIPNSFX songs (.chp, ZX-Art's "Spectrum - CHP"), played by the
# tracker's own code. CHIPNSFX.C is César Nicolás-González's (CNGSoft) tracker,
# GPL-3.0-or-later, from http://cngsoft.no-ip.org/chipnsfx.zip (the 2024-12-31 build,
# kept unchanged in csrc/chipnsfx); nothing is fetched. The result goes to
# ~/.local/opt/bbmp-chipnsfx.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
PREFIX="${1:-$BBMP_OPT/bbmp-chipnsfx}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MINE="$HERE/csrc/chipnsfx"
mkdir -p "$PREFIX/bin"

# the tracker's code is old-style C: its own warnings are not ours
gcc -O2 -std=gnu11 -w -o "$PREFIX/bin/bbmp-chipnsfx" "$MINE/bbmp-chipnsfx.c" -I"$MINE" -lm
echo "installed $PREFIX/bin/bbmp-chipnsfx"
