#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build libsidplayfp with reSIDfp (libresidfp), as the Flatpak does, into
# ~/.local/opt/sidplayfp. For macOS: Homebrew's libsidplayfp comes without reSIDfp, the
# emulation the player uses. (Linux takes the system's.)
set -euo pipefail
. "$(dirname "$0")/sources.sh"
PREFIX="${1:-$BBMP_OPT/sidplayfp}"
mkdir -p "$PREFIX"
get() {   # <name> <version> <sha256>: unpacked into $BBMP_SRC/<name>-<version>
  local tgz="$BBMP_SRC/$1-$2.tar.gz"
  if kept; then kept_copy "$1-$2.tar.gz" "$tgz"; fi
  [ -f "$tgz" ] || curl -sfL -o "$tgz" \
    "https://github.com/libsidplayfp/$1/releases/download/v$2/$1-$2.tar.gz"
  echo "$3  $tgz" | sha256_check || { echo "$tgz is not $1 $2" >&2; exit 1; }
  rm -rf "$BBMP_SRC/$1-$2"
  tar -xzf "$tgz" -C "$BBMP_SRC"
}
get libresidfp 1.2.2 2eb0204b76d52fe825c0e1f98826f62a179033a60543032d0e8b90060ecaa50e
get libsidplayfp 3.1.1 12b79190593bf480b2d11481b5c2de62bac07f344437a66cd8d887329875c626
(cd "$BBMP_SRC/libresidfp-1.2.2" && ./configure --prefix="$PREFIX" --disable-static >/dev/null \
  && make -j"$JOBS" >/dev/null && make install >/dev/null)
(cd "$BBMP_SRC/libsidplayfp-3.1.1" \
  && PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" ./configure --prefix="$PREFIX" --disable-static \
       --without-exsid --without-usbsid >/dev/null \
  && make -j"$JOBS" >/dev/null && make install >/dev/null)
[ -f "$PREFIX/include/sidplayfp/builders/residfp.h" ] || { echo "built without reSIDfp" >&2; exit 1; }
echo "installed libsidplayfp with reSIDfp in $PREFIX"
