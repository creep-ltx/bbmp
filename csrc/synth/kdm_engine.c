/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ken's Digital Music (.kdm), Ken Silverman's song format of the 1990s, played by a
 * replayer written for this player from a description of how the format and its
 * player behave (a clean-room rewrite: no code of Ken's was read to write it).
 *
 * A song holds notes only: when, which track, which pitch, the volume left and right,
 * and three effect numbers (frequency, volume, panning). The instruments of every song
 * are 8-bit samples in one file, waves.kwv, which must be next to the song. It is
 * Ken's and not shipped; the player fetches it from Modland with the song.
 *
 * There is one tick every 1/120 s. Sixteen voices step through their waves in 1/4096
 * sample steps, read with a coarse (16-step) linear interpolation, and are summed
 * and clipped. At each tick's start the first R/2048 frames are crossfaded from what
 * the old voices would have played, so that a note starting or stopping doesn't click.
 *
 * By default this plays the way Ken's original programs sounded: three effect tables
 * (a vibrato, a slow and a faster sine) and the crossfade curve are made with a sine.
 * The C version that other players use computes that sine so that it always comes out
 * 0: no vibrato, no sines, a flat 50 % crossfade. BBMP_KDM_FLAT=1 plays like that,
 * which is how this replayer was checked sample for sample against that version. */

#include "engine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  kRate = 48000,
  kVoices = 16,
  kWaves = 256,          /* a track's wave number is a byte */
  kMaxNotes = 8192,
  kMaxTracks = 256,
  kTables = 17,          /* effect numbers 1..16; 0 is none */
  kMaxLook = 256,
};

typedef struct {
  long long start, length, loop_start, loop_length;  /* in samples, into g.mem */
  int finetune;
  char name[17];
} Wave;

typedef struct {
  int wave;
  long long pos;        /* 1/4096 samples, counts up toward 0 from the end point; < 0 plays */
  long long end;        /* end point in g.mem */
  long long base_inc, inc;
  int base_l, base_r, gain_l, gain_r;
  int fx[3], count[3];  /* frequency, volume, panning */
} Voice;

static struct {
  int open;
  int rate, tick_frames, look;
  /* the song */
  int notes, tracks;
  int track_wave[kMaxTracks];
  int* time;
  unsigned char *track, *pitch, *vol_l, *vol_r, *fx_f, *fx_v, *fx_p;
  long long ticks, tick;
  int next;             /* next note to handle */
  long long counter;
  /* the bank */
  Wave waves[kWaves];
  unsigned char* mem;   /* all samples, then two bytes of 128 */
  long long mem_size;
  /* playing */
  long long note_inc[256];
  int table[kTables][256];
  int weight[kMaxLook];
  Voice voice[kVoices];
  long long* acc;       /* tick_frames * 2 */
  long long prev[kMaxLook * 2], ahead[kMaxLook * 2];
  float* buf;           /* the tick, ready */
  int buf_pos, buf_len;
} g;

static int32_t le32(const uint8_t* p) {
  return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

static int fail(synth_info* info, const char* why) {
  snprintf(info->error, sizeof(info->error), "%s", why);
  return 0;
}

static void release(void) {
  free(g.time); free(g.track); free(g.mem); free(g.acc); free(g.buf);
  memset(&g, 0, sizeof(g));
}

/* round(16384 sin(a pi / 1024)) */
static int sine(int a) {
  return (int)lround(16384.0 * sin((double)a * M_PI / 1024.0));
}

static void make_tables(int rate, int flat) {
  int i, e;
  long long* t = g.note_inc;
  /* the note table: a semitone is 2^(1/12) in 2.30 fixed point, truncated step by step */
  memset(t, 0, sizeof(g.note_inc));
  t[1] = (long long)(11025LL * 2093 / rate) * 8192;
  for (i = 2; i <= 75; ++i) t[i] = (t[i - 1] * 1137589835LL) >> 30;
  t[0] = t[12] >> 1;
  /* entries 255 down to 242 are notes -1 to -14, each the one 12 above halved; the
   * order matters, as 243 to 242 are made from 255 to 254 */
  for (i = 255; i >= 242; --i) t[i] = t[(i + 12) & 255] >> 1;

  for (e = 0; e < kTables; ++e)
    for (i = 0; i < 256; ++i) {
      int v = 0;
      switch (e) {
        case 1: v = flat ? 65536 : 65536 + sine(90 * i) / 9; break;
        case 2: v = 58386 + i * 7150 / 30; if (v > 65536) v = 65536; break;
        case 3: v = 69433 + i * -3897 / 30; if (v < 65536) v = 65536; break;
        case 4: v = flat ? 65536 : 65536 + 4 * sine(2048 * i / 120); break;
        case 5: v = flat ? 65536 : 65536 + sine(2048 * i / 30); break;
        case 6: { static const int chord[3] = {65536, 65536 * 330 / 262, 65536 * 392 / 262};
                  v = chord[(i / 2) % 3]; break; }
        case 7: v = i * 65536 / 120; if (v > 65536) v = 65536; break;
        case 8: v = 65536 - i * 65536 / 120; if (v < 0) v = 0; break;
        default: v = 0;  /* 0 is no table; 9..16 are undefined and 0 */
      }
      g.table[e][i] = v;
    }

  for (i = 0; i < g.look; ++i)
    g.weight[i] = flat ? 32768 : 2 * (16384 - sine(1536 - 1024 * i / g.look));
}

/* ---- loading ---- */

static FILE* open_bank(const char* song) {
  static const char* names[] = {"waves.kwv", "WAVES.KWV", "Waves.kwv", "waves.KWV"};
  const char* slash = strrchr(song, '/');
  const size_t dir = slash ? (size_t)(slash - song + 1) : 0;
  char path[4096];
  size_t i;
  if (dir + 16 > sizeof(path)) return NULL;
  for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    FILE* f;
    memcpy(path, song, dir);
    strcpy(path + dir, names[i]);
    if ((f = fopen(path, "rb"))) return f;
  }
  return NULL;
}

