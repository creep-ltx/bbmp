// SPDX-License-Identifier: GPL-2.0-or-later
// bbmp-vgm: takes the place of libvgm's emu/cores/fmopn_2608rom.h (tools/build-vgm.sh points
// fmopn.c here). The YM2608's rhythm ROM, its six drum sounds, is Yamaha's: it is not built
// in. The user puts a dump of it in the player's extras folder (ym2608_adpcm_rom.bin, as the
// PMD player reads it) and the player names it in BBMP_YM2608_ROM. Without it fmopn.c finds
// no ROM mapped, and the drums are silent.
#ifndef BBMP_YM2608_ROM_H
#define BBMP_YM2608_ROM_H
#ifdef __cplusplus
extern "C" {
#endif
// the ROM's 8192 bytes, or NULL when there is none
const unsigned char* bbmp_ym2608_rom(void);
// 1 when a YM2608 asked for the ROM and there was none
int bbmp_ym2608_rom_missing(void);
#ifdef __cplusplus
}
#endif
#define YM2608_ADPCM_ROM bbmp_ym2608_rom()
#endif
