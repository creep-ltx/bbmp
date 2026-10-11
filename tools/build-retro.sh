#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build bbmp-retro: trackers and drivers of 8-bit computers, consoles and
# handhelds in one helper binary, one engine per format (csrc/retro/*_engine.cpp).
#
#   sccmusixx     SCC-Musixx (MSX, Konami SCC)   smx_engine.cpp, written for this player in a clean room, on emu2212
#   shroom        Shroom (Mario Paint's songs)   own replayer + the game's samples (*)
#   ned           NerdTracker 2 (NES)            the tracker's NES replay routine + libgme
#   picatune      Picatune (not Picatune2)       smufi_engine.cpp, written for this player in a clean room
#   cybertracker  Cybertracker C64               the tracker itself in a small C64 + reSIDfp
#   mgb           Gameboy Tracker                gbt_engine.cpp, written for this player in a clean room + the SoundSystem driver + libgme
#   onyx          Onyx Music File                jangle_engine.cpp, written for this player in a clean room: made into a module for libopenmpt
#   wwsnd         Westwood SND (PC speaker)      own replayer
#   tss           TSS (T'SoundSystem)            own port of the JavaScript version
#   victracker    VIC-TRACKER (VIC-20)           the tracker's player on a 6502 + own VIC sound
#   vgemm         VGM Music Maker (Mega Drive)   own replayer after the editor's VGM export + libgme
#
# From the system: libgme, libopenmpt, libresidfp (all three are there for the player).
#
# Sources go to ~/.local/src, the result to ~/.local/opt/bbmp-retro. Everything
# that is downloaded is pinned: a commit, or a file with its checksum.
#
# (*) The instruments of Mario Paint are recordings of the game's own sounds, which
#     are Nintendo's. They are not fetched: you put them into the player's
#     extras folder (see `bbmp extras`). The player doesn't use the piece engine
#     (docs/replayers.md).
set -euo pipefail
. "$(dirname "$0")/sources.sh"
SRC="$BBMP_SRC"
PREFIX="${1:-$BBMP_OPT/bbmp-retro}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
RETRO="$HERE/csrc/retro"
OBJ="$SRC/modland-retro-build"
mkdir -p "$SRC" "$PREFIX/bin" "$PREFIX/share" "$OBJ"
cd "$SRC"

# A repository at one commit. A full clone, so that the commit is there for certain.
pinned() {   # <github repo> <folder> <commit>
  if kept; then kept_unpack "$2-${3:0:7}.tar.xz" "$2"; return; fi
  [ -d "$2/.git" ] || git clone "https://github.com/$1.git" "$2"
  git -C "$2" cat-file -e "$3^{commit}" 2>/dev/null || git -C "$2" fetch -q origin
  git -C "$2" reset -q --hard "$3"
}
# A file with its checksum.
download() {   # <url> <file> <sha256>
  if kept && [ ! -f "$2" ]; then kept_copy "$(basename "$2")" "$2"; fi
  [ -f "$2" ] && echo "$3  $2" | sha256_check >/dev/null 2>&1 || curl -sL --retry 3 -o "$2" "$1"
  # a site may give some machines something else (nesdev.org did to GitHub's Macs): the
  # Internet Archive's copy, checked the same way
  echo "$3  $2" | sha256_check >/dev/null 2>&1 \
    || curl -sfL --retry 3 -o "$2" "https://web.archive.org/web/2026id_/$1" || true
  echo "$3  $2" | sha256_check
}

OBJECTS=()
LIBS=()

# --- sccmusixx: the SCC, emu2212 by Mitsutaka Okazaki (MIT) ---------------------
pinned digital-sound-antiques/emu2212 emu2212 ee35dbe099f7242253e9b5b2a93847161a355c7b
gcc -O2 -std=gnu17 -c emu2212/emu2212.c -o "$OBJ/emu2212.o"
OBJECTS+=("$OBJ/emu2212.o")
INCLUDES=(-Iemu2212)

# --- ned: NerdTracker II's replay routine for the NES (replay 22 by Bananmos, the
# ca65 port by Memblers), assembled with cc65's ca65 and ld65 into a blob that the
# engine puts into an NSF image together with the song ---------------------------
pinned cc65/cc65 cc65 555282497c3ecf8b313d87d5973093af19c35bd5   # V2.19
[ -x cc65/bin/ld65 ] || make -C cc65 -j"$JOBS" bin CC="gcc -std=gnu17" > cc65.log 2>&1 \
  || { echo "cc65 didn't build; the end of cc65.log:" >&2; tail -20 cc65.log >&2; exit 1; }
