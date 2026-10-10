#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Set up everything ByteBandit Music Player needs, in one go.
#
#   tools/setup.sh           build what is missing, then make the catalogs
#   tools/setup.sh --check   only report what is there and what is missing
#   tools/setup.sh --rebuild build every replayer again, even the ones already there
#
# Nothing here needs root. The replayers are built from source into ~/.local/opt,
# with their sources in ~/.local/src. System packages are only checked, not installed.
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-}"
OPT="$HOME/.local/opt"
missing_system=()
failed=()

say()  { printf '\n== %s\n' "$*"; }
have() { printf '  %-28s %s\n' "$1" "found"; }
lack() { printf '  %-28s %s\n' "$1" "MISSING  $2"; }

say "System packages"
# the Python that runs the player: the clone's own .venv if there is one (see README)
PY=python3
[ -x "$HERE/.venv/bin/python3" ] && PY="$HERE/.venv/bin/python3"
# the package names, for: Arch, Debian/Ubuntu, Fedora
names() {
  case "$1" in
    libopenmpt)  echo "libopenmpt | libopenmpt-dev | libopenmpt-devel" ;;
    libgme)      echo "libgme | libgme-dev | game-music-emu-devel" ;;
    libsidplayfp) echo "libsidplayfp | libsidplayfp-dev | libsidplayfp-devel" ;;
    libsamplerate) echo "libsamplerate | libsamplerate0-dev | libsamplerate-devel" ;;
    libpulse)    echo "libpulse | libpulse-dev | pulseaudio-libs-devel" ;;
    zlib)        echo "zlib | zlib1g-dev | zlib-devel" ;;
    boost)       echo "boost | libboost-dev | boost-devel" ;;
    gcc|g++|make) echo "base-devel | build-essential | gcc gcc-c++ make" ;;
    pkg-config)  echo "pkgconf | pkg-config | pkgconf-pkg-config" ;;
    svn)         echo "subversion | subversion | subversion" ;;
    7z)          echo "7zip | 7zip (or p7zip-full) | 7zip (or p7zip p7zip-plugins)" ;;
    bwrap)       echo "bubblewrap | bubblewrap | bubblewrap" ;;
    venv)        echo "python | python3-venv | python3" ;;
    textual|numpy) echo "in the venv: .venv/bin/pip install textual numpy (see README)" ;;
    *)           echo "$1 | $1 | $1" ;;
  esac
}
hint() { case "$(names "$1")" in in\ the*) names "$1" ;; *) echo "(Arch | Debian, Ubuntu | Fedora: $(names "$1"))" ;; esac; }
# header <name> <header> [c|c++]: there only if a program can be built with it, which
# needs the development package, not only the library a player uses at run time
header() { printf '#include <%s>\n' "$2" | "${CXX:-g++}" -E -x "${3:-c++}" - >/dev/null 2>&1; }
for pair in libopenmpt:libopenmpt/libopenmpt.h libgme:gme/gme.h libsidplayfp:sidplayfp/sidplayfp.h \
            libsamplerate:samplerate.h libpulse:pulse/simple.h zlib:zlib.h boost:boost/version.hpp; do
  name="${pair%%:*}" h="${pair#*:}"
  if header "$name" "$h"; then
    have "$name (headers)"
  elif [ "$name" = boost ] && command -v pacman >/dev/null; then
    have "boost (headers: build-zxtune.sh unpacks Arch's package for itself)"
  else
    lack "$name (headers)" "$(hint "$name")"
    missing_system+=("$name")
  fi
done
for tool in gcc g++ make cmake pkg-config git curl unzip patch perl svn bison bwrap; do
  if command -v "$tool" >/dev/null; then have "$tool"; else lack "$tool" "$(hint "$tool")"; missing_system+=("$tool"); fi
done
if command -v 7z >/dev/null || command -v 7za >/dev/null || command -v 7zz >/dev/null; then have "7z (archives)"
else lack "7z" "$(hint 7z)"; missing_system+=("7z"); fi
if ! command -v bwrap >/dev/null; then
  echo "  (without bubblewrap the replayers run unprotected, with your own rights)"
