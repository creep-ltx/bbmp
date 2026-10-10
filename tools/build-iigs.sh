#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-iigs: SoundSmith songs and NoiseTracker GS modules of the Apple IIGS,
# played by engines written for this player in a clean room, from a description of the
# formats only, on an emulation of the Ensoniq 5503.
# Everything it is built from is in csrc/iigs; nothing is downloaded.
# The result goes to ~/.local/opt/bbmp-iigs.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
PREFIX="${1:-$BBMP_OPT/bbmp-iigs}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$PREFIX/bin"
g++ -O2 -std=gnu++17 -Wall -Wextra -o "$PREFIX/bin/bbmp-iigs" \
  "$HERE/csrc/iigs/bbmp-iigs.cpp" "$HERE/csrc/iigs/ss_cr.cpp" \
  "$HERE/csrc/iigs/ntgs_cr.cpp" -lm
# MAME's BSD licence for the chip emulation goes with the program
mkdir -p "$PREFIX/share/licenses"
install -m644 "$HERE/csrc/iigs/LICENSE.es5503" "$PREFIX/share/licenses/"
echo "installed $PREFIX/bin/bbmp-iigs"
