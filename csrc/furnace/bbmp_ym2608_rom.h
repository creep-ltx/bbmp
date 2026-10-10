/* SPDX-License-Identifier: GPL-2.0-or-later */
/* bbmp: the YM2608's rhythm ROM (its six drum sounds) for Furnace's two YM2608 cores, read
 * from the file the user puts in the player's extras folder (ym2608_adpcm_rom.bin, named in
 * BBMP_YM2608_ROM), as the PMD player reads it. It is Yamaha's, so it is not built in.
 * tools/build-furnace.sh puts this beside sound/rss.h (ymfm's core) and
 * extern/YM2608-LLE/fmopna_rom.h (the LLE core), whose copies of the ROM are replaced.
 * swapped=1 gives it with the two halves of each byte swapped: the order the LLE core reads.
 * Without the file both get bytes that decode to silence (ADPCM-A steps of zero, up and
 * down in turn), so the drums are silent. */
#ifndef BBMP_YM2608_ROM_H
#define BBMP_YM2608_ROM_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char* bbmp_ym2608_rom(int swapped)
{
  static unsigned char rom[2][0x2000];
  static int done = 0;
  if (!done) {
    const char* path = getenv("BBMP_YM2608_ROM");
    FILE* f = path && *path ? fopen(path, "rb") : NULL;
    int ok = 0;
    done = 1;
    if (f) {
      ok = fread(rom[0], 1, 0x2000, f) == 0x2000 && fgetc(f) == EOF;  /* exactly 8192 bytes */
      fclose(f);
    }
    if (!ok) memset(rom[0], 0x80, 0x2000);
    for (int i = 0; i < 0x2000; i++) rom[1][i] = (unsigned char)((rom[0][i] << 4) | (rom[0][i] >> 4));
  }
  return rom[swapped ? 1 : 0];
}
#endif
