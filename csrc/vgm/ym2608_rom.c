// SPDX-License-Identifier: GPL-2.0-or-later
// bbmp-vgm: the YM2608's rhythm ROM, read from the file BBMP_YM2608_ROM names (ym2608_rom.h).
#include <stdio.h>
#include <stdlib.h>

#include "ym2608_rom.h"

static unsigned char rom[0x2000];
static int state;           // 0 not read yet, 1 read, -1 not there

const unsigned char* bbmp_ym2608_rom(void)
{
    if (state == 0) {
        state = -1;
        const char* path = getenv("BBMP_YM2608_ROM");
        FILE* f = path && *path ? fopen(path, "rb") : NULL;
        if (f) {
            // exactly 8192 bytes, or it is not the ROM
            if (fread(rom, 1, sizeof rom, f) == sizeof rom && fgetc(f) == EOF)
                state = 1;
            fclose(f);
        }
    }
    return state == 1 ? rom : NULL;
}

int bbmp_ym2608_rom_missing(void)
{
    return state == -1;
}
