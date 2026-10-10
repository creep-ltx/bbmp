// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-chipnsfx: play a CHIPNSFX song (.chp), as raw PCM on stdout.
//
//   bbmp-chipnsfx <file> <track> <samplerate> [start-ms]
//
// CHIPNSFX is César Nicolás-González's (CNGSoft) three-channel AY tracker for the
// Amstrad CPC, ZX Spectrum and MSX; its songs are text. The song is loaded and played
// by the tracker's own code (CHIPNSFX.C, GPL-3.0-or-later, unchanged, built without
// its user interface), which makes 8-bit sound at 44100 Hz, in stereo as ABC.
//
// Same protocol as the other helpers: one JSON line describing the track, then
// signed 16-bit little-endian stereo until the track ends.
//
// What the tracker's loader trusts, a damaged file can't use: the song is tidied in
// memory first (at most 255 orders, instrument names that fit), and its tempo must
// be one a song can have.

#define DEBUG       // the tracker without its user interface
#define main chipnsfx_main
#include "CHIPNSFX.C"
#undef main

#include <stdint.h>

#define MAX_FILE (1 << 20)
#define MAX_FRAMES (100 * 60 * 30)   // the engine's frames are 1/100 s: 30 minutes
#define NAME_MAX_LEN 16              // instr[i][8..24]

static void json(const char *s) {
  putchar('"');
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p == '"' || *p == '\\') printf("\\%c", *p);
    else if (*p < 0x20 || *p == 0x7f) printf("\\u%04x", *p);
    else if (*p < 0x80) putchar(*p);
    else printf("\\u%04x", *p);          // the files are Latin-1
  }
  putchar('"');
}

static int fail(const char *why) {
  printf("{\"tracks\":0,\"error\":");
  json(why);
  printf("}\n");
  fflush(stdout);
  return 1;
}

// The song as the loader may read it: lines no longer than its buffer, at most 255
// orders (the loader writes them before it counts them), names of at most 16 bytes.
static char *tidy(const char *data, size_t size, size_t *out_size) {
  char *out = malloc(size + 2);
  size_t o = 0, line = 0, orders = 0;
  int in_orders = 0;
  if (!out) return NULL;
  for (size_t i = 0; i < size;) {
    size_t end = i;
    while (end < size && data[end] != '\n') end++;
    size_t len = end - i;
    if (len > MAXSTRLEN - 2) len = MAXSTRLEN - 2;
    const char *l = data + i;
    int keep = 1;
    if (line == 5) in_orders = 1;
    if (in_orders && (len == 0 || l[0] != '#')) in_orders = 0;
    if (in_orders && ++orders > 255) keep = 0;
    if (keep && len > 0 && l[0] == ':') {
      // ":02 FFFE 0000 141 Ding": the name is what follows the fourth space
      size_t spaces = 0, at = 0;
      for (size_t k = 0; k < len; k++)
        if (l[k] == ' ' && ++spaces == 4) { at = k + 1; break; }
      if (at && len - at > NAME_MAX_LEN) len = at + NAME_MAX_LEN;
    }
    if (keep) {
      memcpy(out + o, l, len);
      o += len;
      out[o++] = '\n';
    }
    line++;
    i = end + 1;
  }
  *out_size = o;
  return out;
}

static int hertz_ok(void) { return hertz >= 1 && hertz <= 1000 && divisor >= 1 && divisor <= 255; }

// One frame of the engine (1/100 s, 441 samples, 8-bit stereo). 0 when the song is over.
static int frame(unsigned char *t) {
  if (!snd_frame(1)) return 0;
  snd_waves(t);
  return 1;
}

