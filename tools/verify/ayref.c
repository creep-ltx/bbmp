// SPDX-License-Identifier: GPL-3.0-or-later
// Run an original Z80 player routine on an emulated Z80 and write what it writes to the
// AY chip, frame by frame: the reference a replayer is checked against (tools/verify).
//
//   ayref <player.bin> <player-at> <song> <song-at> <init> <play> <frames> [poke=addr:value ...]
//         [pt3-sample1] [pt3] [table=pc,pc:from:to]
//
// Numbers may be hex (0x...). The machine is 64 KB of RAM and nothing else; the player
// is called at <init> with HL = <song-at>, then at <play> once a frame. Out: 14 bytes a
// frame, registers 0 to 13 as they are after the frame's call, register 13 0xff in a
// frame that didn't write it: the layout of `bbmp-zxtune --aydump`.
//
// pt3-sample1: Vortex Tracker II's PT3 player gives a channel that plays a note before
// any sample is chosen an empty, silent sample; the original Pro Tracker 3 has no
// default sample at all, and Vortex Tracker's editor, Ay_Emul and ZXTune use sample 1.
// With this the player's channels start with sample 1 too, so that such songs can be
// compared in everything else. The three places are found in the player's own init:
// after LD HL,<empty sample> come seven LD (nn),HL, the last three the channels' sample
// pointers. When the song has no sample 1, the player's empty one stays.
//
// pt3: the song is a PT3 module; say (stderr, a line a frame) in which frames the player
// read a sample's or an ornament's line past its end, which a song's sample or ornament offset
// past the end makes it do (it plays whatever bytes follow for that frame).
#include "z80.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t mem[65536];
static uint8_t reg[16];
static int selected;
static int wrote13;

// AYREF_TRACE=<frame>: which bytes of the song the player reads in that frame (stderr)
static long trace_frame = -1, frame_now = -2, song_from, song_to;

static long past_end = -1;     // the first frame that read past the song's end
static long past_object = -1;  // pt3-sample1: the first frame that read past a sample's
                               // or an ornament's end (a song that starts one past it)
static int pt3_layout;
// table=pc,pc:from:to: the instructions at those addresses read a table that is at
// from..to; say (stderr, a line a frame) in which frames one read outside it (a note
// past the table's end, which the original player doesn't check)
static long table_pc[4], table_pcs, table_from, table_to, outside_table = -1;
static long outside_now = -1, past_object_now = -1;     // the last frame it happened in
static uint16_t cur_pc;
static long last_object = -1, last_object_end;

// a PT3 song's samples (32) and ornaments (16): where each starts and ends
static int object_at(uint16_t a, long* start, long* end)
{
  for (int k = 0; k < 48; k++) {
    const long at = song_from + (k < 32 ? 105 + 2 * k : 169 + 2 * (k - 32));
    const long off = mem[at] | mem[at + 1] << 8;
    if (!off) continue;
    const long st = song_from + off;
    if (a == st || a == st + 1) {
      const long len = mem[(uint16_t)(st + 1)];
      *start = st;
      *end = st + 2 + (k < 32 ? 4 * len : len);
      return 1;
    }
  }
  return 0;
}

static uint8_t rd(void* u, uint16_t a)
{
  (void)u;
  if (past_end < 0 && frame_now >= 0 && a >= song_to && a < song_to + 0x400 && a < 0xc000)
    past_end = frame_now;
  if (table_pcs && frame_now >= 0 && outside_now != frame_now && (a < table_from || a >= table_to))
    for (long k = 0; k < table_pcs; k++)
      if (cur_pc == table_pc[k]) {
        if (outside_table < 0) outside_table = frame_now;
        outside_now = frame_now;
        fprintf(stderr, "outside the table: %ld\n", frame_now);
      }
  if (pt3_layout && frame_now >= 0 && a >= song_from && a < song_to) {
    long st, en;
    if (object_at(a, &st, &en)) {
      last_object = st;
      last_object_end = en;
    } else if (last_object >= 0 && a >= last_object_end && a < last_object + 2 + 4 * 256
               && past_object_now != frame_now) {
      if (past_object < 0) past_object = frame_now;
      past_object_now = frame_now;
      fprintf(stderr, "past an object's end: %ld\n", frame_now);
    }
  }
  if (frame_now == trace_frame && a >= song_from && a < song_to)
    fprintf(stderr, "read %04lx = %02x\n", (long)(a - song_from), mem[a]);
  return mem[a];
}
static void wr(void* u, uint16_t a, uint8_t v) { (void)u; mem[a] = v; }
static uint8_t in(z80* z, uint8_t port) { (void)z; (void)port; return 0xff; }

// The Spectrum's AY: port FFFD selects a register, BFFD writes it (the high byte of the
// port is B for OUT (C),A, which is how players write it).
static void out(z80* z, uint8_t port, uint8_t v)
{
  if ((port & 0x02) != 0) return;
  if ((z->b & 0xc0) == 0xc0) selected = v & 0x0f;
  else if ((z->b & 0xc0) == 0x80) {
    reg[selected] = v;
    if (selected == 13) wrote13 = 1;
  }
}

