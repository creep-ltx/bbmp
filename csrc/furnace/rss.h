/* SPDX-License-Identifier: GPL-2.0-or-later */
/* bbmp: in place of Furnace's src/engine/platform/sound/rss.h, whose built-in copy of the
 * YM2608's rhythm ROM is not used: the ROM is the user's file (bbmp_ym2608_rom.h). */
#include "bbmp_ym2608_rom.h"
#define YM2608_ADPCM_ROM (bbmp_ym2608_rom(0))
