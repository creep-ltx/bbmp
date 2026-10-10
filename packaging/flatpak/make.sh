#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Make bbmp.flatpak: build every replayer, helper and library inside the Flatpak SDK
# (org.freedesktop.Sdk 26.08), for any x86-64, from bbmp-sources, and bundle it into one
# file. Nothing is downloaded while it builds and nothing comes from this machine.
#
#   packaging/flatpak/make.sh             the package
#
#   BBMP_SOURCES  the copy of bbmp-sources to build from, an archive of the exact source
#                 tarballs the build uses (default: bbmp-sources beside this
#                 repository's folder)
#   WORK          where flatpak-builder builds and keeps what it built (default
#                 ~/.cache/bbmp-flatpak); a module that built is not built again
#
# Uses the flatpak tools from build-tools.sh when flatpak isn't installed. The whole
# build takes about an hour at four compilers at once (JOBS in manifest.py).
set -euo pipefail
cd "$(dirname "$0")"
if ! command -v flatpak-builder >/dev/null; then
  export PATH="$HOME/.local/opt/flatpak/bin:$PATH"
  export LD_LIBRARY_PATH="$HOME/.local/opt/flatpak/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
SOURCES="${BBMP_SOURCES:-$(cd ../../.. && pwd)/bbmp-sources}"
WORK="${WORK:-$HOME/.cache/bbmp-flatpak}"
[ -f "$SOURCES/sha256sums.txt" ] || {
  echo "no bbmp-sources in $SOURCES (git clone https://github.com/creep-ltx/bbmp-sources, then BBMP_SOURCES=...)" >&2
  exit 1; }
ln -sfn "$(cd "$SOURCES" && pwd)" sources      # the manifest's archives are sources/archives/...
MANIFEST=io.github.bytebandit.bbmp.yml
BUNDLE=bbmp.flatpak
python3 manifest.py
mkdir -p "$WORK"
# at most four compilers at once (flatpak-builder would start one per core for the
# libraries' make; the build scripts take JOBS=4 from the manifest)
flatpak-builder --user --force-clean --jobs=4 --repo="$WORK/repo" --state-dir="$WORK/state" \
  "$WORK/build" "$MANIFEST"
flatpak build-bundle "$WORK/repo" "$BUNDLE" io.github.bytebandit.bbmp \
  --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo
ls -l "$BUNDLE"