static long num(const char* s) { return strtol(s, NULL, 0); }

static int pt3_sample1(long player_at, long player_len, long song_at)
{
  for (long i = player_at; i + 3 < player_at + player_len; i++) {
    if (mem[i] != 0x01 || mem[i + 1] != 0x00 || mem[i + 2] != 0x90) continue;
    const uint16_t empty = (uint16_t)(i - 1);
    for (long j = player_at; j + 3 + 7 * 3 < player_at + player_len; j++) {
      if (mem[j] != 0x21 || mem[j + 1] != (empty & 0xff) || mem[j + 2] != (empty >> 8)) continue;
      const uint16_t s1off = (uint16_t)(mem[song_at + 107] | mem[song_at + 108] << 8);
      if (s1off == 0) return 0;     // no sample 1: the player's own empty one stays
      const uint16_t s1 = (uint16_t)(song_at + s1off);
      for (int k = 4; k < 7; k++) {
        const long at = j + 3 + k * 3;
        if (mem[at] != 0x22) return -1;
        const uint16_t field = (uint16_t)(mem[at + 1] | mem[at + 2] << 8);
        mem[field] = s1 & 0xff;
        mem[(uint16_t)(field + 1)] = s1 >> 8;
      }
      return 0;
    }
  }
  return -1;
}

static long loaded;

static int load(const char* path, long at)
{
  FILE* f = fopen(path, "rb");
  if (!f) { perror(path); return -1; }
  size_t n = fread(mem + at, 1, sizeof mem - at, f);
  fclose(f);
  loaded = (long)n;
  return (int)n;
}

// Call a routine and run until it returns to the sentinel; 0 when it did in time.
static int call(z80* z, uint16_t addr, uint16_t hl)
{
  const uint16_t sentinel = 0x0008;
  z->sp = 0x3ffe;
  mem[0x3ffe] = sentinel & 0xff;
  mem[0x3fff] = sentinel >> 8;
  z->pc = addr;
  z->h = hl >> 8;
  z->l = hl & 0xff;
  z->halted = false;
  for (long steps = 0; steps < 5000000; steps++) {
    if (z->pc == sentinel) return 0;
    cur_pc = z->pc;
    z80_step(z);
  }
  return 1;
}

int main(int argc, char** argv)
{
  if (argc < 8) {
    fprintf(stderr, "usage: %s player.bin player-at song song-at init play frames [poke=addr:value ...]\n", argv[0]);
    return 2;
  }
  if (load(argv[1], num(argv[2])) < 0) return 1;
  const long player_len = loaded;
  if (load(argv[3], num(argv[4])) < 0) return 1;
  song_from = num(argv[4]);
  song_to = song_from + loaded;
  if (getenv("AYREF_TRACE")) trace_frame = num(getenv("AYREF_TRACE"));
  int sample1 = 0;
  for (int i = 8; i < argc; i++) {
    if (strncmp(argv[i], "poke=", 5) == 0) {
      char* colon = strchr(argv[i], ':');
      if (colon) mem[num(argv[i] + 5) & 0xffff] = (uint8_t)num(colon + 1);
    } else if (strcmp(argv[i], "pt3-sample1") == 0) {
      sample1 = 1;
    } else if (strcmp(argv[i], "pt3") == 0) {
      pt3_layout = 1;
    } else if (strncmp(argv[i], "table=", 6) == 0) {
      char* p = argv[i] + 6;
      while (table_pcs < 4) {
        table_pc[table_pcs++] = strtol(p, &p, 0);
        if (*p != ',') break;
        p++;
      }
      if (*p == ':') table_from = strtol(p + 1, &p, 0);
      if (*p == ':') table_to = strtol(p + 1, &p, 0);
    }
  }
  z80 z;
  z80_init(&z);
  z.read_byte = rd;
  z.write_byte = wr;
  z.port_in = in;
  z.port_out = out;
  if (call(&z, (uint16_t)num(argv[5]), (uint16_t)num(argv[4]))) {
    fprintf(stderr, "the player's init didn't return\n");
    return 1;
  }
  if (sample1 && pt3_sample1(num(argv[2]), player_len, num(argv[4]))) {
    fprintf(stderr, "pt3-sample1: the player's init is not as expected\n");
    return 1;
  }
  long frames = num(argv[7]);
  for (long f = 0; f < frames; f++) {
    wrote13 = 0;
    frame_now = f;
    if (call(&z, (uint16_t)num(argv[6]), 0)) {
      fprintf(stderr, "the player didn't return in frame %ld\n", f);
      return 1;
    }
    uint8_t row[14];
    memcpy(row, reg, 14);
    if (!wrote13) row[13] = 0xff;
    fwrite(row, 1, 14, stdout);
  }
  if (past_end >= 0) fprintf(stderr, "read past the song's end in frame %ld\n", past_end);
  if (past_object >= 0) fprintf(stderr, "read past an object's end in frame %ld\n", past_object);
  if (outside_table >= 0) fprintf(stderr, "read outside the table in frame %ld\n", outside_table);
  return 0;
}
