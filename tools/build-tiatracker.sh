#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-tiatracker: TIATracker songs (.ttt, Atari 2600 music; ASMA's "TIATracker"),
# played by the tracker's own song and replay code. TIATracker is Andre "Kylearan"
# Wichmann's (GPL-2, the copy from github.com/dionoid/tiatracker, commit f523802), with
# Stella's TIA sound (GPL-2-or-later); Qt is replaced by small stand-ins. All of it is
# kept in csrc/tiatracker (README.txt there says what comes from where); nothing is
# fetched. GPL-2 only, so this is a program of its own. The result goes to
# ~/.local/opt/bbmp-tiatracker.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
PREFIX="${1:-$BBMP_OPT/bbmp-tiatracker}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MINE="$HERE/csrc/tiatracker"
mkdir -p "$PREFIX/bin" "$PREFIX/share/bbmp-tiatracker"

g++ -O2 -std=gnu++17 -Wall -Wno-unused-variable -Wno-unused-but-set-variable ${CXXFLAGS:-} \
  -o "$PREFIX/bin/bbmp-tiatracker" \
  -I"$MINE/shim" -I"$MINE" -I"$MINE/track" -I"$MINE/tiasound" \
  "$MINE/bbmp-tiatracker.cpp" "$MINE/shim/qjson.cpp" \
  "$MINE"/track/{instrument,note,pattern,percussion,sequence,sequenceentry,track}.cpp \
  "$MINE/tiasound/tiasound.cpp" "$MINE/emulation/player.cpp" "$MINE/emulation/TIASnd.cpp"
# the licence and where each part comes from go with the program
cp "$MINE/license.txt" "$MINE/README.txt" "$PREFIX/share/bbmp-tiatracker/"
echo "installed $PREFIX/bin/bbmp-tiatracker"
