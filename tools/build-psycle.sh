#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-psycle: the player of Psycle for Windows (psycle.sourceforge.net, GPL 2
# or later) without its windows, and Psycle's native plugins as shared libraries.
# Sources go to ~/.local/src/psycle-r<revision>, the result to ~/.local/opt/bbmp-psycle:
#   bin/bbmp-psycle          the helper
#   lib/psycle-plugins/*.so     the plugins, named as songs name them ("arguru-synth-2f.so")
#   lib/psycle-plugins/before-2010/*.so   plugins as they were, for the songs of then
# Needs svn, g++, python3, patch.
set -euo pipefail
. "$(dirname "$0")/sources.sh"
REV=12005                                   # of svn.code.sf.net/p/psycle/code, 2025-05-08
SVN="https://svn.code.sf.net/p/psycle/code/trunk"
STK_SHA256=35c7d5dec90c6a23c907d4cfb5f1dd5800242b89eaafd89a17d90808747eed4a
SRC="$BBMP_SRC/psycle-r$REV"
PREFIX="${1:-$BBMP_OPT/bbmp-psycle}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MY="$HERE/csrc/psycle"
mkdir -p "$SRC/pristine" "$PREFIX/bin" "$PREFIX/lib/psycle-plugins"
export JOBS                                 # build_plugins.py: compilers at once
# from bbmp-sources: its archive is the pristine folder, as svn exported it
if kept; then kept_unpack "psycle-r$REV.tar.xz" "$SRC/pristine"; fi

# ---- the sources, as they are in Psycle's repository ----
fetch() {   # <path in trunk> <path here>
  [ -e "$SRC/pristine/$2" ] && return 0
  mkdir -p "$(dirname "$SRC/pristine/$2")"
  rm -rf "$SRC/pristine/$2.part"
  svn export -q -r "$REV" "$SVN/$1" "$SRC/pristine/$2.part"
  mv "$SRC/pristine/$2.part" "$SRC/pristine/$2"
}
fetch psycle/src/psycle/host psycle/src/psycle/host
fetch psycle-helpers/src psycle-helpers/src
fetch universalis/src universalis/src
fetch diversalis/src diversalis/src
fetch psycle-plugins/src psycle-plugins/src
fetch psycle-plugins/msvc psycle-plugins/msvc
fetch external-packages/stk-4.5.0/stk-4.5.0.tar.bz2 stk-4.5.0.tar.bz2
fetch COPYING COPYING
echo "$STK_SHA256  $SRC/pristine/stk-4.5.0.tar.bz2" | sha256_check

# ---- a tree to build in: the sources with what Linux needs changed ----
W="$SRC/build"
rm -rf "$W"
mkdir -p "$W/external-packages" "$W/obj/host" "$W/obj/pic"
cp -r "$SRC/pristine/psycle" "$SRC/pristine/psycle-helpers" "$SRC/pristine/universalis" \
      "$SRC/pristine/diversalis" "$SRC/pristine/psycle-plugins" "$W/"
tar xjf "$SRC/pristine/stk-4.5.0.tar.bz2" -C "$W/external-packages"
patch -s -d "$W" -p1 < "$MY/psycle-r12005-linux.patch"
# "throw(...)" is Microsoft's way of saying that a function may throw
{ grep -rlF 'throw(...)' "$W/psycle/src/psycle/host" "$W/psycle-plugins/src/psycle/plugin_interface.hpp" || true; } \
  | xargs -r sed -i 's/throw(\.\.\.)/noexcept(false)/g'
# the parts of the host that are windows, VST, Lua or LADSPA: csrc/psycle/compat/host has
# what the player needs of them
H="$W/psycle/src/psycle/host"
rm -f "$H"/{Psycle,PsycleConfig,VstHost24,LuaPlugin,LuaHost,LuaArray,LuaHelper,LuaInternals,ladspahost}.hpp \
      "$H/ladspamachine.h"
# files are included by names in another case than they have
python3 "$MY/fixcase.py" "$W/psycle/src" "$W/psycle-helpers/src" "$W/universalis/src" \
  "$W/diversalis/src" "$W/psycle-plugins/src"
# what a damaged song file must not do: read or write past its buffers, allocate what the
# file does not hold, divide by zero (see the "modland:" comments in the patch)
patch -s -d "$W" -p1 < "$MY/psycle-r12005-damaged.patch"
grep -q 'modland: the sizes are checked' "$W/psycle/src/psycle/host/Instrument.cpp" \
  && grep -q 'modland: a sample is no longer than the file' "$W/psycle/src/psycle/host/Song.cpp" \
  && grep -q 'modland: a byte of the file is a bool' "$W/psycle/src/psycle/host/FileIO.hpp" \
  && grep -q 'bool BEERZ77Decomp2(uint8_t const \* pSource, std::size_t sourceSize' "$W/psycle-helpers/src/psycle/helpers/datacompression.cpp" \
  && grep -q 'modland: was "volume>0.01f' "$W/psycle-plugins/src/psycle/plugins/alk_muter/alk_muter.cpp" \
  || { echo "the patch for damaged files did not apply" >&2; exit 1; }

