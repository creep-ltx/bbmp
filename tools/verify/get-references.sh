#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetch and build what tools/verify checks the replayers against, into
# ~/.cache/bbmp-work/verify (nothing of it is shipped with the player):
#
#   vt/            Vortex Tracker II's source (S.V. Bulba; free to use, crediting him):
#                  its PT3 player, assembled for C000 (ZXAYHOBETA/C000.bin)
#   bulba/         S.V. Bulba's PTxTools and PT2Docs from his site, as the web archive
#                  keeps it: his PT2 player (PTxTools/PT2Play/PT2Play, for C000), and
#                  PT2.TXT, a magazine article (VfNG/NEW) with the listing of Pro
#                  Tracker 2.101's own player
#   pt2std/        that listing assembled for C000 (pt2std.bin, pt2std.lst)
#   sjasmplus/     the assembler (z00m128/sjasmplus), built without Lua
#   bin/ayref      the Z80 runner (ayref.c, on superzazu's z80 in ~/.local/src)
#   bin/sidwrites  libsidplayfp's SID changes (sidwrites.cpp)
#   ~/.local/opt/vice  VICE (vsid), the Arch package unpacked, not installed: the C64
#                  reference (sid.py runs it in a headless cage)
set -euo pipefail
W="$HOME/.cache/bbmp-work/verify"
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$W/bin" "$W/bulba" "$W/pt2std"
cd "$W"

[ -d vt ] || git clone -q --depth 1 https://github.com/rio-rattenrudel/VT.git vt

for f in PTxTools.7z PT2Docs.rar; do
  [ -s "bulba/$f" ] || curl -sL -m 120 -o "bulba/$f" "http://web.archive.org/web/2024id_/https://bulba.untergrund.net/$f"
done
[ -d bulba/PTxTools ] || 7z x -y -obulba/PTxTools bulba/PTxTools.7z >/dev/null
[ -d bulba/PT2Docs ] || { mkdir -p bulba/PT2Docs; unrar x -o+ -inul bulba/PT2Docs.rar bulba/PT2Docs/; }

if [ ! -x sjasmplus/sjasmplus ]; then
  [ -d sjasmplus ] || git clone -q --depth 1 https://github.com/z00m128/sjasmplus.git
  make -C sjasmplus -j"${JOBS:-4}" -s USE_LUA=0 >/dev/null
fi

# the article is in CP866 with CR line ends; the player's listing starts at "LD HL,MUSIC"
iconv -f cp866 -t utf-8 bulba/PT2Docs/PT2.TXT | tr '\r' '\n' | awk 'length > 0' > pt2std/article.txt
start=$(grep -an "HL,MUSIC" pt2std/article.txt | head -1 | cut -d: -f1)
{ echo '        DEVICE NONE'; echo '        ORG #C000'; echo 'MUSIC   EQU #4000'
  tail -n +"$((start - 2))" pt2std/article.txt; } > pt2std/pt2std.asm
sjasmplus/sjasmplus --nologo --raw=pt2std/pt2std.bin --lst=pt2std/pt2std.lst pt2std/pt2std.asm >/dev/null

gcc -O2 -std=gnu17 -Wall -I"$HOME/.local/src/superzazu-z80" "$HERE/ayref.c" \
  "$HOME/.local/src/superzazu-z80/z80.c" -o bin/ayref
g++ -O2 -std=c++17 "$HERE/sidwrites.cpp" -lsidplayfp -o bin/sidwrites

if [ ! -x "$HOME/.local/opt/vice/usr/bin/vsid" ]; then
  mkdir -p "$HOME/.local/opt/vice"
  tmp="$(mktemp -d)"
  curl -sL -o "$tmp/vice.pkg.tar.zst" "$(pacman -Sp vice | tail -1)"
  tar -C "$HOME/.local/opt/vice" -xf "$tmp/vice.pkg.tar.zst" usr
  rm -rf "$tmp"
fi
echo "ready: $W"