/* 1 when the bank is whole */
static int load_bank(FILE* f) {
  long long size, need, total = 0;
  int n, i;
  unsigned char head[8];
  unsigned char* hdr;
  if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 8 || fseek(f, 0, SEEK_SET) != 0) return 0;
  if (fread(head, 1, 8, f) != 8) return 0;
  n = le32(head + 4);
  if (le32(head) != 0 || n < 1 || n > kWaves) return 0;
  need = 8 + 32LL * n;
  if (size < need) return 0;
  hdr = malloc((size_t)n * 32);
  if (!hdr) return 0;
  if (fread(hdr, 1, (size_t)n * 32, f) != (size_t)n * 32) { free(hdr); return 0; }
  for (i = 0; i < n; ++i) {
    const unsigned char* h = hdr + 32 * i;
    Wave* w = &g.waves[i];
    const long long len = le32(h + 16), ls = le32(h + 20), ll = le32(h + 24), ft = le32(h + 28);
    if (len < 0 || len > size - need - total) { free(hdr); return 0; }
    /* an empty slot may have any loop (the real bank's have a loop of 2) */
    if (len > 0 && (ls < 0 || ll < 0 || ls + ll > len)) { free(hdr); return 0; }
    /* 748 is an octave; a pitch at or below -748 would step backwards */
    if (ft <= -748 || ft > 65536) { free(hdr); return 0; }
    memcpy(w->name, h, 16);
    w->name[16] = 0;
    {
      size_t k = strlen(w->name);
      while (k && w->name[k - 1] == ' ') w->name[--k] = 0;
    }
    w->start = total;
    w->length = len;
    w->loop_start = len > 0 ? ls : 0;
    w->loop_length = len > 0 ? ll : 0;
    w->finetune = (int)ft;
    total += len;
  }
  free(hdr);
  /* waves past n stay empty: length 0, no loop, finetune 0 (start at the end) */
  for (i = n; i < kWaves; ++i) g.waves[i].start = total;
  g.mem = malloc((size_t)total + 2);
  if (!g.mem) return 0;
  if (total && fread(g.mem, 1, (size_t)total, f) != (size_t)total) return 0;
  g.mem[total] = g.mem[total + 1] = 128;
  g.mem_size = total + 2;
  return 1;
}

