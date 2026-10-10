/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bbmp-xmp: plays a module with libxmp, for the formats libopenmpt doesn't read
 * (Archimedes Tracker, Coconizer and others).
 *
 *   bbmp-xmp <file> <track> <samplerate> [start-ms]
 *
 * One line of JSON about the song on stdout, then s16le stereo. */

#include <xmp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Module text is in the character set of the machine it was written on. What isn't
 * ASCII is written as Latin-1, which is right for most and readable for the rest. */
static void json(const char* s) {
  putchar('"');
  for (; s && *s; ++s) {
    const unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
    else if (c < 0x20) printf("\\u%04x", c);
    else if (c < 0x80) putchar(c);
    else { putchar(0xC0 | (c >> 6)); putchar(0x80 | (c & 0x3F)); }
  }
  putchar('"');
}

static int fail(const char* why) {
  printf("{\"tracks\":0,\"error\":");
  json(why);
  printf("}\n");
  return 1;
}

int main(int argc, char* argv[]) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s <file> <track> <samplerate> [start-ms]\n", argv[0]);
    return 2;
  }
  const char* path = argv[1];
  int track = atoi(argv[2]);
  const int rate = atoi(argv[3]);
  const long start_ms = argc > 4 ? atol(argv[4]) : 0;

  xmp_context ctx = xmp_create_context();
  if (!ctx) return fail("libxmp couldn't start");
  const int loaded = xmp_load_module(ctx, path);
  if (loaded == -XMP_ERROR_FORMAT) return fail("not a module libxmp knows");
  if (loaded == -XMP_ERROR_DEPACK) return fail("the file is packed in a way libxmp can't unpack");
  if (loaded != 0) return fail("libxmp can't load this file");
  if (xmp_start_player(ctx, rate, 0) != 0) return fail("libxmp can't play this file");

  struct xmp_module_info mi;
  xmp_get_module_info(ctx, &mi);
  const int tracks = mi.num_sequences > 0 ? mi.num_sequences : 1;
  if (track < 0 || track >= tracks) track = 0;
  if (track > 0) xmp_set_position(ctx, mi.seq_data[track].entry_point);
  const int duration = mi.num_sequences > 0 ? mi.seq_data[track].duration : 0;

  char facts[256];
  snprintf(facts, sizeof(facts), "\"Channels\":\"%d\",\"Patterns\":\"%d\",\"Instruments\":\"%d\","
           "\"Samples\":\"%d\"", mi.mod->chn, mi.mod->pat, mi.mod->ins, mi.mod->smp);
  printf("{\"tracks\":%d,\"track\":%d,\"title\":", tracks, track);
  json(mi.mod->name);
  printf(",\"author\":\"\",\"type\":");
  json(mi.mod->type);
  printf(",\"duration_ms\":%d,\"facts\":{%s},\"comment\":", duration, facts);
  json(mi.comment ? mi.comment : "");
  printf(",\"names\":[");
  int first = 1, i;
  for (i = 0; i < mi.mod->ins; ++i) {
    if (!first) putchar(',');
    json(mi.mod->xxi[i].name);
    first = 0;
  }
  printf("]}\n");
  fflush(stdout);

  if (start_ms > 0) xmp_seek_time(ctx, (int)start_ms);

  static char buffer[4096 * 4];
  /* loop 1: xmp_play_buffer returns -XMP_END when the song would start over */
  while (xmp_play_buffer(ctx, buffer, sizeof(buffer), 1) == 0) {
    if (fwrite(buffer, 1, sizeof(buffer), stdout) != sizeof(buffer)) break;
  }
  fflush(stdout);
  xmp_end_player(ctx);
  xmp_release_module(ctx);
  xmp_free_context(ctx);
  return 0;
}