mkdir -p nedplay
download https://www.nesdev.org/nt2/download/nedplay.zip nedplay/nedplay.zip \
  84602a2164280c3a10b080c21fd43f3cac7b6e41e6a53e94faf859c596b4e76f
unzip -o -q nedplay/nedplay.zip nt2-22.asm DPCMCODE.INC nedplay.txt -d nedplay
B="$SRC/nedplay/build"
rm -rf "$B" && mkdir -p "$B"
cp nedplay/nt2-22.asm nedplay/DPCMCODE.INC "$B/"
# The song, the table of DPCM notes and the samples are put in place by the engine
# when it plays a file; here they are left out. The replay routine wants the DPCM
# table right behind its code, so the code comes last.
: > "$B/TEMP.DTT"
: > "$B/temp.dmc"
for i in $(seq 1 17); do echo "ned$i = \$FFFF"; done > "$B/songs.asm"
cat > "$B/blob.cfg" <<'CFG'
MEMORY {
    ZP:  start = $00,   size = $100,  type = rw;
    RAM: start = $200,  size = $600,  type = rw;
    HDR: start = $7F80, size = $80,   type = ro, file = "";
    PRG: start = $8000, size = $4000, type = ro, file = %O;
    SMP: start = $C080, size = $3F80, type = ro, file = "";
}
SEGMENTS {
    NSFHEADER: load = HDR, type = ro;
    RODATA:    load = PRG, type = ro;
    CODE:      load = PRG, type = ro;
    SAMPLES:   load = SMP, type = ro;
    DATA:      load = RAM, type = bss;
    ZEROPAGE:  load = ZP,  type = zp;
    BSS:       load = RAM, type = bss;
}
CFG
(cd "$B" && "$SRC/cc65/bin/ca65" -g nt2-22.asm -o nt2-22.o \
  && "$SRC/cc65/bin/ld65" -C blob.cfg -Ln nt2replay.lbl -o nt2replay.bin nt2-22.o)
install -m644 "$B/nt2replay.bin" "$B/nt2replay.lbl" "$PREFIX/share/"
LIBS+=(-lgme)

# --- shroom: the instruments of Mario Paint are recordings of the game's sounds and
# Nintendo's, so they are not fetched. Who has notes.wav of Mariopants 1.1 puts it
# into the player's extras folder; this tool makes of it what the engine reads. -----
g++ -O2 -std=gnu++17 -o "$PREFIX/bin/shroom-samples" "$RETRO/shroom_samples.cpp"
rm -f "$PREFIX/share/shroom-notes.bin"

# --- cybertracker: CyberTracker 1.01 (CyberBrain of No Name, 2001) is the only player
# of its songs. Its disk image is not fetched here: the player offers to download it
# (Downloads in Settings, `bbmp download cybertracker`). -------------------------
rm -f "$PREFIX/share/cybertracker101.d64"
LIBS+=(-lresidfp)

# --- mgb: the replay routine of Gameboy Tracker, SoundSystem (MIT), assembled with
# rgbds 0.6.1 (MIT) into mgb_driver.h -------------------------------------------------
if [ ! -x rgbds-0.6.1/rgblink ]; then
  download https://github.com/gbdev/rgbds/releases/download/v0.6.1/rgbds-0.6.1.tar.gz rgbds-0.6.1.tar.gz \
    872018509c6cbc2d9e947a1a6d509fd9dc56a010827f608c5f4fa46de2fa186a
  mkdir -p rgbds-0.6.1
  tar xzf rgbds-0.6.1.tar.gz -C rgbds-0.6.1 --strip-components=1
  make -C rgbds-0.6.1 -j"$JOBS" rgbasm rgblink CFLAGS="-O2 -std=gnu17" Q= > rgbds.log 2>&1 \
    || { echo "rgbds didn't build; the end of rgbds.log:" >&2; tail -20 rgbds.log >&2; exit 1; }