int synthw_kdm_open(const char* path, const uint8_t* data, size_t size, int track, synth_info* info) {
  long long m, t, tables, i, last;
  const uint8_t* p;
  FILE* bank;
  const char* flat_env = getenv("BBMP_KDM_FLAT");
  const int flat = flat_env && flat_env[0] && strcmp(flat_env, "0") != 0;

  release();
  if (size < 12 || le32(data) != 0) return fail(info, "not a KDM song");
  m = le32(data + 4);
  t = le32(data + 8);
  if (m < 1 || m > kMaxNotes || t < 1 || t > kMaxTracks) return fail(info, "not a KDM song");
  if (size == (size_t)(12 + 4 * t + 11 * m)) tables = 7;
  else if (size == (size_t)(12 + 4 * t + 10 * m)) tables = 6;   /* older: no panning effects */
  else return fail(info, "not a KDM song");
  if (track != 0) return fail(info, "the song has one track");

  g.notes = (int)m;
  g.tracks = (int)t;
  g.time = malloc(sizeof(int) * (size_t)m);
  g.track = calloc(7, (size_t)m);
  if (!g.time || !g.track) { release(); return fail(info, "the song can't be loaded"); }
  g.pitch = g.track + m; g.vol_l = g.pitch + m; g.vol_r = g.vol_l + m;
  g.fx_f = g.vol_r + m; g.fx_v = g.fx_f + m; g.fx_p = g.fx_v + m;
  for (i = 0; i < t; ++i) g.track_wave[i] = data[12 + i];
  /* then per track: quantize and two default volumes, which only the editor uses */
  p = data + 12 + 4 * t;
  for (i = 0; i < m; ++i) g.time[i] = le32(p + 4 * i);
  p += 4 * m;
  memcpy(g.track, p, (size_t)(m * tables));   /* the panning effects stay 0 when absent */
  last = g.time[0];
  for (i = 0; i < m; ++i) {
    if (g.track[i] >= t || g.fx_f[i] > 16 || g.fx_v[i] > 16 || g.fx_p[i] > 16) {
      release();
      return fail(info, "the song can't be loaded");
    }
    if (g.time[i] > last) last = g.time[i];
  }
  /* The song plays from its first note's time until all notes are handled, which is
   * at the latest time; it starts again in that tick, which isn't played. */
  g.ticks = last - (long long)g.time[0];
  if (g.ticks < 1) { release(); return fail(info, "the song can't be loaded"); }

  bank = open_bank(path);
  if (!bank) { release(); return fail(info, "waves.kwv, which has the instruments, is not next to the song"); }
  if (!load_bank(bank)) {
    fclose(bank);
    release();
    return fail(info, "waves.kwv, which has the instruments, is damaged");
  }
  fclose(bank);

  g.rate = kRate;
  g.tick_frames = kRate / 120;
  g.look = kRate >> 11;
  if (g.look > kMaxLook) g.look = kMaxLook;
  make_tables(kRate, flat);
  g.acc = malloc(sizeof(long long) * 2 * (size_t)g.tick_frames);
  g.buf = malloc(sizeof(float) * 2 * (size_t)g.tick_frames);
  if (!g.acc || !g.buf) { release(); return fail(info, "the song can't be loaded"); }
  g.counter = g.time[0];
  g.open = 1;

  info->rate = kRate;
  info->tracks = 1;
  info->duration_ms = (long)(g.ticks * 1000 / 120);
  snprintf(info->type, sizeof(info->type), "Ken's Digital Music");
  for (i = 0; i < t && info->n_names < SYNTH_MAX_NAMES; ++i) {
    const char* name = g.waves[g.track_wave[i]].name;
    int k, seen = 0;
    if (!name[0]) continue;
    for (k = 0; k < info->n_names && !seen; ++k) seen = strcmp(info->names[k], name) == 0;
    if (!seen) snprintf(info->names[info->n_names++], sizeof(info->names[0]), "%s", name);
  }
  return 1;
}

/* ---- playing ---- */

static long long increment(int wave, int note) {
  return (g.note_inc[note] * (g.waves[wave].finetune + 748)) >> 24;
}

static void handle_note(int n) {
  const int wave = g.track_wave[g.track[n]];
  const long long inc = increment(wave, g.pitch[n]);
  int v;
  if (g.vol_l[n] == 0) {
    /* note off: every playing voice of this wave at this pitch, whichever track started it */
    for (v = 0; v < kVoices; ++v)
      if (g.voice[v].pos < 0 && g.voice[v].wave == wave && g.voice[v].base_inc == inc)
        g.voice[v].pos = 0;
    return;
  }
  {
    /* the voice nearest its end point, or past it; voice 0 wins a tie, then the highest */
    int best = 0;
    Voice* x;
    const Wave* w = &g.waves[wave];
    for (v = kVoices - 1; v >= 1; --v)
      if (g.voice[v].pos > g.voice[best].pos) best = v;
    x = &g.voice[best];
    x->wave = wave;
    x->end = w->start + w->length;
    x->pos = -(w->length * 4096);
    x->base_inc = x->inc = inc;
    x->base_l = x->gain_l = g.vol_l[n];
    x->base_r = x->gain_r = g.vol_r[n];
    x->fx[0] = g.fx_f[n]; x->fx[1] = g.fx_v[n]; x->fx[2] = g.fx_p[n];
    x->count[0] = x->count[1] = x->count[2] = 0;
  }
}

