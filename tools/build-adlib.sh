#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-adlib: Ad Lib formats that AdPlug doesn't play, with replayers written
# for this player, on AdPlug's emulation of the chip (Nuked OPL3).
# The result goes to ~/.local/opt/bbmp-adlib.
#
#   AdPlug, libbinio   LGPL 2.1   shared with bbmp-chip and bbmp-misc
#                                 (~/.local/opt/adplug; build-misc.sh builds it if missing)
set -euo pipefail
. "$(dirname "$0")/sources.sh"
PREFIX="${1:-$BBMP_OPT/bbmp-adlib}"
ADPLUG="$BBMP_OPT/adplug"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MINE="$HERE/csrc/adlib"
mkdir -p "$PREFIX/bin"

if [ ! -f "$ADPLUG/lib/libadplug$SO" ]; then
  "$HERE/tools/build-misc.sh" "${BBMP_MISC_PREFIX:-$BBMP_OPT/bbmp-misc}"
fi

g++ -O2 -std=gnu++17 -ffp-contract=off -Wall -o "$PREFIX/bin/bbmp-adlib" \
  "$MINE/bbmp-adlib.cpp" "$MINE/ppf_cr.cpp" "$MINE/d01_cr.cpp" "$MINE/sig_cr.cpp" "$MINE/lem_cr.cpp" "$MINE/fmt_cr.cpp" \
  "$MINE/mus_cr.cpp" "$MINE/cal_cr.cpp" "$MINE/vib_cr.cpp" \
  -I"$MINE" -I"$ADPLUG/include" -L"$ADPLUG/lib" -Wl,-rpath,"$ADPLUG/lib" -ladplug -llibbinio -lm $ICONV
echo "installed $PREFIX/bin/bbmp-adlib"
