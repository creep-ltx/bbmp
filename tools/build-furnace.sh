#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build a headless Furnace (Deflemask .dmf, FamiTracker .ftm, Furnace .fur) and
# install csrc/furnace/bbmp-furnace, which streams it in modland's pipe protocol.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
SRC="$BBMP_SRC/furnace"
PREFIX="${1:-$BBMP_OPT/furnace}"
FURNACE_COMMIT=fa9541db8004fcdab92808ef57779cd0249e451f   # the version bbmp-sources keeps, which the player was tested with
if kept; then
  kept_unpack "furnace-${FURNACE_COMMIT:0:7}.tar.xz" "$SRC"
else
  git_pin https://github.com/tildearrow/furnace.git "$SRC" "$FURNACE_COMMIT"
fi
# outrate.patch: the rate a file is written at (Furnace writes 44100 Hz otherwise, the player
# plays 48000). no-unlicensed-cores.patch: QSound's and Supervision's sound cores state no
# licence, so they are not built and those two chips are silent.
apply_patches "$SRC" "$(dirname "$0")"/../csrc/furnace/*.patch
# The YM2608's rhythm ROM (its six drum sounds, Yamaha's) is not built in: both of Furnace's
# YM2608 cores read it from the file the user puts in the extras folder (BBMP_YM2608_ROM,
# csrc/furnace/bbmp_ym2608_rom.h); without it the drums are silent. Their copies are replaced.
FUR="$(cd "$(dirname "$0")/.." && pwd)/csrc/furnace"
install -m644 "$FUR/bbmp_ym2608_rom.h" "$FUR/rss.h" "$SRC/src/engine/platform/sound/"
install -m644 "$FUR/bbmp_ym2608_rom.h" "$FUR/fmopna_rom.h" "$SRC/extern/YM2608-LLE/"
# GCC 15+ no longer pulls these headers in implicitly
cmake -S "$SRC" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=OFF -DUSE_SDL2=OFF \
  -DUSE_RTMIDI=OFF -DWITH_JACK=OFF -DWITH_PORTAUDIO=OFF -DUSE_BACKWARD=OFF -DUSE_FREETYPE=OFF \
  -DWITH_INSTRUMENTS=OFF -DWITH_WAVETABLES=OFF -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_CXX_FLAGS="${CXXFLAGS:-} -include climits -include cstdint -include cstring"
cmake --build "$SRC/build" -j"$JOBS"
cmake --install "$SRC/build"
# neither core without a licence may be in it (an object of an older build could be)
if nm "$PREFIX/bin/furnace" 2>/dev/null | grep -qE ' [TtDd] _?(qsound_update|supervision_sound_)'; then
  echo "Furnace still has QSound's or Supervision's core built in" >&2; exit 1
fi

if nm "$PREFIX/bin/furnace" 2>/dev/null | grep -qE ' [RrDd] _?YM2608_ADPCM_ROM$'; then
  echo "Furnace still has the YM2608's rhythm ROM built in" >&2; exit 1
fi

# Furnace's own config (sample rate) lives here, not in ~/.config
mkdir -p "$PREFIX/config/furnace"
printf '!DIV_CONFIG_START!\naudioRate=48000\n~DIV_CONFIG_END~\n' > "$PREFIX/config/furnace/furnace.cfg"

HERE="$(cd "$(dirname "$0")/.." && pwd)"
install -m755 "$HERE/csrc/furnace/bbmp-furnace" "$PREFIX/bin/bbmp-furnace"
chmod +x "$PREFIX/bin/bbmp-furnace"
echo "installed $PREFIX/bin/bbmp-furnace"
