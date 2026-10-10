#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build ZXTune's player core (Spectrum, YM, xSF family, V2, ...) and the
# bbmp-zxtune helper. Needs Boost headers; Arch only ships them in the "boost"
# package, so if they're missing Boost's own release (its headers, checked by SHA-256) is
# unpacked into ~/.local/opt/boost without installing anything system-wide. Built from
# bbmp-sources (BBMP_SOURCES), they come from the same release there.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
SRC="${ZXTUNE_SRC:-$BBMP_SRC/zxtune}"
PREFIX="${1:-$BBMP_OPT/zxtune}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ZXTUNE_COMMIT=e36902fe1cc06d9e73116db644b688708a7745d4
BOOST=boost-1.92.0
BOOST_SHA256=ea7b982002cc9dfbe59b0b217b206f470dc75f3de0bb2973d844118934d82411
BOOST_INC=/usr/include
if kept; then
  BOOST_INC="$BBMP_SRC/$BOOST"
  if [ ! -d "$BOOST_INC/boost" ]; then
    f="$(kept_file "$BOOST-b2-nodocs.tar.xz")"
    tar -xf "$f" -C "$BBMP_SRC" "$BOOST/boost"
  fi
elif [ "$(uname -s)" = Darwin ]; then
  BOOST_INC="$BREW/include"
elif [ ! -d /usr/include/boost ]; then
  BOOST_INC="$BBMP_OPT/boost/usr/include"
  if [ ! -d "$BOOST_INC/boost" ]; then
    tmp="$(mktemp -d)"
    curl -sfL -o "$tmp/$BOOST.tar.xz" \
      "https://github.com/boostorg/boost/releases/download/$BOOST/$BOOST-b2-nodocs.tar.xz"
    echo "$BOOST_SHA256  $tmp/$BOOST.tar.xz" | sha256_check \
      || { echo "Boost's download is not the release this was built with" >&2; exit 1; }
    tar -xf "$tmp/$BOOST.tar.xz" -C "$tmp" "$BOOST/boost"
    mkdir -p "$BOOST_INC"
    mv "$tmp/$BOOST/boost" "$BOOST_INC/"
    rm -rf "$tmp"
  fi
fi
if kept; then
  kept_unpack "zxtune-${ZXTUNE_COMMIT:0:7}.tar.xz" "$SRC"
else
  # the commit the player was tested with (and bbmp-sources keeps), not the newest
  git_pin https://github.com/vitamin-caig/zxtune.git "$SRC" "$ZXTUNE_COMMIT"
fi

# Pro Tracker 2 and 3 as their original players play them (checked register by register
# against them, tools/verify): in PT3 a note and its ornament, and the channels'
# envelope offsets, are added in a byte; PT2's volume table is rounded. And without
# UnRAR (its licence adds a restriction) and vgmstream (its relic_mixfft.c is for
# non-commercial use only): neither is registered, and the helpers don't link them
# (csrc/zxtune/no-rar-vgmstream.patch, 7 October 2026). Nor the AY Emul player, which runs
# songs on Z80~Ex, GPL 2 only (csrc/zxtune/no-z80ex.patch; Game Music Emu plays AY Emul),
# nor the VGM and S98 player, libvgm, whose OPL4 is GPL 2 only and some of whose chips state
# no licence (csrc/zxtune/no-libvgm.patch; tools/build-vgm.sh builds bbmp-vgm for them).
# And without Sony's PlayStation 2 BIOS, which ZXTune builds into the program for the PSF
# and PSF2 player: the user provides it in the extras folder (csrc/zxtune/no-ps2-bios.patch);
# ZXTune's copy is removed from the tree so that nothing can build it in. And HivelyTracker's
# replayer checks what a damaged file gives it (csrc/zxtune/hvl-damaged-files.patch, the
# final audit of 7 October 2026).
apply_patches "$SRC" "$HERE"/csrc/zxtune/*.patch
rm -f "$SRC/src/module/players/xsf/scph10000_he.inc"
# an object built before still names it as a dependency: built again
grep -rl --include='*.d' scph10000_he.inc "$SRC/obj" 2>/dev/null | while read -r D; do rm -f "$D" "${D%.d}.o"; done || true

# the helper is built as one more app inside the ZXTune tree
mkdir -p "$SRC/apps/bbmp-zxtune"
cp "$HERE/csrc/zxtune/main.cpp" "$HERE/csrc/zxtune/Makefile" "$SRC/apps/bbmp-zxtune/"

PLATFORM=linux
[ "$(uname -s)" = Darwin ] && PLATFORM=darwin      # ZXTune's own macOS settings (Apple's linker)
make -C "$SRC/apps/bbmp-zxtune" -j"$JOBS" platform=$PLATFORM release=1 \
  system.zlib=1 cxx_flags="-isystem $BOOST_INC -include $HERE/csrc/zxtune/stdint-both.h"
mkdir -p "$PREFIX/bin"
install -m755 "$(find "$SRC/bin" -name bbmp-zxtune -type f | head -1)" "$PREFIX/bin/"
echo "installed $PREFIX/bin/bbmp-zxtune"
