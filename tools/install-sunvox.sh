#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetch the SunVox library (free to use, prebuilt by its author) for .sunvox songs.
# From bbmp-sources (BBMP_SOURCES) it is built instead, from the source in the same zip,
# for any x86-64 (the author's build needs SSE3).
set -euo pipefail
. "$(dirname "$0")/sources.sh"
PREFIX="${1:-$BBMP_OPT/sunvox}"
VERSION=2.1.4d
# the zip as bbmp-sources keeps it (the same as the author's download of 29 September 2026)
SHA256=abe851de9d65a10e06673bf33257154591a2ff63ad2bb298488a5941cc5f4057
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
if kept && [ "$(uname -s)" != Darwin ]; then
  unzip -q "$(kept_file "sunvox_lib-$VERSION.zip")" -d "$tmp"
  rm -f "$tmp"/sunvox_lib/sunvox_lib/linux/lib_x86_64/sunvox.so
  # as its MAKE_LINUX_X86 script builds it, without the SSE3 it asks for there
  make -C "$tmp/sunvox_lib/sunvox_lib/make" -j"$JOBS" TARGET_OS=linux MAKE_DYNAMIC_LIB=true \
    TARGET_ARCH=x86_64 MAKE_WITHOUT_SIMD=true STYPE=PS_STYPE_FLOAT32 > "$tmp/make.log" 2>&1 \
    || { tail -30 "$tmp/make.log"; exit 1; }
  make -C "$tmp/sunvox_lib/sunvox_lib/make" install TARGET_OS=linux MAKE_DYNAMIC_LIB=true TARGET_ARCH=x86_64
elif kept; then
  unzip -q "$(kept_file "sunvox_lib-$VERSION.zip")" -d "$tmp"
else
  curl -sfL -o "$tmp/sunvox.zip" "https://warmplace.ru/soft/sunvox/sunvox_lib-$VERSION.zip"
  echo "$SHA256  $tmp/sunvox.zip" | sha256_check \
    || { echo "SunVox's download is not the zip this was tested with ($VERSION)" >&2; exit 1; }
  unzip -q "$tmp/sunvox.zip" -d "$tmp"
fi
mkdir -p "$PREFIX"
if [ "$(uname -s)" = Darwin ]; then        # the author's own Mac build, for this processor
  arch="$(uname -m)"; [ "$arch" = arm64 ] || arch=x86_64
  install -m644 "$tmp/sunvox_lib/sunvox_lib/macos/lib_$arch/sunvox.dylib" "$PREFIX/sunvox.dylib"
else
  install -m644 "$tmp"/sunvox_lib/sunvox_lib/linux/lib_x86_64/sunvox.so "$PREFIX/sunvox.so"
fi
# its licence asks for its credit and all the texts of docs/license to go with it:
# LICENSE.txt (the credit), sundog.txt, libflac.txt, tremor.txt
for f in "$tmp"/sunvox_lib/sunvox_lib/docs/license/*.txt; do
  install -m644 "$f" "$PREFIX/$(basename "$f")"
done
for f in LICENSE.txt sundog.txt libflac.txt tremor.txt; do
  [ -f "$PREFIX/$f" ] || { echo "SunVox's $f is missing from its zip" >&2; exit 1; }
done
echo "installed in $PREFIX"