fi
pinned gbdev/GBSoundSystem GBSoundSystem c0c6a6f32c1f703be979efc54c4891c275189ab1
rm -rf "$OBJ/mgb" && mkdir -p "$OBJ/mgb"
cp "$RETRO/mgb_SoundSystem.def" "$OBJ/mgb/SoundSystem.def"
( cd "$OBJ/mgb"
  "$SRC/rgbds-0.6.1/rgbasm" -i "$SRC/GBSoundSystem/Driver/" -o driver.o "$SRC/GBSoundSystem/Driver/SoundSystem.asm" > /dev/null
  "$SRC/rgbds-0.6.1/rgbasm" -o entry.o "$RETRO/mgb_gbs.asm"
  "$SRC/rgbds-0.6.1/rgblink" -l "$RETRO/mgb_gbs.link" -o driver.gb -n driver.sym driver.o entry.o
  python3 - <<'PY'
d = open('driver.gb', 'rb').read()
assert len(d) == 0x4000 and d[0x400] == 0xc3 and d[0x403] == 0xc3
blob = d[0x400:].rstrip(b'\x00')
with open('mgb_driver.h', 'w') as f:
    f.write('// generated: the SoundSystem driver and the GBS entry, from $0400\n')
    f.write('static const unsigned char mgb_driver[] = {\n')
    for i in range(0, len(blob), 16):
        f.write(' ' + ''.join(' 0x%02x,' % b for b in blob[i:i + 16]) + '\n')
    f.write('};\n')
PY
)
INCLUDES+=(-I"$OBJ/mgb")

# --- onyx: libopenmpt plays the module that the engine makes of the song ------------
LIBS+=(-lopenmpt)

# --- victracker: VIC-TRACKER 2.0's own player (musiced/player.asm, BSD licence, Daniel
# Kahlin), assembled with dasm (GPL 2) into vt_player.h, with the song memory at $3300
# where the tracker keeps it ------------------------------------------------------------
pinned dasm-assembler/dasm dasm 09270863f5f70e5200c30d2778f654ff7a40adba   # 2.20.14.1
[ -x dasm/src/dasm ] || make -C dasm/src -j"$JOBS" CFLAGS="-O2 -std=gnu17" > dasm.log 2>&1
download http://www.kahlin.net/daniel/victracker/vt-2.0/victracker-2.0.tar.gz victracker-2.0.tar.gz \
  d06c726fbde199cf00d66d1945317446cbf3f43c9e5e928ad5bd501d7c75da85
rm -rf "$OBJ/vt" && mkdir -p "$OBJ/vt"
tar xzf victracker-2.0.tar.gz -C "$OBJ/vt" --strip-components=2 \
  victracker-2.0/musiced/player.asm victracker-2.0/musiced/playerdata.asm
cat > "$OBJ/vt/vt_player.asm" <<'ASM'
	processor 6502
VERSION_MAJOR	EQU	2
VERSION_MINOR	EQU	0
	include	"playerdata.asm"
	seg	code
	org	$6000
	include	"player.asm"
ASM
( cd "$OBJ/vt"
  "$SRC/dasm/src/dasm" vt_player.asm -f3 -ovt_player.bin -svt_player.sym > dasm.out || { cat dasm.out; exit 1; }
  python3 - <<'PY'
blob = open('vt_player.bin', 'rb').read()
syms = {}
for line in open('vt_player.sym'):
    parts = line.split()
    if len(parts) >= 2 and parts[0].startswith('pl_'):
        syms[parts[0]] = int(parts[1], 16)
with open('vt_player.h', 'w') as f:
    f.write('// generated from VIC-TRACKER 2.0 musiced/player.asm (BSD licence)\n')
    f.write('#define VT_PLAYER_ORG 0x6000\n')
    for name in ('pl_Init', 'pl_Play', 'pl_ThisSong', 'pl_Step', 'pl_PatternPos', 'pl_ThisEndStep'):
        f.write('#define VT_%s 0x%04x\n' % (name, syms[name]))
    f.write('static const unsigned char vt_player[] = {\n')
    for i in range(0, len(blob), 16):
        f.write(' ' + ''.join(' 0x%02x,' % b for b in blob[i:i + 16]) + '\n')
    f.write('};\n')
PY
)
INCLUDES+=(-I"$OBJ/vt")

# --- the helper -----------------------------------------------------------------
ENGINES=()
for f in "$RETRO"/*_engine.cpp; do
  name="$(basename "${f%_engine.cpp}")"
  # BBMP_RETRO_ENGINES="ned mgb" builds only those (for trying things out)
  case " ${BBMP_RETRO_ENGINES:-$name} " in *" $name "*) ENGINES+=("$f");; esac
done
# -ffp-contract=off: SCC-Musixx's resampling is checked bit for bit; no fused multiply-add
g++ -O2 -ffp-contract=off -std=gnu++17 -o "$PREFIX/bin/bbmp-retro" -I"$RETRO" "${INCLUDES[@]}" \
  "$RETRO/bbmp-retro.cpp" "${ENGINES[@]}" "${OBJECTS[@]}" "${LIBS[@]}" -lm $ICONV
# TSS's BSD licence asks that its notice go with the program
mkdir -p "$PREFIX/share/licenses"
install -m644 "$RETRO/LICENSE.tss" "$PREFIX/share/licenses/"
echo "installed $PREFIX/bin/bbmp-retro"
