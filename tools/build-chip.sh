#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-chip: StSound (YM), AdPlug (AdLib), mdxmini (X68000 MDX), sc68 (SC68),
# 98fmplayer (PC-98 FMP), libkss (MSX), EUPPlayer (FM Towns), organya.h (Organya),
# libpxtone (Piston Collage), Highly Quixotic (Capcom QSF) and pmdc (PMD: the player's
# own driver on ymfm's YM2608) in one helper binary, under the GPL 3.
# (psgplay, SNDH, is GPL 2 only and snsf9x, SNSF, Snes9x's non-commercial code: neither
# may be combined with GPL 3 code, so since 7 October 2026 they are helpers of their own,
# tools/build-psgplay.sh and tools/build-snsf.sh.)
# Sources go to ~/.local/src, the result to ~/.local/opt/bbmp-chip.
# (pmdmini, PMDWin and fmgen, played PMD until 1 October 2026: their terms forbid passing
# them on, so pmdc, written for this player in a clean room, replaced them.)
set -euo pipefail
. "$(dirname "$0")/sources.sh"
SRC="$BBMP_SRC"
PREFIX="${1:-$BBMP_OPT/bbmp-chip}"
ADPLUG="$BBMP_OPT/adplug"
SC68="$BBMP_OPT/sc68"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$SRC" "$PREFIX/bin"
rm -f "$PREFIX/left-out.txt"
cd "$SRC"

# clone <github repo> <folder> <commit> [nosub]: that commit (the one bbmp-sources keeps, the
# version the player was built and tested with) from the internet, or its archive from
# bbmp-sources
clone() {
  if kept; then kept_unpack "$2-${3:0:7}.tar.xz" "$2"; return; fi
  git_pin "https://github.com/$1.git" "$2" "$3" "${4:-}"
}
clone arnaud-carre/StSound StSound a27c29339bed5076cd3342cae8270d2e813b914c
clone mistydemeo/mdxmini mdxmini 003531a471c1955f4ed4357d0e2a6cba809c34a0
clone adplug/libbinio libbinio 3baed731da6b44883c6820132fb1d2a44abc0939
clone adplug/adplug adplug 1e6133f94abd99625c628fd01d7a9c8b681be7de
clone takamichih/fmplayer fmplayer 4fa914e4b2b994cb3ccf92d571a20a7cdf1fe36a
clone digital-sound-antiques/libkss libkss 15fb77fbb949d9d679df7e9652723b2c41b9f46e
clone tjhayasaka/eupplayer eupplayer 8587733b18598e04ffea89c4b7a35c53b452d959
clone Strultz/organya.h organya.h f7a086fc00f21de5e1c410d38d7412627b8d2cf0
clone Wohlstand/libpxtone libpxtone dec7b6015a3f2736603d02bfb5e4b9bd453fc41c
# its two submodules are taken below, at other commits (its own pointers are ssh URLs)
clone kode54/deadbeef_hq deadbeef_hq b92e40f5269f3c9eb95ce1b0ce49ac892c3a7a9b nosub
# deadbeef_hq's own submodules point at versions from before psflib had a licence (MIT,
# May 2015) and Highly Quixotic one (GPL-3, July 2015): take those two commits instead.
# The code is the same but for one cast in qsound.c and psf_load's last argument.
if ! kept; then
  # a submodule's folder is there but empty (deadbeef_hq names them, nosub leaves them out)
  for d in deadbeef_hq/psflib deadbeef_hq/QSoundCore; do
    [ -e "$d/.git" ] || rm -rf "$d"
  done
  git_pin https://github.com/kode54/psflib.git deadbeef_hq/psflib 4d957c21bf6134072d39a8fe7a014206f98cd975
  git_pin https://github.com/kode54/Highly_Quixotic.git deadbeef_hq/QSoundCore 0cf02d89db9d936239a2d6ab99f5fc2c0a9a182b
fi
grep -q 'MIT License' deadbeef_hq/psflib/LICENSE && grep -q 'GENERAL PUBLIC LICENSE' deadbeef_hq/QSoundCore/LICENSE.TXT || {
  echo "psflib or Highly Quixotic is not the licensed version (psflib 4d957c2, Highly Quixotic 0cf02d8)" >&2; exit 1; }
