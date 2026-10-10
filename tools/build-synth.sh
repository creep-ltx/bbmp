#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-synth: replayers of small synthesizer and tracker formats in one
# helper binary, one engine for each:
#
#   protrekkr  ProTrekkr, NoiseTrekker (.ptk .ntk)  ProTrekkr's replayer, BSD 2-clause
#   kdm        Ken's Digital Music (.kdm)           written for this player from a description
#                                                   of the format (clean room), GPL 3 or later
#   skale      Skale Tracker (.skm)                 libopenmpt (BSD 3-clause) with L. Preiner's
#                                                   SKM loader (BSD 2-clause)
#   buz        Buzzic (.buz), Buzzic 2 (.buz2)     written for this player from a description
#                                                   of the formats (clean room), GPL 3 or later
#   jxs        JayTrax (.jxs)                       written for this player from a description
#                                                   of the format (clean room), GPL 3 or later
#   sbstudio   SBStudio (.pac)                      libpac 0.9.0, ISC
#
# The replayers of ProTrekkr and libpac are taken from
# rePlayer (github.com/arnaud-neny/rePlayer), which has them in a form that builds
# for 64 bits. Sources go to ~/.local/src, the result to ~/.local/opt/bbmp-synth.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
SRC="$BBMP_SRC"
PREFIX="${1:-$BBMP_OPT/bbmp-synth}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
C="$HERE/csrc/synth"

REPLAYER=e3bf02cb41a40cdaebf8dc80efc0ae5ac488dc38      # rePlayer 0.22.14
OPENMPT=0eafb124cfd15f94e32302734607e71f2c36c84f       # OpenMPT r25644, libopenmpt 0.9.0-pre.13
SKMLOADER=27dde229f19bd8a1c05ba04314ed5e17a086fcee     # skale-skm-loader-openmpt

mkdir -p "$SRC" "$PREFIX/bin"
cd "$SRC"

# waits for the compilers started in the background; fails when one of them did
# at most JOBS compilers at once (default 4): one per file, all at once, is what
# filled the memory on the night of 30 September 2026
throttle() {
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 0.2; done
}
waitall() {
  local failed=0 pid
  for pid in $(jobs -p); do wait "$pid" || failed=1; done
  return $failed
}