static void apply_effects(Voice* x) {
  const int f = x->fx[0] && x->count[0] < 256;
  const int vo = x->fx[1] && x->count[1] < 256;
  const int pa = x->fx[2] && x->count[2] < 256;
  if (f) x->inc = (x->base_inc * g.table[x->fx[0]][x->count[0]++]) >> 16;
  else x->inc = x->base_inc;
  if (vo || pa) {
    long long l = x->base_l, r = x->base_r;
    if (vo) {
      const long long k = g.table[x->fx[1]][x->count[1]++];
      l = (l * k) >> 16;
      r = (r * k) >> 16;
    }
    if (pa) {
      const long long k = g.table[x->fx[2]][x->count[2]++];
      l = (l * (131072 - k)) >> 16;
      r = (r * k) >> 16;
    }
    x->gain_l = (int)l;
    x->gain_r = (int)r;
  }
}

/* Mixes up to `frames` frames of a voice into out (stereo), moving pos and end. */
static void mix(long long* pos_io, long long* end_io, const Voice* x, long long* out, int frames) {
  const Wave* w = &g.waves[x->wave];
  long long pos = *pos_io, end = *end_io;
  const long long inc = x->inc;
  const long long gl = x->gain_l, gr = x->gain_r;
  int i;
  for (i = 0; i < frames && pos < 0; ++i) {
    const long long at = end + (pos >> 12);
    const int s0 = g.mem[at], s1 = g.mem[at + 1];
    const int s = s0 + (((s1 - s0) * (int)((pos >> 8) & 15) + 8) >> 4);
    out[2 * i] += (s - 128) * gl;
    out[2 * i + 1] += (s - 128) * gr;
    pos += inc;
    if (pos >= 0 && w->loop_length > 0) {
      const long long span = w->loop_length * 4096;
      end = w->start + w->loop_start + w->loop_length;
      pos -= span;
      /* once is all a real song needs; a step longer than the loop would read past it */
      if (pos >= 0) pos = pos % span - span;
    }
  }
  *pos_io = pos;
  *end_io = end;
}

static void render_tick(void) {
  const int n = g.tick_frames, L = g.look;
  int v, i;
  memset(g.acc, 0, sizeof(long long) * 2 * (size_t)n);
  for (v = 0; v < kVoices; ++v) {
    Voice* x = &g.voice[v];
    if (x->pos >= 0) continue;
    apply_effects(x);
    mix(&x->pos, &x->end, x, g.acc, n);
  }
  /* the crossfade from what the voices of the last tick would have played on */
  for (i = 0; i < L && i < n; ++i) {
    g.acc[2 * i] += ((g.prev[2 * i] - g.acc[2 * i]) * g.weight[i]) >> 16;
    g.acc[2 * i + 1] += ((g.prev[2 * i + 1] - g.acc[2 * i + 1]) * g.weight[i]) >> 16;
  }
  memset(g.ahead, 0, sizeof(g.ahead));
  for (v = 0; v < kVoices; ++v) {
    const Voice* x = &g.voice[v];
    long long pos = x->pos, end = x->end;
    if (pos < 0) mix(&pos, &end, x, g.ahead, L);
  }
  memcpy(g.prev, g.ahead, sizeof(g.prev));
  for (i = 0; i < 2 * n; ++i) {
    long long s = g.acc[i];
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    g.buf[i] = (float)s / 32768.0f;
  }
  g.buf_pos = 0;
  g.buf_len = n;
}

/* 0 when the song has come to its end */
static int next_tick(void) {
  if (g.tick >= g.ticks) return 0;
  while (g.next < g.notes && g.time[g.next] <= g.counter) handle_note(g.next++);
  ++g.counter;
  ++g.tick;
  render_tick();
  return 1;
}

int synthw_kdm_render(float* out, int frames) {
  int done = 0;
  if (!g.open) return 0;
  while (done < frames) {
    int k;
    if (g.buf_pos >= g.buf_len && !next_tick()) break;
    k = g.buf_len - g.buf_pos;
    if (k > frames - done) k = frames - done;
    memcpy(out + 2 * done, g.buf + 2 * g.buf_pos, sizeof(float) * 2 * (size_t)k);
    g.buf_pos += k;
    done += k;
  }
  return done;
}

void synthw_kdm_close(void) {
  release();
}
