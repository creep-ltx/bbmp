#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-fac: FAC SoundTracker (MSX) songs, played by a replayer of our own
# (csrc/fac/fac_cr.cpp, written in a clean room) on the chip emulators emu8950 (Y8950,
# MSX-AUDIO) and emu2413 (YM2413, MSX-MUSIC) by Mitsutaka Okazaki, both MIT licence.
# Sources go to ~/.local/src/modland-fac, the result to ~/.local/opt/bbmp-fac.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
SRC="$BBMP_SRC/modland-fac"
PREFIX="${1:-$BBMP_OPT/bbmp-fac}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$SRC" "$PREFIX/bin"
cd "$SRC"

# fetch one commit of a repository, so that it is the same one next month
pinned() {
  local repo="$1" dir="$2" commit="$3"
  if kept; then kept_unpack "$dir-${commit:0:7}.tar.xz" "$dir"; return; fi
  if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" != "$commit" ]; then
    rm -rf "$dir"
    git init -q "$dir"
    git -C "$dir" remote add origin "https://github.com/$repo.git"
    git -C "$dir" fetch -q --depth 1 origin "$commit"
    git -C "$dir" -c advice.detachedHead=false checkout -q FETCH_HEAD
  fi
}
pinned digital-sound-antiques/emu8950 emu8950 fb129b18432c9a3255fdb87d87d968e79ab75e1c
pinned digital-sound-antiques/emu2413 emu2413 2e4f6f9752f43cc33572c6636ca31a647600732c

mkdir -p build
for f in emu8950/emu8950.c emu8950/emuadpcm.c emu2413/emu2413.c; do
  gcc -O2 -std=gnu17 -w -c "$f" -o "build/$(basename "${f%.c}").o"
done
# no fused multiply-add: the resampler's sums are rounded as written, on every machine
g++ -O2 -std=gnu++20 -Wall -ffp-contract=off -o "$PREFIX/bin/bbmp-fac" "$HERE/csrc/fac/fac_cr.cpp" \
  -Iemu8950 -Iemu2413 build/emu8950.o build/emuadpcm.o build/emu2413.o -lm
echo "installed $PREFIX/bin/bbmp-fac"