# fetch <folder> <repository> <commit> [folders to check out...]: one commit and no history
fetch() {
  local dir="$1" url="$2" commit="$3"
  shift 3
  if kept; then         # bbmp-sources' archive; rePlayer's fresh each time (it is changed below)
    local name="$dir"
    [ "$dir" = rePlayer-synth ] && { name=rePlayer; rm -f "$dir/.bbmp-sources"; }
    kept_unpack "$name-${commit:0:7}.tar.xz" "$dir"
    return
  fi
  if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" != "$commit" ]; then
    mkdir -p "$dir"
    git -C "$dir" init -q
    git -C "$dir" remote add origin "$url" 2>/dev/null || git -C "$dir" remote set-url origin "$url"
    if [ $# -gt 0 ]; then
      git -C "$dir" sparse-checkout set "$@"
      git -C "$dir" fetch -q --depth 1 --filter=blob:none origin "$commit"
    else
      git -C "$dir" fetch -q --depth 1 origin "$commit"
    fi
    git -C "$dir" checkout -q -f FETCH_HEAD
  fi
}

R=source/Replays
fetch rePlayer-synth https://github.com/arnaud-neny/rePlayer.git "$REPLAYER" \
  $R/ProTrekkr $R/SBStudio
fetch openmpt-skm https://github.com/OpenMPT/openmpt.git "$OPENMPT"
fetch skale-skm-loader-openmpt https://github.com/lpreiner/skale-skm-loader-openmpt.git "$SKMLOADER"

RP="$SRC/rePlayer-synth/$R"
# a file of rePlayer as it was before the changes below (unpacked fresh from bbmp-sources)
restore() { kept || git -C "$SRC/rePlayer-synth" checkout -q -- "$@"; }
B="$SRC/rePlayer-synth/build"
mkdir -p "$B"

# One object file for an engine, in which only its synthw_* functions can be seen
# from outside: the replayers are full of global names. Groups of sections are
# taken apart first (--force-group-allocation), or what two engines have in common
# (the templates of the C++ library) would be dropped from the second.
bundle() {
  rm -f "$B/$1.o"
  one_object --groups "$B/$1.o" 'synthw_*' -- "$B/$1"/*.o
}

# ---- protrekkr ----
P="$RP/ProTrekkr/protrekkr"
# two of its headers say "unsigned long long" where the others say uint64_t
sed -i 's/^typedef unsigned long long Uint64;/#include <stdint.h>\ntypedef uint64_t Uint64;/' \
  "$P/replay/include/synth.h" "$P/samples/aiff/include/aiff.h"
# Songs from before the portable format (NoiseTrekker, ProTrekkr 1) hold the
# synthesizer's settings as the structure was in memory, 156 bytes of it. It has
# grown by two bytes and with its padding by four, of which the loader takes off two.
grep -q 'modland' "$P/files/synths.cpp" || sed -i \
  's/sizeof(Synth_Parameters) - \(.*\) - 2, 1, in);/156 - \1, 1, in); \/* modland: the size it had in these versions *\//' \
  "$P/files/synths.cpp"
[ "$(grep -c 'modland: the size' "$P/files/synths.cpp")" = 3 ]
# The loader reads the unpacked song from memory without looking at its end, and
# takes a sample's length as the file says: ptk_engine.cpp reads for it and says
# whether a sample fits in what is left. A song with no tempo divided by zero.
restore "$R/ProTrekkr/protrekkr/files/files.cpp" "$R/ProTrekkr/protrekkr/replay/replay.cpp"
sed -i \
  -e 's|^int Read_Mod_Data(void \*Datas, int Unit, int Length, ReplayerFile& Handle);|&\nvoid modland_read_mem(void* to, long bytes); int modland_sample_fits(long length, ReplayerFile\& in); /* modland */|' \
  -e 's|memcpy(Data, Mod_Memory + Mod_Mem_Pos, Unit \* Length);|modland_read_mem(Data, (long)Unit * Length); /* modland */|' \
  -e 's|memcpy(&svalue, Mod_Memory + Mod_Mem_Pos, Unit \* Length);|modland_read_mem(\&svalue, 2); /* modland */|' \
  -e 's|memcpy(&ivalue, Mod_Memory + Mod_Mem_Pos, Unit \* Length);|modland_read_mem(\&ivalue, 4); /* modland */|' \
  -e 's|^\( *\)Allocate_Wave(swrite, slwrite, Sample_Length\[swrite\]\[slwrite\], 1, FALSE, NULL, NULL);|\1if(!modland_sample_fits(Sample_Length[swrite][slwrite], in)) Sample_Length[swrite][slwrite] = 0; /* modland */\n&|' \
  "$P/files/files.cpp"
[ "$(grep -c 'modland' "$P/files/files.cpp")" = 5 ]
sed -i \
  's|^    SamplesPerTick = (int) ((60 \* MIX_RATE) / (Beats_Per_Min \* Ticks_Per_Beat));|    SamplesPerTick = (Beats_Per_Min * Ticks_Per_Beat) ? (int) ((60 * MIX_RATE) / (Beats_Per_Min * Ticks_Per_Beat)) : 0; /* modland */|' \
  "$P/replay/replay.cpp"
[ "$(grep -c 'modland' "$P/replay/replay.cpp")" = 1 ]
mkdir -p "$B/protrekkr"
PTK_FLAGS="-O2 -std=gnu++17 -fPIC -w -D__STAND_ALONE__ -D__WINAMP__ -D__REPLAYER__
  -D__MP3_CODEC__ -D__GSM_CODEC__ -D__AT3_CODEC__ -I$P -I$C/ptk_win -I$C"
for f in editors/patterns_blocks files/303s files/files files/reverbs files/synths replay/replay \
         replay/samples_unpack replay/spline replay/synth replay/tb_303; do
  throttle
  g++ $PTK_FLAGS -c "$P/$f.cpp" -o "$B/protrekkr/$(basename "$f").o" &
done
throttle
g++ $PTK_FLAGS -D__GCC__ -c "$P/replay/wavpack.cpp" -o "$B/protrekkr/wavpack.o" &
throttle
g++ $PTK_FLAGS -c "$C/ptk_engine.cpp" -o "$B/protrekkr/ptk_engine.o" &
waitall
bundle protrekkr

# kdm, jxs and buz are written for this player in a clean room; nothing of this helper is
# left out of a package
rm -f "$PREFIX/left-out.txt"
ENGINES=(protrekkr kdm skale buz jxs sbstudio)

# ---- kdm ----
# Ken's Digital Music written for this player (csrc/synth/kdm_engine.c); waves.kwv,
# the instruments, comes with the songs from Modland
mkdir -p "$B/kdm"
gcc -O2 -std=gnu17 -fPIC -Wall -I"$C" -c "$C/kdm_engine.c" -o "$B/kdm/kdm_engine.o"
bundle kdm

# ---- buz ----
# Buzzic 1 and Buzzic 2 written for this player (csrc/synth/buz_engine.cpp). Single
# precision step by step: no fused multiply-add, or the sound is no longer bit for bit
mkdir -p "$B/buz"
g++ -O2 -std=gnu++17 -fPIC -Wall -ffp-contract=off -I"$C" -c "$C/buz_engine.cpp" -o "$B/buz/buz_engine.o"
bundle buz

# ---- jxs ----
# JayTrax written for this player (csrc/synth/jxs_engine.cpp). Its integers wrap at
# 32 bits as the original's did (-fwrapv), and its doubles are computed step by step
# (no fused multiply-add), or the sound is no longer bit for bit
mkdir -p "$B/jxs"
g++ -O2 -std=gnu++17 -fPIC -Wall -ffp-contract=off -fwrapv -I"$C" -c "$C/jxs_engine.cpp" -o "$B/jxs/jxs_engine.o"
bundle jxs

# ---- sbstudio ----
L="$RP/SBStudio/libpac/src"
# libpac's loader stores a sound under the number the file gives it, in a table of
# 255 in the module, and sheets and sounds as many as there are blocks of them:
# a damaged file wrote past them (and the module's pointers after them, which
# pac_close() then freed). An instrument number of a note over 254 was looked up
# past the table. A module refused for having more than 255 sheets had them freed,
# all of them, from its table of 255. And the loader plays a song through to find
# its length, as far as 2^63 bytes of sound: a song that never ends kept it there
# until the helper's alarm; an hour now, as far as the helper plays.
restore "$R/SBStudio/libpac/src/load.c"
sed -i \
  -e 's|^   m->soundmap\[s.number+1\] = i+1;|   if (s.number + 1 >= PAC_SOUND_MAX \|\| i + 1 > m->soundcnt) { free (s.sample); return -1; } /* modland */\n&|' \
  -e 's|^   s = &m->sheet\[i\];|   if (i >= m->sheetcnt) return -1; /* modland */\n&|' \
  -e 's|^         n->instr = pb;|         n->instr = pb < PAC_SOUND_MAX ? pb : 0; /* modland */|' \
  -e 's|^   for (i = 0; i < m->sheetcnt; i++)$|   for (i = 0; i < m->sheetcnt \&\& i < PAC_SHEET_MAX; i++) /* modland */|' \
  -e 's|^   if (pac_read (m, NULL, LONG_MAX / 8 \* 8) < (LONG_MAX / 8 \* 8))$|   if (pac_read (m, NULL, pac_rate * pac_framesize * 3600L) < pac_rate * pac_framesize * 3600L) /* modland */|' \
  "$L/load.c"
[ "$(grep -c 'modland' "$L/load.c")" = 5 ]
mkdir -p "$B/sbstudio"
for f in effect error gus init load mix read seek; do
  gcc -O2 -std=gnu17 -fPIC -w -DNDEBUG -I"$L" -c "$L/$f.c" -o "$B/sbstudio/$f.o"
done
gcc -O2 -std=gnu17 -fPIC -Wall -DNDEBUG -I"$L" -I"$C" -c "$C/sbstudio_engine.c" -o "$B/sbstudio/sbstudio_engine.o"
bundle sbstudio

# ---- skale: libopenmpt with the SKM loader ----
O="$SRC/openmpt-skm"
# The loader, with a hook through which the engine loads a song again with other
# gains for its instruments (the sends to an effect bus, see csrc/synth/skale_fx.cpp):
# when modland_skm_hook is set, instrument n gets gain modland_skm_gain[n] in place
# of its fader, and no BAL.
cp skale-skm-loader-openmpt/Load_skm.cpp "$B/Load_skm.cpp"
sed -i \
  -e '0,/^OPENMPT_NAMESPACE_BEGIN$/s//extern "C" { int modland_skm_hook = 0; float modland_skm_gain[256]; }  \/\/ modland\n\n&/' \
  -e 's/^\(\t\t\t\t\t\)std::vector<SKMChunk> sub;$/\1if(modland_skm_hook) mixerBalance = 0.0f;  \/\/ modland\n&/' \
  -e 's/mixerInstrumentGains.emplace_back(instrIndex, mixerVolume.value_or(1.0f));/mixerInstrumentGains.emplace_back(instrIndex, modland_skm_hook ? modland_skm_gain[recordIndex] : mixerVolume.value_or(1.0f));  \/\/ modland/' \
  "$B/Load_skm.cpp"
[ "$(grep -c '// modland' "$B/Load_skm.cpp")" = 3 ]
cmp -s "$B/Load_skm.cpp" "$O/soundlib/Load_skm.cpp" || cp "$B/Load_skm.cpp" "$O/soundlib/Load_skm.cpp"
grep -q ProbeFileHeaderSKM "$O/soundlib/Sndfile.h" || sed -i \
  -e 's|^\(\s*\)static ProbeResult ProbeFileHeaderXM(MemoryFileReader file, const uint64 \*pfilesize);|&\n\1static ProbeResult ProbeFileHeaderSKM(MemoryFileReader file, const uint64 *pfilesize);|' \
  -e 's|^\(\s*\)bool ReadXM(FileReader &file, ModLoadingFlags loadFlags = loadCompleteModule);|&\n\1bool ReadSKM(FileReader \&file, ModLoadingFlags loadFlags = loadCompleteModule);|' \
  "$O/soundlib/Sndfile.h"
grep -q 'MPT_DECLARE_FORMAT(SKM)' "$O/soundlib/Sndfile.cpp" || sed -i \
  's|^\(\s*\)MPT_DECLARE_FORMAT(XM),|&\n\1MPT_DECLARE_FORMAT(SKM),|' "$O/soundlib/Sndfile.cpp"
grep -q '"skm"' "$O/soundlib/Tables.cpp" || sed -i \
  's|^\(\s*\){ UL_("Scream Tracker Music Interface Kit"), "stx" },|\1{ UL_("Skale Tracker"),                      "skm" },\n&|' \
  "$O/soundlib/Tables.cpp"
[ "$(grep -c SKM "$O/soundlib/Sndfile.h")" = 2 ]
grep -q 'MPT_DECLARE_FORMAT(SKM)' "$O/soundlib/Sndfile.cpp"
grep -q '"skm"' "$O/soundlib/Tables.cpp"
# without the libraries for MP3, Ogg and so on: libopenmpt then uses the decoders it
# brings along (SKM has samples in Ogg Vorbis), and the helper needs nothing more
make -C "$O" -j"$JOBS" CONFIG=gcc STATIC_LIB=1 SHARED_LIB=0 EXAMPLES=0 OPENMPT123=0 TEST=0 \
  NO_ZLIB=1 NO_MPG123=1 NO_OGG=1 NO_VORBIS=1 NO_VORBISFILE=1 NO_PORTAUDIO=1 NO_PORTAUDIOCPP=1 \
  NO_PULSEAUDIO=1 NO_SDL2=1 NO_SNDFILE=1 NO_FLAC=1 VERBOSE=0 bin/libopenmpt.a > "$SRC/openmpt-skm.log" 2>&1 ||
  { tail -30 "$SRC/openmpt-skm.log"; exit 1; }
g++ -O2 -std=gnu++17 -Wall -I"$O" -I"$C" -c "$C/skale_engine.cpp" -o "$B/skale_engine.o"
g++ -O2 -std=gnu++17 -Wall -I"$C" -c "$C/skale_fx.cpp" -o "$B/skale_fx.o"

DEFS=()
OBJS=()
for e in "${ENGINES[@]}"; do
  DEFS+=("-DWITH_$(echo "$e" | tr a-z A-Z)")
  [ "$e" = skale ] || OBJS+=("$B/$e.o")
done
g++ -O2 -std=gnu++17 -Wall "${DEFS[@]}" -I"$C" -o "$PREFIX/bin/bbmp-synth" "$C/bbmp-synth.cpp" \
  "${OBJS[@]}" "$B/skale_engine.o" "$B/skale_fx.o" \
  "$O/bin/libopenmpt.a" -lsamplerate -lz -lm
# mda-vst's MIT notice goes with the program
mkdir -p "$PREFIX/share/licenses"
install -m644 "$C/LICENSE.mda" "$PREFIX/share/licenses/"
echo "installed $PREFIX/bin/bbmp-synth"
