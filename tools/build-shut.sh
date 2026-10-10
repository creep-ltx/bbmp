#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-shut, which starts a replayer shut in by Landlock where bubblewrap can't
# start (inside a Flatpak; sandbox.py). The result goes to ~/.local/opt/bbmp-shut,
# which the Flatpak builds with the replayers (packaging/flatpak).
set -euo pipefail
. "$(dirname "$0")/sources.sh"
PREFIX="${1:-$BBMP_OPT/bbmp-shut}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$PREFIX/bin"
cc -O2 -std=gnu17 -Wall -Wextra -Werror -o "$PREFIX/bin/bbmp-shut" "$HERE/csrc/shut/bbmp-shut.c"
echo "built $PREFIX/bin/bbmp-shut (Landlock ABI here: $("$PREFIX/bin/bbmp-shut" --abi))"
