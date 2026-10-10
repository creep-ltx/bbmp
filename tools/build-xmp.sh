#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-xmp: libxmp, for module formats libopenmpt doesn't read (Archimedes
# Tracker, Coconizer). Sources go to ~/.local/src, the result to ~/.local/opt/bbmp-xmp.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
SRC="$BBMP_SRC"
PREFIX="${1:-$BBMP_OPT/bbmp-xmp}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=libxmp-4.7.3
mkdir -p "$SRC" "$PREFIX/bin"
cd "$SRC"
if kept; then kept_unpack libxmp-1e08121.tar.xz libxmp; fi      # the commit of the tag
[ -d libxmp ] || git clone --depth 1 --branch "$VERSION" https://github.com/libxmp/libxmp.git libxmp
cmake -S libxmp -B libxmp/build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED=OFF -DBUILD_STATIC=ON \
  -DLIBXMP_DOCS=OFF -DWITH_UNIT_TESTS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON >/dev/null
cmake --build libxmp/build -j"$JOBS" >/dev/null
LIB="$(find libxmp/build -name 'libxmp*.a' | head -1)"
cc -O2 -std=gnu17 -Wall -I libxmp/include -o "$PREFIX/bin/bbmp-xmp" \
  "$HERE/csrc/xmp/bbmp-xmp.c" "$LIB" -lm
echo "built $PREFIX/bin/bbmp-xmp"
