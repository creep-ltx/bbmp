#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-musicline: a replayer for Musicline Editor modules (*.ml).
# The replay routine is in csrc/musicline. Its tables (pitch, LFO waves, filter
# coefficients) are those of the original replayer; they are taken from
# NostalgicPlayer (MIT), fetched into ~/.local/src at a fixed commit.
# The result goes to ~/.local/opt/bbmp-musicline.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
SRC="$BBMP_SRC"
PREFIX="${1:-$BBMP_OPT/bbmp-musicline}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
NP_COMMIT=2485bec27b1f02c13d0fe70c7c796acd61e36223
NP_FILE=Source/Agents/Players/MusiclineEditor/Tables.cs
NP_SHA256=45aa0b77bbb6154c066c230ab37a7f870e5850849f349c66ac315230371a7b8f
NP="$SRC/nostalgicplayer-musicline"
mkdir -p "$NP" "$PREFIX/bin"

check() { echo "$NP_SHA256  $NP/Tables.cs" | sha256_check >/dev/null 2>&1; }
if kept; then
  kept_unpack "nostalgicplayer-musicline-${NP_COMMIT:0:7}.tar.xz" "$NP"
  check || { echo "Tables.cs is not the file that was expected" >&2; exit 1; }
elif ! check; then
  curl -sfL -o "$NP/Tables.cs" \
    "https://raw.githubusercontent.com/neumatho/NostalgicPlayer/$NP_COMMIT/$NP_FILE"
  curl -sfL -o "$NP/LICENSE" \
    "https://raw.githubusercontent.com/neumatho/NostalgicPlayer/$NP_COMMIT/LICENSE"
  check || { echo "Tables.cs is not the file that was expected" >&2; exit 1; }
fi

BUILD="$NP/build"
mkdir -p "$BUILD"
python3 "$HERE/csrc/musicline/gen_tables.py" "$NP/Tables.cs" "$BUILD/mline_tables.h"
g++ -O2 -std=gnu++17 -Wall -Wextra -I"$BUILD" -I"$HERE/csrc/musicline" \
  -o "$PREFIX/bin/bbmp-musicline" \
  "$HERE/csrc/musicline/bbmp-musicline.cpp" \
  "$HERE/csrc/musicline/mline_load.cpp" \
  "$HERE/csrc/musicline/mline_play.cpp" -lm
# the licences of the code it is made from go with the program (LGPL, MIT)
mkdir -p "$PREFIX/share/licenses"
install -m644 "$HERE/csrc/musicline/README.md" "$HERE/csrc/musicline/COPYING.LGPL" \
  "$HERE/csrc/musicline/LICENSE.NostalgicPlayer" "$PREFIX/share/licenses/"
echo "installed $PREFIX/bin/bbmp-musicline"