fi
if "$PY" -c 'import sys; sys.exit(sys.version_info < (3, 11))' 2>/dev/null; then have "python $("$PY" -c 'import platform; print(platform.python_version())')"
else lack "python 3.11 or newer" "(Arch | Debian, Ubuntu | Fedora: python | python3 | python3)"; missing_system+=("python3.11"); fi
if [ "$PY" = python3 ] && ! python3 -c 'import venv, ensurepip' 2>/dev/null; then
  lack "python3 -m venv" "$(hint venv)"; missing_system+=("python3-venv")
fi
for module in textual numpy; do
  if "$PY" -c "import $module" 2>/dev/null; then
    have "python: $module"
  else
    lack "python: $module" "$(hint "$module")"
    missing_system+=("python-$module")
  fi
done
for module in gi; do
  if "$PY" -c "import $module" 2>/dev/null; then have "python: $module (media keys)"
  else lack "python: $module" "optional: media keys need PyGObject (python-gobject | python3-gi | python3-gobject)"; fi
done
if { pkg-config --exists freetype2 2>/dev/null || [ -f /usr/include/freetype2/ft2build.h ]; } && header x11 X11/Xlib.h && header alsa alsa/asoundlib.h; then
  have "freetype, X11, ALSA (headers)"
else
  lack "freetype, X11, ALSA (headers)" "optional: Arkos Tracker 3 songs need them (freetype2 libx11 alsa-lib | libfreetype-dev libx11-dev libasound2-dev | freetype-devel libX11-devel alsa-lib-devel); without them it is left out"
  export SKIP_AT3=1
fi
if command -v flac >/dev/null || command -v ffmpeg >/dev/null; then have "flac or ffmpeg (export)"
else lack "flac or ffmpeg" "optional: FLAC export needs one of them; WAV works without"; fi

say "Replayers"
declare -A RESULT=(
  [build-uade.sh]="$OPT/uade/lib/libuade.so"
  [build-zxtune.sh]="$OPT/zxtune/bin/bbmp-zxtune"
  [build-vgm.sh]="$OPT/bbmp-vgm/bin/bbmp-vgm"
  [build-chip.sh]="$OPT/bbmp-chip/bin/bbmp-chip"
  [build-psgplay.sh]="$OPT/bbmp-psgplay/bin/bbmp-psgplay"
  [build-snsf.sh]="$OPT/bbmp-snsf/bin/bbmp-snsf"
  [build-furnace.sh]="$OPT/furnace/bin/bbmp-furnace"
  [install-sunvox.sh]="$OPT/sunvox/sunvox.so"
  [build-xmp.sh]="$OPT/bbmp-xmp/bin/bbmp-xmp"
  [build-retrovert.sh]="$OPT/bbmp-rv/bin/bbmp-rv"
  [build-musicline.sh]="$OPT/bbmp-musicline/bin/bbmp-musicline"
  [build-fac.sh]="$OPT/bbmp-fac/bin/bbmp-fac"
  [build-psycle.sh]="$OPT/bbmp-psycle/bin/bbmp-psycle"
  [build-iigs.sh]="$OPT/bbmp-iigs/bin/bbmp-iigs"
  [build-nez.sh]="$OPT/bbmp-nez/bin/bbmp-nez"
  [build-console.sh]="$OPT/bbmp-console/bin/bbmp-console"
  [build-misc.sh]="$OPT/bbmp-misc/bin/bbmp-misc"
  [build-pc.sh]="$OPT/bbmp-pc/bin/bbmp-pc"
  [build-synth.sh]="$OPT/bbmp-synth/bin/bbmp-synth"
  [build-amiga.sh]="$OPT/bbmp-amiga/bin/bbmp-amiga"
  [build-retro.sh]="$OPT/bbmp-retro/bin/bbmp-retro"
  [build-ay.sh]="$OPT/bbmp-ay/bin/bbmp-ay"
  [build-adlib.sh]="$OPT/bbmp-adlib/bin/bbmp-adlib"
  [build-darkwave.sh]="$OPT/bbmp-darkwave/bin/bbmp-darkwave"
  [build-pcmusic.sh]="$OPT/bbmp-pcmusic/bin/bbmp-pcmusic"
  [build-piece.sh]="$OPT/bbmp-piece/bin/bbmp-piece"
  [build-chipnsfx.sh]="$OPT/bbmp-chipnsfx/bin/bbmp-chipnsfx"
  [build-tiatracker.sh]="$OPT/bbmp-tiatracker/bin/bbmp-tiatracker"
  [build-synder.sh]="$OPT/bbmp-synder/bin/bbmp-synder"
  [build-t0ast.sh]="$OPT/bbmp-t0ast/bin/bbmp-t0ast"
  [build-berotracker.sh]="$OPT/bbmp-berotracker/bin/bbmp-berotracker"
  [build-axs.sh]="$OPT/bbmp-axs/bin/bbmp-axs"
  [build-shut.sh]="$OPT/bbmp-shut/bin/bbmp-shut"
)
ORDER=(build-uade.sh build-zxtune.sh build-vgm.sh build-chip.sh build-psgplay.sh build-snsf.sh build-furnace.sh install-sunvox.sh
       build-xmp.sh build-retrovert.sh build-musicline.sh build-fac.sh build-psycle.sh
       build-iigs.sh build-nez.sh build-console.sh build-misc.sh build-pc.sh
       build-synth.sh build-amiga.sh build-retro.sh
       build-ay.sh build-adlib.sh build-darkwave.sh build-pcmusic.sh build-piece.sh
       build-chipnsfx.sh build-tiatracker.sh build-synder.sh build-t0ast.sh build-berotracker.sh build-axs.sh build-shut.sh)