static void restart(void) {
  snd_order = 0;
  snd_score = 0;
  flag_n = 1;     // play to the end once; the loop isn't played again
  snd_play(0);
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s <file> <track> <samplerate> [start-ms]\n", argv[0]);
    return 2;
  }
  int rate = atoi(argv[3]);
  if (rate < 8000 || rate > 192000) rate = 48000;
  long long start_ms = argc > 4 ? atoll(argv[4]) : 0;
  if (start_ms < 0) start_ms = 0;

  FILE *in = fopen(argv[1], "rb");
  if (!in) return fail("can't read the file");
  char *data = malloc(MAX_FILE);
  size_t size = data ? fread(data, 1, MAX_FILE, in) : 0;
  fclose(in);
  if (!size) return fail("can't read the file");
  size_t tidy_size;
  char *song = tidy(data, size, &tidy_size);
  free(data);
  if (!song) return fail("out of memory");
  FILE *mem = fmemopen(song, tidy_size, "r");
  if (!mem) return fail("out of memory");
  stdin = mem;                       // loadsong("-") reads stdin
  if (!loadsong("-")) return fail("not a CHIPNSFX song");
  if (!hertz_ok()) return fail("the song's tempo is not one a song can have");
  if (!orders || !scrln[order[0][0][0]] || !scrln[order[1][0][0]] || !scrln[order[2][0][0]])
    return fail("the song has nothing to play");

  // as the tracker's WAVE export sets it up, with its oversampling (-Q) and stereo ABC
  SND_DEPTH = 1;
  SND_DEPTHSTEP = 125000 << (1 + SND_FIXED - SND_DEPTH);
  SND_DEPTHMASK = (1 << SND_DEPTH) - 1;
  flag_s = 1, flag_ss = 1;
  stereo_u[0] = 2, stereo_u[1] = 1, stereo_u[2] = 0;

  // how long: the score walked through once, without sound
  long frames = 0;
  restart();
  while (frames < MAX_FRAMES && snd_frame(1)) frames++;
  if (!frames) return fail("the song has nothing to play");

  printf("{\"tracks\":1,\"track\":0,\"title\":");
  json((char *)title1);
  printf(",\"author\":\"\",\"type\":\"CHIPNSFX\",\"duration_ms\":%ld,\"facts\":{\"Tempo\":",
         frames * 10);
  char tempo[64];
  snprintf(tempo, sizeof tempo, "%d Hz, %d ticks a row", hertz, divisor);
  json(tempo);
  printf("},\"comment\":");
  json((char *)title2);
  printf(",\"names\":[]}\n");
  fflush(stdout);

  restart();
  unsigned char t[SND_QUANTUM * 2];
  long skip = start_ms / 10;
  for (long i = 0; i < skip && i < frames; i++)
    if (!frame(t)) return 0;

  // 44100 Hz to the rate asked for, by straight lines between samples; the engine's
  // sound is 0..255 above silence at 0, so what stays (the mean) is taken away
  double step = (double)SND_QUALITY / rate, pos = 0, dc[2] = {0, 0};
  int16_t out[4096 * 2];
  int n = 0, have = 0, at = 0;
  float prev[2] = {0, 0}, cur[2] = {0, 0};
  long done = skip;
  for (;;) {
    while (pos >= 1.0 || !have) {
      if (at >= SND_QUANTUM || !have) {
        if (done >= frames || !frame(t)) goto end;
        done++, at = 0;
      }
      prev[0] = cur[0], prev[1] = cur[1];
      for (int c = 0; c < 2; c++) {
        double v = t[at * 2 + c];
        dc[c] += (v - dc[c]) * 0.0005;
        cur[c] = (float)(v - dc[c]);
      }
      at++;
      if (have) pos -= 1.0;
      have = 1;
    }
    double side[2];
    for (int c = 0; c < 2; c++) side[c] = (prev[c] + (cur[c] - prev[c]) * pos) * 120.0;
    for (int c = 0; c < 2; c++) {
      // ABC is hard: A only left, C only right. A quarter of each side goes into the
      // other, as the player mixes the Amiga's channels, so one channel isn't one ear
      double v = side[c] * 0.75 + side[1 - c] * 0.25;
      out[n * 2 + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    pos += step;
    if (++n == 4096) {
      if (fwrite(out, 4, n, stdout) != (size_t)n) return 0;
      n = 0;
    }
  }
end:
  if (n) fwrite(out, 4, n, stdout);
  return 0;
}
