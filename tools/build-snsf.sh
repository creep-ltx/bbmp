#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-snsf: Super Nintendo songs (SNSF, MINISNSF), played by snsf9x (loveemu's
# Snes9x trimmed for sound) and read by psflib (MIT). Snes9x may be passed on for
# non-commercial purposes only, terms that can't be combined with the GPL, so this is a
# program of its own with no GPL code in it: the files written for it (csrc/snsf) are
# under the MIT licence, and Snes9x's licence goes with it. Until 7 October 2026 it was
# part of bbmp-chip. Sources go to ~/.local/src, the result to ~/.local/opt/bbmp-snsf.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
SRC="$BBMP_SRC"
PREFIX="${1:-$BBMP_OPT/bbmp-snsf}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MINE="$HERE/csrc/snsf"
mkdir -p "$PREFIX/bin" "$PREFIX/share/bbmp-snsf"
cd "$SRC"
# clone <github repo> <folder> <commit> [nosub], as tools/build-chip.sh does: that commit
clone() {
  if kept; then kept_unpack "$2-${3:0:7}.tar.xz" "$2"; return; fi
  git_pin "https://github.com/$1.git" "$2" "$3" "${4:-}"
}
clone loveemu/snsf9x snsf9x 128cb0d500c98caa815da117eb0339873cf157ae
clone kode54/deadbeef_hq deadbeef_hq b92e40f5269f3c9eb95ce1b0ce49ac892c3a7a9b nosub
# psflib from the commit that has its licence (MIT, May 2015), as tools/build-chip.sh
if ! kept; then
  [ -e deadbeef_hq/psflib/.git ] || rm -rf deadbeef_hq/psflib
  git_pin https://github.com/kode54/psflib.git deadbeef_hq/psflib 4d957c21bf6134072d39a8fe7a014206f98cd975
fi
grep -q 'MIT License' deadbeef_hq/psflib/LICENSE || { echo "psflib is not the licensed version (4d957c2)" >&2; exit 1; }
# nothing under the GPL may come in: snes_spc in snes9x/apu is LGPL 2.1, the rest Snes9x's
if grep -l 'GNU General Public License' snsf9x/snsf9x/*.cpp snsf9x/snsf9x/snes9x/*.cpp \
     snsf9x/snsf9x/snes9x/apu/*.cpp deadbeef_hq/psflib/psflib.c; then
  echo "a GPL file is among bbmp-snsf's sources" >&2; exit 1
fi

# Snes9x's DSP reads up to 0x2FF bytes past the SPC's memory on every song: more padding
# there (csrc/snsf/snsf9x-spc-padding.patch, the final audit of 7 October 2026)
apply_patches . "$MINE"/*.patch

mkdir -p snsf9x/build-snsf
for f in snsf9x/snsf9x/SNESSystem.cpp snsf9x/snsf9x/snes9x/{cpu,cpuexec,cpuops,dma,globals,memmap,ppu,sa1,sdd1}.cpp \
         snsf9x/snsf9x/snes9x/apu/{apu,SNES_SPC,SNES_SPC_misc,SNES_SPC_state,SPC_DSP,SPC_Filter}.cpp \
         "$MINE/snsf_engine.cpp"; do
  # PTR_NOT_INT: snes9x keeps pointers in "pint", 32 bits unless told otherwise
  g++ -O2 -std=gnu++17 -fPIC -w -Wno-register -DPTR_NOT_INT '-DZeroMemory(p,n)=memset((p),0,(n))' \
    -include cstring -include cstdint -Isnsf9x/snsf9x -Isnsf9x/snsf9x/snes9x \
    -Ideadbeef_hq/psflib -I"$MINE" -c "$f" -o "snsf9x/build-snsf/$(basename "${f%.cpp}").o"
done
gcc -O2 -std=gnu17 -fPIC -Ideadbeef_hq/psflib -c deadbeef_hq/psflib/psflib.c -o snsf9x/build-snsf/psflib.o
g++ -O2 -std=gnu++17 -o "$PREFIX/bin/bbmp-snsf" "$MINE/bbmp-snsf.cpp" -I"$MINE" snsf9x/build-snsf/*.o -lz
# the licences and where each part comes from go with the program
cp snsf9x/snsf9x/snes9x/docs/snes9x-license.txt "$PREFIX/share/bbmp-snsf/snes9x-license.txt"
cp snsf9x/snsf9x/snes9x/apu/license.txt "$PREFIX/share/bbmp-snsf/snes_spc-LGPL-2.1.txt"
cp deadbeef_hq/psflib/LICENSE "$PREFIX/share/bbmp-snsf/psflib-LICENSE.txt"
cp "$MINE/LICENSE" "$MINE/README.txt" "$PREFIX/share/bbmp-snsf/"
echo "installed $PREFIX/bin/bbmp-snsf"