# EUPPlayer mixes without clamping, so loud passages wrap around; clip them instead
grep -q 'modland: clip' eupplayer/eupplayer_townsEmulator.cc || \
  sed -i 's|      d >>= 10;\t\t\t// .*|&\n      d = d > 32767 ? 32767 : d < -32768 ? -32768 : d;  /* modland: clip */|' \
    eupplayer/eupplayer_townsEmulator.cc

# StSound works out the envelope's step as clock / (period * rate) in 32 bits:
# periods above 44739 (at 48 kHz) overflow, and a slow envelope runs fast instead
grep -q '(yms64)per \* replayFrequency' StSound/StSoundLibrary/Ym2149Ex.cpp || \
  sed -i 's|step /= (per \* replayFrequency);|step /= ((yms64)per * replayFrequency);|' \
    StSound/StSoundLibrary/Ym2149Ex.cpp

# mdxmini follows a track's jumps without looking where they lead: a song whose
# track goes past its data (Metal Sight's metal05.mdx) reads wild memory and may
# crash. There the track ends, as at its end mark.
grep -q 'modland: past the data' mdxmini/src/mdxmml_ym2151.c || \
  sed -i 's|^  ptr = mdx->track\[t\].current_mml_ptr;$|&\n  if ( ptr < 0 \|\| ptr >= mdx->length ) { note_off(t, songdata); return -1; }  /* modland: past the data */|' \
    mdxmini/src/mdxmml_ym2151.c
make -C mdxmini -f Makefile.lib -j"$JOBS"
if [ ! -f "$ADPLUG/lib/libadplug$SO" ]; then
  for p in libbinio adplug; do
    cmake -S "$p" -B "$p/build" -DCMAKE_INSTALL_PREFIX="$ADPLUG" -DCMAKE_PREFIX_PATH="$ADPLUG" -DCMAKE_INSTALL_LIBDIR=lib \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
    cmake --build "$p/build" -j"$JOBS"
    cmake --install "$p/build"
  done
fi

if kept; then
  kept_unpack sc68-2.2.1.tar.gz sc68-2.2.1
elif [ ! -d sc68-2.2.1 ]; then
  curl -sL -o sc68.tgz https://downloads.sourceforge.net/project/sc68/sc68/2.2.1/sc68-2.2.1.tar.gz
  echo "d7371f0f406dc925debf50f64df1f0700e1d29a8502bb170883fc41cc733265f  sc68.tgz" | sha256_check
  tar xzf sc68.tgz
fi

