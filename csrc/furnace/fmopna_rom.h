/* SPDX-License-Identifier: GPL-2.0-or-later */
/* bbmp: in place of YM2608-LLE's fmopna_rom.h, whose built-in copy of the YM2608's rhythm
 * ROM is not used: the ROM is the user's file (bbmp_ym2608_rom.h), its nibbles swapped into
 * the order this core reads. (The core's own copy was read off the chip and differs from the
 * usual dump in 4 of its 8192 bytes.) */
#include "bbmp_ym2608_rom.h"
#define rss_rom (bbmp_ym2608_rom(1))
