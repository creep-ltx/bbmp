/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Capcom QSound (QSF) through Highly Quixotic and psflib, behind a tiny C interface. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "psflib.h"
#include "qsound.h"

#include "qsf_engine.h"

struct state {
  uint8_t *key, *z80, *smp;
  uint32_t key_size, z80_size, smp_size;
  long length_ms, fade_ms;
  char title[256], artist[256], game[256], year[64], copyright[256], ripper[128], comment[1024];
};

static struct state st;
static void *emu;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* "[[h:]m:]s[.fraction]" -> milliseconds, -1 if it isn't a time */
static long parse_time(const char *s) {
  double total = 0, part = 0;
  int seen = 0;
  for (const char *p = s; *p; p++) {
    if (*p == ':') { total = (total + part) * 60; part = 0; }
    else if ((*p >= '0' && *p <= '9') || *p == '.' || *p == ',') {
      part = strtod(p, (char **)&p); p--; seen = 1;
    } else if (*p != ' ') return -1;
  }
  return seen ? (long)((total + part) * 1000) : -1;
}

static int on_tag(void *ctx, const char *name, const char *value) {
  (void)ctx;
  if (!strcasecmp(name, "length")) st.length_ms = parse_time(value);
  else if (!strcasecmp(name, "fade")) st.fade_ms = parse_time(value);
  else if (!strcasecmp(name, "title")) snprintf(st.title, sizeof st.title, "%s", value);
  else if (!strcasecmp(name, "artist")) snprintf(st.artist, sizeof st.artist, "%s", value);
  else if (!strcasecmp(name, "game")) snprintf(st.game, sizeof st.game, "%s", value);
  else if (!strcasecmp(name, "year")) snprintf(st.year, sizeof st.year, "%s", value);
  else if (!strcasecmp(name, "copyright")) snprintf(st.copyright, sizeof st.copyright, "%s", value);
  else if (!strcasecmp(name, "qsfby")) snprintf(st.ripper, sizeof st.ripper, "%s", value);
  else if (!strcasecmp(name, "comment")) snprintf(st.comment, sizeof st.comment, "%s", value);
  return 0;
}

static int upload(const char *section, uint32_t start, const uint8_t *data, uint32_t size) {
  uint8_t **array;
  /* A section's place comes from the file: a Z80 program or sample ROM is never
   * near 32 MiB (the largest QSound sample ROMs are 8), and room up to where one says
   * it goes is made and cleared, 2 GiB before this limit. */
  uint32_t *array_size, max_size = 0x2000000;
  if (!strcmp(section, "KEY")) { array = &st.key; array_size = &st.key_size; max_size = 11; }
  else if (!strcmp(section, "Z80")) { array = &st.z80; array_size = &st.z80_size; }
  else if (!strcmp(section, "SMP")) { array = &st.smp; array_size = &st.smp_size; }
  else return -1;
  if (start + size < start || start + size > max_size) return -1;
  if (start + size > *array_size) {
    *array = realloc(*array, start + size);
    memset(*array + *array_size, 0, start + size - *array_size);
    *array_size = start + size;
  }
  memcpy(*array + start, data, size);
  return 0;
}

static int on_exe(void *ctx, const uint8_t *exe, size_t exe_size, const uint8_t *res, size_t res_size) {
  (void)ctx; (void)res; (void)res_size;
  while (exe_size >= 11) {
    char s[4] = {exe[0], exe[1], exe[2], 0};
    uint32_t ofs = le32(exe + 3), size = le32(exe + 7);
    exe += 11; exe_size -= 11;
    if (size > exe_size || upload(s, ofs, exe, size) < 0) return -1;
    exe += size; exe_size -= size;
  }
  return 0;
}

static void *f_open(const char *uri) { return fopen(uri, "rb"); }
static size_t f_read(void *buf, size_t size, size_t n, void *h) { return fread(buf, size, n, h); }
static int f_seek(void *h, int64_t ofs, int whence) { return fseek(h, (long)ofs, whence); }
static int f_close(void *h) { return fclose(h); }
static long f_tell(void *h) { return ftell(h); }
static const psf_file_callbacks files = {"/", f_open, f_read, f_seek, f_close, f_tell};

const char *qsfw_open(const char *path, struct qsfw_info *info) {
  static int initialised;
  if (!initialised && qsound_init() == 0) initialised = 1;
  memset(&st, 0, sizeof st);
  st.length_ms = st.fade_ms = -1;
  if (psf_load(path, &files, 0x41, on_exe, 0, on_tag, 0, 0) <= 0) return "not a QSF file";
  emu = malloc(qsound_get_state_size());
  qsound_clear_state(emu);
  if (st.key_size == 11)
    qsound_set_kabuki_key(emu, be32(st.key), be32(st.key + 4), st.key[8] << 8 | st.key[9], st.key[10]);
  else
    qsound_set_kabuki_key(emu, 0, 0, 0, 0);
  qsound_set_z80_rom(emu, st.z80, st.z80_size);
  qsound_set_sample_rom(emu, st.smp, st.smp_size);
  info->game = st.game;
  info->year = st.year;
  info->copyright = st.copyright;
  info->ripper = st.ripper;
  info->comment = st.comment;
  info->title = st.title[0] ? st.title : st.game;
  info->author = st.artist;
  info->length_ms = st.length_ms;
  info->fade_ms = st.fade_ms;
  return 0;
}

int qsfw_render(int16_t *buf, unsigned frames) {
  uint32_t n = frames;
  if (qsound_execute(emu, 0x7fffffff, buf, &n) < 0) return -1;
  return (int)n;
}

void qsfw_close(void) {
  free(emu); free(st.key); free(st.z80); free(st.smp);
  emu = 0;
}
