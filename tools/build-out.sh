#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build libbbmp-out, the sound output where there is no PulseAudio (macOS: CoreAudio),
# from csrc/out and miniaudio (public domain or MIT-0, one header). The result goes to
# ~/.local/opt/bbmp-out/lib. On Linux it is built only to be tested (BBMP_AUDIO=miniaudio).
set -euo pipefail
. "$(dirname "$0")/sources.sh"
PREFIX="${1:-$BBMP_OPT/bbmp-out}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=0.11.22
SUM=9019743287e443c55e5737a7297f38e5e358561701d6db2d905afb114390c410
mkdir -p "$PREFIX/lib" "$BBMP_SRC/miniaudio-$VERSION"
HEADER="$BBMP_SRC/miniaudio-$VERSION/miniaudio.h"
if kept; then kept_copy "miniaudio-$VERSION.h" "$HEADER"; fi
[ -f "$HEADER" ] || curl -sfL -o "$HEADER" \
  "https://raw.githubusercontent.com/mackron/miniaudio/$VERSION/miniaudio.h"
echo "$SUM  $HEADER" | sha256_check || { echo "$HEADER is not miniaudio $VERSION" >&2; exit 1; }
if [ "$(uname -s)" = Darwin ]; then
  cc -O2 -dynamiclib -I "$(dirname "$HEADER")" -o "$PREFIX/lib/libbbmp-out.dylib" \
    "$HERE/csrc/out/bbmp_out.c" -framework CoreAudio -framework AudioToolbox -framework CoreFoundation
  echo "built $PREFIX/lib/libbbmp-out.dylib"
else
  cc -O2 -shared -fPIC -I "$(dirname "$HEADER")" -o "$PREFIX/lib/libbbmp-out.so" \
    "$HERE/csrc/out/bbmp_out.c" -lpthread -lm -ldl
  echo "built $PREFIX/lib/libbbmp-out.so"
fi
