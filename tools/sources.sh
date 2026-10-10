# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck shell=bash
# Read by every tools/build-*.sh: where the sources come from and where things go.
#
#   BBMP_SOURCES  a copy of bbmp-sources (an archive of the exact source tarballs the
#                 build uses). When it is set, sources are unpacked from it, each checked
#                 against its sha256sums.txt, and nothing is downloaded: the build needs no
#                 internet. Unset, the scripts fetch from the internet as they always did.
#   BBMP_SRC      where sources are unpacked and built (default ~/.local/src)
#   BBMP_OPT      where the replayers are installed (default ~/.local/opt). A script's first
#                 argument still names its own folder; this is where it finds the others
#                 (UADE, AdPlug, sc68, Game Music Emu).
#   JOBS          compilers at once (default 4)
#   BBMP_PUBLIC   1: a package build. A script that leaves an engine out of it writes
#                 it into <its prefix>/left-out.txt, which the player reads to say "not
#                 in this package" for those formats.
# macOS: the scripts are written for GNU's tools (sed -i, sed -E with \1, find, tar), so
# Homebrew's GNU versions go first; Homebrew's headers and libraries are found; and the
# C library has no iconv of its own (ICONV is added where it is used). SO is what a
# shared library's name ends in.
SO=.so
ICONV=
OLD_C=
if [ "$(uname -s)" = Darwin ]; then
  BREW="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"
  # bison too: macOS's is 2.3, rgbds needs 3
  [ -d "$BREW/opt/bison/bin" ] && PATH="$BREW/opt/bison/bin:$PATH"
  for tool in gnu-sed coreutils findutils grep gnu-tar make; do
    [ -d "$BREW/opt/$tool/libexec/gnubin" ] && PATH="$BREW/opt/$tool/libexec/gnubin:$PATH"
  done
  export PATH
  export CPATH="$BREW/include${CPATH:+:$CPATH}"
  export LIBRARY_PATH="$BREW/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
  # libsidplayfp and libresidfp as build-sidplayfp.sh made them (Homebrew's lacks reSIDfp)
  SIDFP="${BBMP_OPT:-$HOME/.local/opt}/sidplayfp"
  export PKG_CONFIG_PATH="$SIDFP/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
  export CPATH="$SIDFP/include:$CPATH" LIBRARY_PATH="$SIDFP/lib:$LIBRARY_PATH"
  SO=.dylib
  ICONV=-liconv
  # old C that GCC warns about and Apple's clang refuses: warnings there too ($OLD_C)
  OLD_C="-Wno-incompatible-function-pointer-types -Wno-int-conversion -Wno-implicit-function-declaration -Wno-implicit-int"
fi

# a script that stops on an error says where (set -e alone stops without a word)
set -E
trap 'rc=$?; echo "$(basename "$0"): stopped at line $LINENO: $BASH_COMMAND (exit $rc)" >&2' ERR

BBMP_SRC="${BBMP_SRC:-$HOME/.local/src}"
BBMP_OPT="${BBMP_OPT:-$HOME/.local/opt}"
JOBS="${JOBS:-4}"
BBMP_PUBLIC="${BBMP_PUBLIC:-0}"
mkdir -p "$BBMP_SRC"

# sha256_check: checks "<sum>  <file>" lines on stdin, quietly; Linux has sha256sum, macOS
# shasum
sha256_check() {
  if command -v sha256sum >/dev/null; then sha256sum -c --quiet -; else shasum -a 256 -c --quiet; fi
}

# one_object [--groups] <out.o> <name pattern>... -- <objects>...: the objects linked
# into one, in which only the names that match a pattern (gt1w_*) can be seen from
# outside; the rest become local, so two engines' globals can't clash. GNU: ld -r, then
# objcopy; --groups takes the groups of sections apart first (or what two engines share
# of the C++ library would be dropped from the second). macOS: ld -r keeps only the
# exported names (C names begin with _ there) and makes the rest local itself.
one_object() {
  local groups=()
  [ "$1" = --groups ] && { groups=(--force-group-allocation); shift; }
  local out="$1"; shift
  local keep=()
  while [ "$1" != "--" ]; do keep+=("$1"); shift; done
  shift
  if [ "$(uname -s)" = Darwin ]; then
    local args=()
    for k in "${keep[@]}"; do args+=(-exported_symbol "_$k"); done
    ld -r -o "$out" "${args[@]}" "$@"
  else
    local args=()
    for k in "${keep[@]}"; do args+=(-G "$k"); done
    ld -r "${groups[@]}" -o "$out.all.o" "$@"
    objcopy --wildcard "${args[@]}" "$out.all.o" "$out"
    rm -f "$out.all.o"
  fi
}

