#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-vgm: VGM and S98 files (MSX, Vectrex, ColecoVision, Virtual Boy, X68000 and
# Spectrum VGMs, PC-98 S98s, VGMs of any chip opened from disk). The VGM and S98 player
# (csrc/vgm: reading the files, the commands, timing, loops, data blocks and DAC streams,
# the chips' setup and the mixing) is bbmp's own, written from the published format
# documents; the sound chips are emulated by the licensed cores of libvgm (Valley Bell),
# as the copy in ZXTune's source tree (3rdparty/vgm, commit e36902f) has them. Nothing
# else of libvgm is built: not its player, not its glue (SoundEmu, the resampler, the
# panning, the DAC stream control, the chips' *intf.c), whose files state no licence.
# The OPL4 core (ymf278b.c, after openMSX) is GPL 2 only, so the program is GPL 2 only
# as a whole (its own files are GPL 2 or later).
# Sources go to ~/.local/src, the result to ~/.local/opt/bbmp-vgm.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
SRC="$BBMP_SRC"
PREFIX="${1:-$BBMP_OPT/bbmp-vgm}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MINE="$HERE/csrc/vgm"
ZXTUNE="${ZXTUNE_SRC:-$SRC/zxtune}"
ZXTUNE_COMMIT=e36902fe1cc06d9e73116db644b688708a7745d4
mkdir -p "$SRC" "$PREFIX/bin" "$PREFIX/share/bbmp-vgm"
if kept; then
  kept_unpack "zxtune-${ZXTUNE_COMMIT:0:7}.tar.xz" "$ZXTUNE"
elif [ ! -d "$ZXTUNE" ]; then
  git init -q "$ZXTUNE"
  git -C "$ZXTUNE" remote add origin https://github.com/vitamin-caig/zxtune.git
  git -C "$ZXTUNE" fetch -q --depth 1 origin "$ZXTUNE_COMMIT"
  git -C "$ZXTUNE" checkout -q --detach FETCH_HEAD
fi
VGM="$ZXTUNE/3rdparty/vgm"

# The cores, each with its licence (stated in the file, see csrc/vgm/README.txt):
#   MAME, BSD-3-Clause: sn76496 segapcm rf5c68 multipcm upd7759 okim6258 okim6295
#     okiadpcm k051649 k054539 c6280_mame c140 c219 k053260 pokey qsound_mame scsp
#     scspdsp saa1099_mame es5503 x1_010 c352 iremga20 ymz280b ymf271 gb
#   MAME, GPL-2.0-or-later: ym2151 fmopn fmopl (ymdeltat, part of them, states nothing
#     of its own)
#   Mitsutaka Okazaki, MIT (stated in its repositories, digital-sound-antiques/emu2149
#     and emu2413): emu2413 emu2149;  laoo, MIT: mikey
#   DOSBox's AdLibEmu, LGPL-2.1-or-later: adlibemu_opl2 adlibemu_opl3 (the licence is
#     in adlibemu_opl_inc.c, which both include)
#   Mednafen, GPL-2.0-or-later: vsu;  Gens/GS, GPL-2.0-or-later: pwm
#   openMSX, GPL-2.0 only: ymf278b
#   NSFPlay (Brezza, Brad Smith), reuse allowed by its author: np_nes_apu np_nes_dmc np_nes_fds
CORES="sn76496 emu2413 ym2151 segapcm rf5c68 fmopn ymdeltat adlibemu_opl2 adlibemu_opl3 fmopl
  ymf278b ymz280b ymf271 emu2149 pwm gb np_nes_apu np_nes_dmc np_nes_fds mikey multipcm
  upd7759 okim6258 okim6295 okiadpcm k051649 k054539 c6280_mame c140 c219 k053260 pokey
  qsound_mame scsp scspdsp vsu saa1099_mame es5503 x1_010 c352 iremga20"
# every core states its licence, but for the ones named above as stating it elsewhere
for c in $CORES; do
  case $c in ymdeltat|adlibemu_opl2|adlibemu_opl3|np_nes_apu|np_nes_dmc|np_nes_fds|emu2149|emu2413) continue ;; esac
  head -40 "$VGM/emu/cores/$c.c" | grep -qiE 'license|General Public|Permission is hereby granted|zlib' || {
    echo "$c.c states no licence and may not be built in" >&2; exit 1; }
done
grep -q 'license:GPL-2.0' "$VGM/emu/cores/ymf278b.c" || {
  echo "libvgm's ymf278b.c is not the one this build was checked against" >&2; exit 1; }

# the chips the cores are built for (fmopn and fmopl build only what is asked for)
DEVS="SN76496 YM2413 YM2612 YM2151 SEGAPCM RF5C68 YM2203 YM2608 YM2610 YM3812 YM3526 Y8950
  YMF262 YMF278B YMZ280B YMF271 AY8910 32X_PWM GAMEBOY NES_APU YMW258 UPD7759 MSM6258 MSM6295
  K051649 K054539 C6280 C140 C219 K053260 POKEY QSOUND SCSP VBOY_VSU SAA1099 ES5503 X1_010
  C352 GA20 MIKEY"
DEFS=(-DHAVE_STDINT_H -DVGM_LITTLE_ENDIAN -DSNDDEV_SELECT)
for d in $DEVS; do DEFS+=("-DSNDDEV_$d"); done
# the flags ZXTune built libvgm with, so that the cores sound as they did
FLAGS=(-O2 -DNDEBUG -funroll-loops -finline-functions -fdata-sections -ffunction-sections
       -funsigned-char -fno-strict-aliasing)

BUILD="$SRC/bbmp-vgm-build"
rm -rf "$BUILD"
mkdir -p "$BUILD"
compile() {
  local f="$1" o="$BUILD/$(basename "${1%.*}").o"
  case "$f" in
    *.cpp) g++ -std=c++20 "${FLAGS[@]}" "${DEFS[@]}" -I"$VGM" -c "$f" -o "$o" ;;
    *) gcc "${FLAGS[@]}" "${DEFS[@]}" -I"$VGM" ${CORE_W:-} -c "$f" -o "$o" ;;
  esac
}
export -f compile
export BUILD VGM
export FLAGS_S="${FLAGS[*]}" DEFS_S="${DEFS[*]}"
UNITS=()
for c in $CORES; do UNITS+=("$VGM/emu/cores/$c.c"); done
printf '%s\n' "${UNITS[@]}" | CORE_W=-w xargs -P "$JOBS" -I{} bash -c \
  'FLAGS=($FLAGS_S); DEFS=($DEFS_S); compile "$1"' _ {}
printf '%s\n' "$MINE"/*.cpp "$MINE"/*.c | xargs -P "$JOBS" -I{} bash -c \
  'FLAGS=($FLAGS_S); DEFS=($DEFS_S); compile "$1"' _ {}
g++ "${FLAGS[@]}" -o "$PREFIX/bin/bbmp-vgm" "$BUILD"/*.o -lz -lm $ICONV

# the licences and where each part comes from go with the program
cp "$MINE/README.txt" "$MINE/NOTICES.txt" "$MINE/GPL-2.0.txt" "$MINE/LGPL-2.1.txt" "$PREFIX/share/bbmp-vgm/"
echo "installed $PREFIX/bin/bbmp-vgm"
