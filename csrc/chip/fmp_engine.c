/* SPDX-License-Identifier: GPL-3.0-or-later */
/* 98fmplayer behind a tiny C interface: its headers are C-only and its PMD
 * driver's names clash with pmdmini's, so bbmp-chip.cpp only sees these. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/fmplayer_common.h"
#include "common/fmplayer_file.h"
#include "fmdriver/fmdriver.h"
#include "fmdriver/ppz8.h"
#include "libopna/opna.h"
#include "libopna/opnadrum.h"
#include "libopna/opnatimer.h"

#include "fmp_engine.h"

static struct fmdriver_work work;
static struct ppz8 ppz8;
static struct opna opna;
static struct opna_timer timer;
static uint8_t adpcm_ram[OPNA_ADPCM_RAM_SIZE];
static struct fmplayer_file *file;

const char *fmpw_open(const char *path, int *is_fmp) {
  enum fmplayer_file_error err;
  file = fmplayer_file_alloc(path, &err);
  if (!file) return fmplayer_file_strerror(err);
  fmplayer_init_work_opna(&work, &ppz8, &opna, &timer, adpcm_ram);
  /* 98fmplayer looks for the chip's drums in its own folder. The player says where
   * the user has put them. */
  const char *rom_path = getenv("BBMP_YM2608_ROM");
  static uint8_t drum_rom[OPNA_ROM_SIZE];
  FILE *rom = rom_path ? fopen(rom_path, "rb") : 0;
  if (rom) {
    if (fread(drum_rom, 1, sizeof drum_rom, rom) == sizeof drum_rom)
      opna_drum_set_rom(&opna.drum, drum_rom);
    fclose(rom);
  }
  fmplayer_file_load(&work, file, 1);
  *is_fmp = file->type == FMPLAYER_FILE_TYPE_FMP;
  return 0;
}

const char *fmpw_comment(int line) {
  return work.get_comment ? work.get_comment(&work, line) : 0;
}

void fmpw_render(int16_t *buf, unsigned frames) {
  memset(buf, 0, frames * 4);
  opna_timer_mix(&timer, buf, frames);
}

int fmpw_playing(void) { return work.playing; }
int fmpw_loops(void) { return work.loop_cnt; }

/* Decode a YM2608 rhythm ROM dump into the six WAV files pmdmini looks for
 * (2608_BD.WAV ... 2608_RIM.WAV, mono, 16 bit, at the chip's own rate). */
int fmpw_write_rhythm(const char *rom_path, const char *dest_dir) {
  static const char *names[6] = {"BD", "SD", "TOP", "HH", "TOM", "RIM"};
  static struct opna_drum drum;
  static uint8_t rom[OPNA_ROM_SIZE];
  FILE *f = fopen(rom_path, "rb");
  if (!f) return -1;
  size_t got = fread(rom, 1, sizeof rom, f);
  int extra = fgetc(f);
  fclose(f);
  if (got != sizeof rom || extra != EOF) return -2;   /* not an 8192-byte dump */
  opna_drum_set_rom(&drum, rom);
  for (int i = 0; i < 6; i++) {
    char path[4096];
    snprintf(path, sizeof path, "%s/2608_%s.WAV", dest_dir, names[i]);
    FILE *out = fopen(path, "wb");
    if (!out) return -3;
    uint32_t bytes = drum.drums[i].len * 2, rate = 55467;
    uint32_t riff = 36 + bytes, fmt = 16, byterate = rate * 2;
    uint16_t pcm = 1, mono = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, out); fwrite(&riff, 4, 1, out);
    fwrite("WAVEfmt ", 1, 8, out); fwrite(&fmt, 4, 1, out);
    fwrite(&pcm, 2, 1, out); fwrite(&mono, 2, 1, out);
    fwrite(&rate, 4, 1, out); fwrite(&byterate, 4, 1, out);
    fwrite(&align, 2, 1, out); fwrite(&bits, 2, 1, out);
    fwrite("data", 1, 4, out); fwrite(&bytes, 4, 1, out);
    fwrite(drum.drums[i].data, 2, drum.drums[i].len, out);
    if (fclose(out) != 0) return -3;
  }
  return 0;
}

void fmpw_close(void) {
  if (file) fmplayer_file_free(file);
  file = 0;
}