# The libraries read damaged files without looking where (the final audit of 7 October
# 2026, csrc/chip/*.patch, each says what it changes): libkss's OPX converter writes past
# its buffer for an OPX over 32 KB; StSound's YM and Digi-Mix loader and its LZH unpacker
# take sizes and counts from the file; sc68 follows a chunk's size past the file's end.
apply_patches . "$HERE"/csrc/chip/*.patch

# sc68 is built again when its patch changed (or it was built before the patch)
SC68_STAMP="$(cat "$HERE/csrc/chip/sc68-"*.patch | cksum)"
if [ ! -f "$SC68/lib/libapi68.a" ] || [ "$(cat "$SC68/.bbmp-patches" 2>/dev/null)" != "$SC68_STAMP" ]; then
  (cd sc68-2.2.1 && ./configure --prefix="$SC68" CFLAGS="-O2 -fPIC -std=gnu89 $OLD_C" \
     && sed -i '/^SUBDIRS/s/ info\b//; /^SUBDIRS/s/ man\b//' Makefile \
     && make -j"$JOBS" && make install)   # its texinfo manual doesn't build, so skip it
  echo "$SC68_STAMP" > "$SC68/.bbmp-patches"
fi
# Of sc68's 84 replay routines only its own six stubs (by Benjamin Gerard: they call the
# code inside the song file) stay installed. The other 78 are routines ripped from games,
# editors and demos, with no terms that allow passing them on (bbmp's survey, 7 October
# 2026): the player downloads them on request from sc68 2.2.1 on SourceForge
# (src/bbmp/downloads.py, "sc68replays"), and bbmp-chip finds them through
# BBMP_SC68_REPLAYS.
FINE_REPLAYS=(dwaga mon none note note2 timerc)
REPLAYS="$SC68/share/sc68/Replay"
for f in "$REPLAYS"/*; do
  printf '%s.bin\n' "${FINE_REPLAYS[@]}" | grep -qxF "$(basename "$f")" || rm -f "$f"
done
left="$(ls "$REPLAYS" | LC_ALL=C sort | tr '\n' ' ')"
want="$(printf '%s.bin\n' "${FINE_REPLAYS[@]}" | LC_ALL=C sort | tr '\n' ' ')"
[ "$left" = "$want" ] || {
  echo "sc68's Replay folder holds more (or less) than the routines that may be passed on:" >&2
  echo "  there: $left" >&2; echo "  wanted: $want" >&2; exit 1; }
# sc68's sample song (Sample/About-Intro.sc68, Tao's "YM - Jam" with its own replay code)
# is someone else's music, not given to us to pass on: it is not installed.
rm -rf "$SC68/share/sc68/Sample"

# libkss: its CMake flags don't allow C23's stricter prototypes, so add -std=gnu17.
# Of the six MSX drivers libkss builds in (its kss-drivers submodule), MGSDRV and
# MoonBlaster's may be passed on; MuSICA's (kinrou5), MPK's (mpk106, mpk103) and
# OPLLDRV's (opx4kss) terms don't allow it. libkss's own switches leave those four out;
# the player downloads them on request (downloads.py) and bbmp-chip hands them to libkss
# from BBMP_MSX_DRIVERS.
cmake -S libkss -B libkss/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_C_FLAGS_RELEASE="-O2 -std=gnu17 -DEXCLUDE_DRIVER_KINROU -DEXCLUDE_DRIVER_MPK106 -DEXCLUDE_DRIVER_OPX4KSS"
cmake --build libkss/build -j"$JOBS"
# Left out, their arrays are all zeros and so not in the data section (b, not d): check
# that. (MPK103 has no switch of its own: EXCLUDE_DRIVER_MPK106 leaves out both.)
if nm libkss/build/libkss.a 2>/dev/null | grep -E ' [dD] _?(KINROU|MPKDRV106|MPKDRV103|OPXDRV)$'; then
  echo "libkss still has MuSICA's, MPK's or OPLLDRV's driver built in" >&2; exit 1
fi

# 98fmplayer: its OPNA emulation, FM drivers and file loader, as C17
FMP_SRC="fmplayer/libopna/opna.c fmplayer/libopna/opnaadpcm.c fmplayer/libopna/opnadrum.c
  fmplayer/libopna/opnafm.c fmplayer/libopna/opnassg.c fmplayer/libopna/opnassg-sinc-c.c
  fmplayer/libopna/opnatimer.c fmplayer/fmdriver/fmdriver_fmp.c fmplayer/fmdriver/fmdriver_pmd.c
  fmplayer/fmdriver/fmdriver_common.c fmplayer/fmdriver/ppz8.c fmplayer/common/fmplayer_file.c
  fmplayer/common/fmplayer_file_unix.c fmplayer/common/fmplayer_work_opna.c
  fmplayer/common/fmplayer_drumrom_unix.c $HERE/csrc/chip/fmp_engine.c"
mkdir -p fmplayer/build
for f in $FMP_SRC; do
  gcc -O2 -std=gnu17 -fPIC -Ifmplayer -I"$HERE/csrc/chip" -c "$f" -o "fmplayer/build/$(basename "${f%.c}").o"
done
# One relocatable object whose only public names are fmpw_*, so 98fmplayer's
# PMD driver can't collide with other functions of the same names.
rm -f fmplayer/build/fmp-all.o fmplayer/build/fmp.o
one_object fmplayer/build/fmp.o 'fmpw_*' -- fmplayer/build/*.o

# ymfm (Aaron Giles, BSD-3-Clause) as the YM2608 for pmdc: PMD songs on the player's own
# driver (csrc/chip/pmdc_driver.cpp, written from a description of PMD's behaviour)
YMFM=81aec25ccbb98f4873a255f7551ac4dadac59b4a
if kept; then
  kept_unpack "ymfm-${YMFM:0:7}.tar.gz" ymfm
elif [ "$(cat ymfm/.bbmp-commit 2>/dev/null)" != "$YMFM" ]; then
  curl -sL -o ymfm.tar.gz "https://codeload.github.com/aaronsgiles/ymfm/tar.gz/$YMFM"
  echo "9bcf001b30f53754c49cf2fb864792a64702b8aeebb105baa908ad5dd33f443f  ymfm.tar.gz" | sha256_check
  rm -rf ymfm && mkdir ymfm && tar xzf ymfm.tar.gz -C ymfm --strip-components=1 && rm ymfm.tar.gz
  echo "$YMFM" > ymfm/.bbmp-commit
fi
mkdir -p ymfm/build
for f in ymfm/src/ymfm_opn.cpp ymfm/src/ymfm_adpcm.cpp ymfm/src/ymfm_ssg.cpp \
         "$HERE/csrc/chip/pmdc_driver.cpp" "$HERE/csrc/chip/pmdc_engine.cpp"; do
  g++ -O2 -std=gnu++17 -fPIC -Iymfm/src -I"$HERE/csrc/chip" -c "$f" -o "ymfm/build/$(basename "${f%.cpp}").o"
done

gcc -O2 -std=gnu17 -Iorganya.h -c "$HERE/csrc/chip/organya_impl.c" -o organya.h/organya.o
# Highly Quixotic (QSF) and psflib, plain C
mkdir -p deadbeef_hq/build
for f in deadbeef_hq/psflib/psflib.c deadbeef_hq/psflib/psf2fs.c deadbeef_hq/QSoundCore/Core/*.c \
         "$HERE/csrc/chip/qsf_engine.c"; do
  gcc -O2 -std=gnu17 -fPIC -DEMU_COMPILE -DEMU_LITTLE_ENDIAN -Ideadbeef_hq/psflib \
    -Ideadbeef_hq/QSoundCore/Core -I"$HERE/csrc/chip" -c "$f" -o "deadbeef_hq/build/$(basename "${f%.c}").o"
done
# Organya's sound bank (organya.h/default.wdb, Studio Pixel's samples) has no terms that let
# it be passed on: it is not installed; the player downloads it ("organyabank") and hands it
# to bbmp-chip in BBMP_ORGANYA_BANK. A bank an earlier build installed goes.
rm -f "$PREFIX/share/default.wdb"

g++ -O2 -std=gnu++17 -o "$PREFIX/bin/bbmp-chip" "$HERE/csrc/chip/bbmp-chip.cpp" \
  StSound/StSoundLibrary/*.cpp StSound/StSoundLibrary/LZH/*.cpp -IStSound/StSoundLibrary \
  -Imdxmini/src mdxmini/obj/libmdxmini.a \
  -I"$SC68/include/sc68" -DSC68_SHARE="\"$SC68/share/sc68\"" \
  "$SC68/lib/libapi68.a" "$SC68/lib/libio68.a" "$SC68/lib/libemu68.a" "$SC68/lib/libfile68.a" \
  "$SC68/lib/libunice68.a" -lz \
  -I"$HERE/csrc/chip" fmplayer/build/fmp.o ymfm/build/*.o -lsamplerate \
  -Ilibkss/src -Ilibkss/modules libkss/build/libkss.a libkss/build/modules/*/lib*.a \
  -Ieupplayer eupplayer/eupplayer.cc eupplayer/eupplayer_townsEmulator.cc \
  eupplayer/eupplayer_townsInternal.cc eupplayer/sintbl.cc \
  -Iorganya.h organya.h/organya.o \
  deadbeef_hq/build/*.o -lz \
  -Ilibpxtone/src libpxtone/src/*.cpp -DpxINCLUDE_OGGVORBIS -DpxINCLUDE_OGGVORBIS_STB -Ilibpxtone/stb_vorbis \
  -I"$ADPLUG/include" -L"$ADPLUG/lib" -Wl,-rpath,"$ADPLUG/lib" -ladplug -llibbinio -lm $ICONV
echo "installed $PREFIX/bin/bbmp-chip"