for script in "${ORDER[@]}"; do
  [ -f "$HERE/tools/$script" ] || continue      # not in this copy of the source
  if [ -e "${RESULT[$script]}" ]; then have "$script"; else lack "$script" "not built yet"; fi
done

if [ "$MODE" = "--check" ]; then
  [ ${#missing_system[@]} -eq 0 ] || printf '\nInstall first: %s\n' "${missing_system[*]}"
  exit 0
fi
if [ ${#missing_system[@]} -gt 0 ]; then
  printf '\nInstall these with your package manager, then run this again:\n  %s\n' "${missing_system[*]}"
  exit 1
fi

for script in "${ORDER[@]}"; do
  [ -f "$HERE/tools/$script" ] || continue
  if [ -e "${RESULT[$script]}" ] && [ "$MODE" != "--rebuild" ]; then
    continue
  fi
  say "Running tools/$script (the log is in /tmp/bbmp-setup-$script.log)"
  if "$HERE/tools/$script" > "/tmp/bbmp-setup-$script.log" 2>&1 && [ -e "${RESULT[$script]}" ]; then
    echo "  done"
  else
    echo "  FAILED; the formats it plays will show as unplayable"
    tail -5 "/tmp/bbmp-setup-$script.log" | sed 's/^/    /'
    failed+=("$script")
  fi
done

say "Catalogs"
CATALOGS="${XDG_DATA_HOME:-$HOME/.local/share}/bbmp/catalogs"
if compgen -G "$CATALOGS/*.db" >/dev/null && [ "$MODE" != "--rebuild" ]; then
  echo "  already there, in $CATALOGS (bbmp update updates them)"
else
  # the collections chosen (bbmp collections --use changes them), made where missing
  "$HERE/bbmp" collections --offline | sed 's/^/  /'
  if [ "$MODE" = "--rebuild" ]; then "$HERE/bbmp" update | sed 's/^/  /'; fi
fi

say "Extra files"
echo "  Some formats use files the player can't get by itself (ROMs, sample sets)."
"$HERE/bbmp" extras | grep -E '^(there|NOT THERE|Extra files)' | sed 's/^/  /'
echo "  'bbmp extras' says what each one is."

say "Result"
info="$("$HERE/bbmp" info)"
printf '%s\n' "$info" | sed 's/^/  /'
if printf '%s\n' "$info" | grep -q '^Sandbox: off'; then
  printf '\nWARNING: no sandbox works here, so the replayers run with your own rights:\n'
  printf 'a harmful song file could reach your files and the network.\n'
  printf 'Install bubblewrap (bwrap) and check that it can start (some systems turn off\n'
  printf 'user namespaces), or build bbmp-shut (tools/build-shut.sh) on a kernel with Landlock.\n'
fi
if [ ${#failed[@]} -gt 0 ]; then
  printf '\nNot everything was built: %s\n' "${failed[*]}"
  exit 1
fi
printf '\nReady. Start the player with:  %s/bbmp\n' "$HERE"
