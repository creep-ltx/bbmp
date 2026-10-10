#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-nez: libnezplug (NEZplug++ as a library) for SGC (Sega Master System,
# Game Gear, ColecoVision) and GBR (Game Boy) rips.
# Sources go to ~/.local/src, the result to ~/.local/opt/bbmp-nez.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
SRC="$BBMP_SRC"
PREFIX="${1:-$BBMP_OPT/bbmp-nez}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
# John Regan's libnezplug. His own repository (github.com/jprjr/libnezplug) is gone;
# this copy has his history plus one commit that removes an unused variable.
REPO="https://github.com/gcielniak/libnezplug.git"
COMMIT="161d6e2b7187dc1426e33bd0f39d79b865899cca"
mkdir -p "$SRC" "$PREFIX/bin"
cd "$SRC"

# (a folder unpacked from modland-player-sources is used as it is, without git)
if kept; then kept_unpack "libnezplug-${COMMIT:0:7}.tar.xz" libnezplug; fi
[ -d libnezplug ] || git clone "$REPO" libnezplug
cd libnezplug
if [ -d .git ]; then
  [ "$(git rev-parse HEAD)" = "$COMMIT" ] || { git fetch origin; git checkout -q --detach "$COMMIT"; }
  git checkout -q -- src
fi
# our hooks: the helper is told of every write to a sound chip and every call of the
# song's play routine, which is how it finds where a song loops
if patch -p1 --dry-run -R -s -f < "$HERE/csrc/nez/libnezplug-hooks.patch" >/dev/null 2>&1; then
  :     # there already
else
  patch -p1 -s < "$HERE/csrc/nez/libnezplug-hooks.patch"
fi

SOURCES="src/cpu/kmz80/kmdmg.c src/cpu/kmz80/kmevent.c src/cpu/kmz80/kmr800.c
  src/cpu/kmz80/kmz80.c src/cpu/kmz80/kmz80c.c src/cpu/kmz80/kmz80t.c src/cpu/kmz80/kmz80u.c
  src/device/logtable.c src/device/nes/s_apu.c src/device/nes/s_fds1.c src/device/nes/s_fds2.c
  src/device/nes/s_fds3.c src/device/nes/s_fds.c src/device/nes/s_fme7.c src/device/nes/s_mmc5.c
  src/device/nes/s_n106.c src/device/nes/s_vrc6.c src/device/nes/s_vrc7.c
  src/device/opl/s_deltat.c src/device/opl/s_opl.c src/device/s_dmg.c src/device/s_hesad.c
  src/device/s_hes.c src/device/s_psg.c src/device/s_scc.c src/device/s_sng.c
  src/format/audiosys.c src/format/handler.c src/format/m_gbr.c src/format/m_hes.c
  src/format/m_kss.c src/format/m_nsd.c src/format/m_nsf.c src/format/m_sgc.c
  src/format/m_zxay.c src/format/nezplug.c src/format/nsf6502.c src/format/songinfo.c
  src/format/trackinfo.c src/logtable/log_table_12_7_30.c src/logtable/log_table_12_8_30.c
  src/opltable/opl_table.c"
# one file of everything, as libnezplug's own Makefile builds it (its tables are
# defined in a header, which only works that way)
rm -rf build && mkdir -p build
perl aux/amalgate.pl $SOURCES > build/nezplug.c
# GCC 15+ compiles C as C23; this is C89-style code
gcc -O2 -std=gnu17 -w -Isrc/include -c build/nezplug.c -o build/nezplug.o

g++ -O2 -std=gnu++17 -Wall -Wno-parentheses -o "$PREFIX/bin/bbmp-nez" "$HERE/csrc/nez/bbmp-nez.cpp" \
  -Isrc/include build/nezplug.o -lm
echo "installed $PREFIX/bin/bbmp-nez"