INC="-I$MY/compat -I$MY/compat/host -I$MY -I$W/psycle/src -I$W/psycle-helpers/src
  -I$W/universalis/src -I$W/diversalis/src -I$W/psycle-plugins/src"
CXX="g++ -std=gnu++14 -O2 -w -fpermissive -DNDEBUG"
# clang (macOS) refuses what GCC lets by: an extra qualification on two members (taken
# out here, for both), narrowing in braces (let by); 64-bit ARM is a CPU diversalis
# doesn't know; and Apple's C library calls GNU's sincos __sincos
sed -i 's/static int XMSampler::CalcLPBFromSpeed/static int CalcLPBFromSpeed/' "$W/psycle/src/psycle/host/XMSampler.hpp"
sed -i -E 's/([a-zA-Z_]+ )Player::CalcPosition/\1CalcPosition/' "$W/psycle/src/psycle/host/Player.hpp"
sed -i 's/inline void WaveDataController::Work(/inline void Work(/' "$W/psycle/src/psycle/host/Sampler.hpp"
# a misspelt name in a branch only macOS reaches (_POSIX_CPUTIME == -1)
sed -i 's/process_cputime_supported = false;/process_cpu_time_supported = false;/' "$W/universalis/src/universalis/os/clocks.cpp"
# instantiated again by "template class XMInstrument::WaveData<int16_t>" further down
sed -i '/^ *template void XMInstrument::WaveData<short>::Init();/d' "$W/psycle/src/psycle/host/XMInstrument.cpp"
sed -i 's/#elif defined __arm__$/#elif defined __arm__ || defined __aarch64__/' "$W/diversalis/src/diversalis/cpu.hpp"
[ "$(uname -s)" = Darwin ] && CXX="$CXX -Wno-c++11-narrowing -include $MY/compat/darwin.h"
U="$W/universalis/src/universalis"
UNIVERSALIS="$U/exception.cpp $U/compiler/exception.compiler.cpp $U/compiler/typenameof.cpp
  $U/cpu/exception.cpu.cpp $U/os/clocks.cpp $U/os/exception.os.cpp $U/os/loggers.cpp
  $U/os/terminal.cpp $U/os/thread_name.cpp $U/stdlib/exception.stdlib.cpp"
HELPERS="$(ls "$W"/psycle-helpers/src/psycle/helpers/*.cpp)"

compile() {   # <folder of objects> <flags> <sources...>: in parallel, stops at the first error
  local out="$1" flags="$2"; shift 2
  printf '%s\n' "$@" | xargs -P "$JOBS" -I{} bash -c '
    o="$0/$(basename "${2%.cpp}").o"
    $1 -c "$2" -o "$o" || { echo "failed: $2" >&2; exit 255; }' "$out" "$CXX $flags" {}
}

# ---- the helper ----
HOST=""
for f in AudioDriver FileIO Instrument internal_machines Machine Player Plugin Preset Sampler \
         Song XMInstrument XMSampler ExclusiveLock InstPreview LoaderHelper; do
  HOST="$HOST $H/$f.cpp"
done
compile "$W/obj/host" "-DWINAMP_PLUGIN -include $MY/compat/compat.hpp $INC" \
  $HOST $HELPERS $UNIVERSALIS "$U/os/sched.cpp" "$MY/host.cpp" "$MY/bbmp-psycle.cpp"
g++ -o "$PREFIX/bin/bbmp-psycle" "$W"/obj/host/*.o $( [ "$(uname -s)" = Darwin ] || echo -ldl) -lpthread

# ---- the plugins ----
UNIQUE=-fno-gnu-unique                       # GCC's; clang (macOS) has no GNU unique symbols
[ "$(uname -s)" = Darwin ] && UNIQUE=
compile "$W/obj/pic" "-fPIC $UNIQUE -include $MY/compat/plugin.hpp $INC" $HELPERS $UNIVERSALIS
mkdir -p "$W/plugins/.obj"
rm -f "$W/plugins/.obj/libhelpers.a"
ar rcs "$W/plugins/.obj/libhelpers.a" "$W"/obj/pic/*.o
python3 "$MY/build_plugins.py" "$W" "$W/plugins" "$MY/compat"
rm -rf "$PREFIX/lib/psycle-plugins"
mkdir -p "$PREFIX/lib/psycle-plugins/before-2010"
cp "$W"/plugins/*.so "$PREFIX/lib/psycle-plugins/"
cp "$W"/plugins/before-2010/*.so "$PREFIX/lib/psycle-plugins/before-2010/"
cp "$SRC/pristine/COPYING" "$PREFIX/COPYING.psycle"

echo "built $PREFIX/bin/bbmp-psycle and $(find "$PREFIX/lib/psycle-plugins" -name "*.so" | wc -l) plugins"