# kept: true when the sources come from bbmp-sources
kept() { [ -n "${BBMP_SOURCES:-}" ]; }

# kept_file <archive>: the path of one of bbmp-sources' archives, checked against its
# sha256sums.txt (an archive split into parts is put together first)
kept_file() {
  local f="$BBMP_SOURCES/archives/$1" sum
  if [ ! -f "$f" ] && compgen -G "$f.part-*" >/dev/null; then
    mkdir -p "$BBMP_SRC/.joined"
    cat "$f".part-* > "$BBMP_SRC/.joined/$1"
    sum="$(awk -v n="archives/$1" '$2 == n {print $1}' "$BBMP_SOURCES/sha256sums-joined.txt")"
    f="$BBMP_SRC/.joined/$1"
  else
    sum="$(awk -v n="archives/$1" '$2 == n {print $1}' "$BBMP_SOURCES/sha256sums.txt")"
  fi
  [ -n "$sum" ] || { echo "$1 is not in bbmp-sources ($BBMP_SOURCES)" >&2; return 1; }
  echo "$sum  $f" | sha256_check >&2 \
    || { echo "$f is not the file bbmp-sources lists" >&2; return 1; }
  printf '%s\n' "$f"
}

# kept_copy <archive> <file>: one of bbmp-sources' files, copied as <file>
kept_copy() {
  local f
  f="$(kept_file "$1")" || exit 1
  mkdir -p "$(dirname "$2")"
  cp "$f" "$2"
}

# kept_unpack <archive> <folder>: the archive's one top folder becomes <folder>. Done once:
# <folder>/.bbmp-sources names the archive it came from. A folder that is a git clone is
# not replaced (set BBMP_SRC to an empty folder).
kept_unpack() {
  local f top tmp
  f="$(kept_file "$1")" || exit 1
  [ "$(cat "$2/.bbmp-sources" 2>/dev/null)" = "$1" ] && return 0
  if [ -e "$2/.git" ]; then
    echo "$2 is a git clone; set BBMP_SRC to another folder to build from bbmp-sources" >&2
    exit 1
  fi
  tmp="$2.unpacking"
  rm -rf "$tmp" "$2"
  mkdir -p "$tmp"
  case "$1" in
    *.zip) unzip -q "$f" -d "$tmp" ;;
    *)     tar -xf "$f" -C "$tmp" ;;
  esac
  top="$(ls -A "$tmp")"
  if [ "$(printf '%s\n' "$top" | wc -l)" = 1 ] && [ -d "$tmp/$top" ]; then
    mv "$tmp/$top" "$2"
    rmdir "$tmp"
  else
    mv "$tmp" "$2"            # an archive without one top folder
  fi
  echo "$1" > "$2/.bbmp-sources"
}

# left_out <prefix> <engine> <why>: an engine this build leaves out (BBMP_PUBLIC=1)
left_out() {
  mkdir -p "$1"
  printf '%s\t%s\n' "$2" "$3" >> "$1/left-out.txt"
}

# git_pin <url> <folder> <commit> [nosub]: <folder> as a git checkout of exactly <commit>
# (the full hash: the one bbmp-sources keeps and the player was tested with), never the
# newest commit upstream, with its submodules at the commits it names (nosub: none). A
# checkout already at <commit> is left as it is, with the changes the build made to it.
git_pin() {
  [ "$(git -C "$2" rev-parse HEAD 2>/dev/null)" = "$3" ] && return 0
  if [ ! -d "$2" ]; then
    git init -q "$2"
    git -C "$2" remote add origin "$1"
  elif [ ! -e "$2/.git" ]; then
    echo "$2 is not a git checkout; set BBMP_SRC to another folder to build from the internet" >&2
    exit 1
  fi
  git -C "$2" fetch -q --depth 1 origin "$3" || git -C "$2" fetch -q origin
  git -C "$2" checkout -q -f --detach "$3"
  if [ "${4:-}" != nosub ] && [ -f "$2/.gitmodules" ]; then
    git -C "$2" submodule update -q --init --recursive --force
  fi
  [ "$(git -C "$2" rev-parse HEAD)" = "$3" ] || { echo "$2 is not at $3" >&2; exit 1; }
}

# apply_patches <folder> <patch>...: each patch (-p1, from <folder>) applied once; one
# already applied is left alone, one that applies neither way stops the build
apply_patches() {
  local dir="$1" p
  shift
  for p in "$@"; do
    [ -f "$p" ] || continue
    if ! patch -R -p1 -s -f --dry-run -d "$dir" < "$p" >/dev/null 2>&1; then
      patch -p1 -s -f -d "$dir" < "$p" || { echo "$(basename "$p") does not apply in $dir" >&2; exit 1; }
    fi
  done
}
