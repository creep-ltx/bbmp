/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bbmp-pc, engine "buzz": Jeskola Buzz songs (*.bmx), as far as they can be played
 * without Buzz's machines, which are Windows plug-ins, most of them without source.
 *
 * What plays are the songs that use only the machines of BmxPlay, a small Buzz
 * player by Joric^Proxium (public domain, https://github.com/joric/bmxplay): _303,
 * _xi, _voice, _filter and _delay. Its loader and its machines are used as they are
 * (bmxplay.h, with its sound output taken out by tools/build-pc.sh). Every other
 * song is refused, with the name of the first machine that is missing.
 *
 * The sequencer here is BmxPlay's BmxTick() with two additions: patterns of the
 * master machine (volume, tempo) are obeyed, and the song stops at its end. */

#include "engine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bmxplay.h"

#define BUZZ_RATE 44100

static char* g_data;            /* bmxplay keeps pointers into the file and writes to it */
static size_t g_size;
static long g_ticks_left;
static int g_in_tick;           /* frames of the current tick still to mix */
static short g_mix[BUFSIZE * 2];
static uint16_t g_conn[65536][2];  /* the song's connections, source and destination */

/* ---- a look at the file before BmxPlay, which checks nothing, gets it ---- */

typedef struct { const uint8_t* p; const uint8_t* end; int bad; } reader;

static uint32_t rd(reader* r, int bytes) {
  uint32_t v = 0;
  int i;
  if (r->bad || r->end - r->p < bytes) { r->bad = 1; return 0; }
  for (i = 0; i < bytes; ++i) v |= (uint32_t)r->p[i] << (8 * i);
  r->p += bytes;
  return v;
}
static const char* rd_str(reader* r) {
  const char* s = (const char*)r->p;
  while (!r->bad && r->p < r->end && *r->p) r->p++;
  if (r->p >= r->end) { r->bad = 1; return ""; }
  r->p++;
  return s;
}
static void rd_skip(reader* r, size_t bytes) {
  if (r->bad || (size_t)(r->end - r->p) < bytes) r->bad = 1; else r->p += bytes;
}

/* Buzz is a Windows program: its text is Latin-1 (or near it) */
static void latin1(char* to, size_t size, const char* from) {
  size_t n = 0;
  for (; *from && n + 3 < size; ++from) {
    const unsigned char c = (unsigned char)*from;
    if (c < 0x80) to[n++] = (char)c;
    else { to[n++] = (char)(0xC0 | (c >> 6)); to[n++] = (char)(0x80 | (c & 0x3F)); }
  }
  to[n] = 0;
}

static const BmxMachineHeader* gear(const char* dll) {
  size_t k;
  for (k = 0; k < sizeof(BmxGears) / sizeof(BmxGears[0]); ++k)
    if (strcmp(dll, BmxGears[k]->dllname) == 0) return BmxGears[k];
  return NULL;
}

/* Finds a section by name; returns its place in the directory + 1, or 0 when it
 * is missing, damaged, or there more than once (BmxLoad reads every entry of the
 * directory, so a second one would be read without having been looked at). */
static int section(const uint8_t* data, size_t size, const char* name, reader* r) {
  const uint32_t n = data[4] | data[5] << 8 | data[6] << 16 | (uint32_t)data[7] << 24;
  uint32_t i;
  int found = 0;
  if (n > BMX_ENTRIES || 8 + 12 * (size_t)n > size) return 0;
  for (i = 0; i < n; ++i) {
    const uint8_t* e = data + 8 + 12 * i;
    const uint32_t off = e[4] | e[5] << 8 | e[6] << 16 | (uint32_t)e[7] << 24;
    const uint32_t len = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
    if (memcmp(e, name, 4) != 0) continue;
    if (found || off > size || len > size - off) return 0;
    r->p = data + off;
    r->end = data + off + len;
    r->bad = 0;
    found = (int)i + 1;
  }
  return found;
}

/* The _xi machine's data, as its init reads it: 4 bytes, then its sample's header
 * as the 32-bit program wrote it (74 bytes), then the sample, 8 bits a byte or
 * 4 bits (packed two to a byte). Its work reads one byte past the sample's end. */
static int xi_data_whole(const uint8_t* d, size_t length) {
  int32_t samples;
  int8_t points;
  uint8_t packed;
  size_t bytes;
  if (length < 4 + 74) return 0;
  memcpy(&samples, d + 4, 4);
  points = (int8_t)d[4 + 12];
  packed = d[4 + 12 + 1 + 48 + 2];
  if (samples < 0 || points < 0 || points > 12) return 0;
  bytes = packed == 1 ? (size_t)samples / 2 : (size_t)samples;
  return bytes <= length - (4 + 74);
}

/* 1 when every machine is one of BmxPlay's and the sections are whole */
static int check(const uint8_t* data, size_t size, pc_info* info) {
  reader r;
  int machines, i, k;
  int tracks[BMX_MACHINES], sources[BMX_MACHINES], gp[BMX_MACHINES], tp[BMX_MACHINES];
  int waiting[BMX_MACHINES];
  int at_mach, at_conn, at_patt;
  if (size < 8 || memcmp(data, "Buzz", 4) != 0) {
    snprintf(info->error, sizeof(info->error), "not a Buzz song");
    return 0;
  }
  /* BmxLoad reads the sections in the order of the directory, and needs the
   * machines before the connections, and the connections before the patterns */
  {
    reader skip;
    at_conn = section(data, size, "CONN", &skip);
    at_patt = section(data, size, "PATT", &skip);
  }
  if (!(at_mach = section(data, size, "MACH", &r))) goto broken;
  if (!at_conn || !at_patt || at_mach > at_conn || at_conn > at_patt) goto broken;
  machines = (int)rd(&r, 2);
  if (machines < 1 || machines > BMX_MACHINES) goto broken;
  for (i = 0; i < machines; ++i) {
    const char* name = rd_str(&r);
    const int type = (int)rd(&r, 1);
    const char* dll = type ? rd_str(&r) : "Master";
    const BmxMachineHeader* g = type ? gear(dll) : NULL;
    int attrs;
    if (r.bad) goto broken;
    /* the master first, and only there: BmxLoad starts every other machine */
    if ((i == 0) != (type == 0)) goto broken;
    if (type && !g) {
      char text[128];
      latin1(text, sizeof(text), dll);
      snprintf(info->error, sizeof(info->error),
               "the song needs the Buzz machine \"%s\", a Windows plug-in that can't be played here",
               text);
      return 0;
    }
    gp[i] = type ? (int)g->gpsize : 5;
    tp[i] = type ? (int)g->tpsize : 0;
    sources[i] = 0;
    if (info->n_names < PC_MAX_NAMES) {
      char text[40], both[80];
      latin1(text, sizeof(text), name);
      snprintf(both, sizeof(both), "%s (%.20s)", text, dll);
      snprintf(info->names[info->n_names++], sizeof(info->names[0]), "%.66s", both);
    }
    rd_skip(&r, 8);
    {
      const uint32_t length = rd(&r, 4);
      if (!r.bad && g && strcmp(g->dllname, "_xi") == 0 &&
          (length > (size_t)(r.end - r.p) || !xi_data_whole(r.p, length)))
        goto broken;
      rd_skip(&r, length);
    }
    attrs = (int)rd(&r, 2);
    if (attrs > BMX_MACHINE_ATTRS) goto broken;
    for (k = 0; k < attrs; ++k) { rd_str(&r); rd(&r, 4); }
    rd_skip(&r, (size_t)gp[i]);
    tracks[i] = (int)rd(&r, 2);
    /* BmxPlay's machines play their first track only */
    if (type && tracks[i] > 1) {
      snprintf(info->error, sizeof(info->error),
               "the machine \"%.60s\" has %d tracks, BmxPlay's machines play one", name, tracks[i]);
      return 0;
    }
    rd_skip(&r, (size_t)tracks[i] * tp[i]);
    if (r.bad) goto broken;
  }
  if (!section(data, size, "CONN", &r)) goto broken;
  k = (int)rd(&r, 2);
  for (i = 0; i < k; ++i) {
    const int src = (int)rd(&r, 2), dst = (int)rd(&r, 2);
    rd_skip(&r, 4);
    if (r.bad || src >= machines || dst >= machines) goto broken;
    g_conn[i][0] = (uint16_t)src;
    g_conn[i][1] = (uint16_t)dst;
    sources[dst]++;
  }
  /* BmxSmartMix works a machine once all its sources are done, until the master's
   * are; with a machine that feeds itself, or the master, or a loop, it looks for
   * one past the last machine and on past the end of its table. Done as it does. */
  {
    int machine = 0, c;
    for (i = 0; i < machines; ++i) waiting[i] = sources[i];
    while (waiting[0] != 0) {
      if (waiting[machine] != 0) {
        if (++machine >= machines) goto broken;
        continue;
      }
      for (c = 0; c < k; ++c)
        if (g_conn[c][0] == machine) waiting[g_conn[c][1]]--;
      waiting[machine]--;
      machine = 0;
    }
  }
  if (!section(data, size, "PATT", &r)) goto broken;
  {
    int total = 0;
    for (i = 0; i < machines; ++i) {
      const int patterns = (int)rd(&r, 2);
      const int ptracks = (int)rd(&r, 2);
      if (r.bad || patterns > BMX_PATTERNS || ptracks > BMX_TRACKS) goto broken;
      /* the sequencer hands a machine the first track of its pattern */
      if (patterns > 0 && tp[i] > 0 && ptracks < 1) goto broken;
      total += patterns;
      if (total > BMX_PATTERNS || sources[i] > BMX_TRACKS) goto broken;
      for (k = 0; k < patterns; ++k) {
        int rows, t;
        rd_str(&r);
        rows = (int)rd(&r, 2);
        for (t = 0; t < sources[i]; ++t) { rd(&r, 2); rd_skip(&r, (size_t)rows * 4); }
        rd_skip(&r, (size_t)rows * gp[i]);
        rd_skip(&r, (size_t)rows * tp[i] * ptracks);
        if (r.bad) goto broken;
      }
    }
  }
  if (!section(data, size, "SEQU", &r)) goto broken;
  rd_skip(&r, 12);
  k = (int)rd(&r, 2);
  if (k > BMX_SEQUENCES) goto broken;
  for (i = 0; i < k; ++i) {
    const int machine = (int)rd(&r, 2);
    const uint32_t events = rd(&r, 4);
    if (machine >= machines) goto broken;
    if (events) {
      const int psize = (int)rd(&r, 1), esize = (int)rd(&r, 1);
      if (psize < 1 || psize > 4 || esize < 1 || esize > 2) goto broken;
      rd_skip(&r, (size_t)events * (psize + esize));
    }
    if (r.bad) goto broken;
  }
  return 1;
broken:
  snprintf(info->error, sizeof(info->error), "the Buzz song is damaged");
  return 0;
}

/* ---- the sequencer: BmxPlay's BmxTick(), and the master's patterns ---- */

static void master_row(const char* row) {
  char* now = bmx.mach.m[0].gpar;       /* volume w, BPM w, TPB b */
  const unsigned volume = *(const WORD*)row, bpm = *(const WORD*)(row + 2);
  const unsigned tpb = *(const byte*)(row + 4);
  if (volume != 0xFFFF) *(WORD*)now = (WORD)volume;
  if (bpm != 0xFFFF && bpm >= 16) *(WORD*)(now + 2) = (WORD)bpm;
  if (tpb != 0xFF && tpb >= 1) *(WORD*)(now + 4) = (WORD)tpb | (*(WORD*)(now + 4) & 0xFF00);
}

static void tick(void) {
  unsigned j, k, t, r;
  const int now = BmxCurrentTick;
  for (j = 0; j < bmx.seq.sequences; j++) {
    BmxSequence* s = &bmx.seq.s[j];
    BmxMachine* m = &bmx.mach.m[s->machine];
    const char* data = s->data;
    m->active = TRUE;
    for (k = 0; k < s->events; k++) {
      unsigned pos = 0, event = 0;
      memcpy(&pos, data, s->psize);
      data += s->psize;
      memcpy(&event, data, s->esize);
      data += s->esize;
      if ((int)pos == now) { s->event = (WORD)event; s->row = 0; }
    }
    if (s->events != 0 && s->event >= 0x10 && s->event - 0x10 < m->patterns) {
      BmxPattern* p = &bmx.p[s->event - 0x10 + m->firstpattern];
      r = s->row;
      if (r < p->rows) {
        if (s->machine >= 1)
          ((LPFTICK)(m->mh->tick))(m->me, p->gdata + r * m->gpsize, p->tdata[0] + r * m->tpsize);
        else
          master_row(p->gdata + r * m->gpsize);
        t = 0;
        for (k = 0; k < bmx.con->connections; k++) {
          if (bmx.con->c[k].dst == s->machine) {
            const char* sp = p->sdata[t] + r * 2 * 2;
            const WORD amp = *(const WORD*)(sp + 0), pan = *(const WORD*)(sp + 2);
            if (amp != 0xffff) bmx.con->c[k].amp = amp;
            if (pan != 0xffff) bmx.con->c[k].pan = pan;
            t++;
          }
        }
      }
      s->row++;
      if (s->row >= p->rows) { s->row = 0; s->event = 0; }
    } else {
      s->row = 0;
    }
  }
  BmxCurrentTick++;
  BmxSamplesPerTick = BmxGetSamplesPerTick();
}

/* the song's length, by going through the master's patterns without sound */
static long measure(long ticks) {
  double frames = 0;
  long i;
  unsigned j;
  char keep[5];
  memcpy(keep, bmx.mach.m[0].gpar, 5);
  /* the song's length in ticks is a 32-bit number of the file: an hour is enough,
   * as the helper plays no more */
  for (i = 0; i < ticks && frames < 3600.0 * BUZZ_RATE; ++i) {
    for (j = 0; j < bmx.seq.sequences; j++) {
      BmxSequence* s = &bmx.seq.s[j];
      BmxMachine* m = &bmx.mach.m[s->machine];
      const char* data = s->data;
      unsigned k;
      if (s->machine != 0) continue;
      for (k = 0; k < s->events; k++) {
        unsigned pos = 0, event = 0;
        memcpy(&pos, data, s->psize);
        data += s->psize;
        memcpy(&event, data, s->esize);
        data += s->esize;
        if ((long)pos == i) { s->event = (WORD)event; s->row = 0; }
      }
      if (s->events != 0 && s->event >= 0x10 && s->event - 0x10 < m->patterns) {
        BmxPattern* p = &bmx.p[s->event - 0x10 + m->firstpattern];
        if (s->row < p->rows) master_row(p->gdata + s->row * m->gpsize);
        s->row++;
        if (s->row >= p->rows) { s->row = 0; s->event = 0; }
      }
    }
    {
      const int spt = BmxGetSamplesPerTick();
      if (spt <= 0) break;            /* where pcw_buzz_render() ends the song too */
      frames += spt;
    }
  }
  memcpy(bmx.mach.m[0].gpar, keep, 5);
  for (j = 0; j < bmx.seq.sequences; j++) { bmx.seq.s[j].event = 0; bmx.seq.s[j].row = 0; }
  return (long)(frames * 1000.0 / BUZZ_RATE);
}

int pcw_buzz_open(const char* path, const uint8_t* data, size_t size, int track, pc_info* info) {
  int err;
  unsigned bpm, tpb;
  (void)path;
  if (!check(data, size, info)) { info->n_names = 0; return 0; }
  if (track != 0) {
    snprintf(info->error, sizeof(info->error), "no such track");
    return 0;
  }
  g_size = size;
  g_data = (char*)malloc(size + 16);
  if (!g_data) return 0;
  memcpy(g_data, data, size);
  memset(g_data + size, 0, 16);
  memset(&bmx, 0, sizeof(bmx));
  BmxPosInTick = 0;
  err = BmxLoad(g_data);
  if (err) {
    snprintf(info->error, sizeof(info->error), "BmxPlay can't load the song (error %d)", err);
    return 0;
  }
  bpm = *(WORD*)(bmx.mach.m[0].gpar + 2);
  tpb = *(byte*)(bmx.mach.m[0].gpar + 4);
  if (bpm < 16 || tpb < 1 || !bmx.con) {
    snprintf(info->error, sizeof(info->error), "the Buzz song has no tempo");
    return 0;
  }
  /* BmxPlay reads the ticks per beat as a word, whose upper half is the next byte */
  *(WORD*)(bmx.mach.m[0].gpar + 4) = (WORD)tpb;
  g_ticks_left = (long)bmx.seq.endloop;
  if (g_ticks_left <= 0 || g_ticks_left > (long)bmx.seq.songsize) g_ticks_left = (long)bmx.seq.songsize;
  if (g_ticks_left <= 0) {
    snprintf(info->error, sizeof(info->error), "the Buzz song is empty");
    return 0;
  }
  info->duration_ms = measure(g_ticks_left);
  BmxCurrentTick = 0;
  BmxSamplesPerTick = BmxGetSamplesPerTick();
  g_in_tick = 0;
  info->rate = BUZZ_RATE;       /* BmxPlay's machines are made for this rate */
  info->tracks = 1;
  snprintf(info->type, sizeof(info->type), "Buzz (BmxPlay machines)");
  return 1;
}

int pcw_buzz_render(float* out, int frames) {
  int done = 0;
  while (done < frames) {
    int n, i;
    if (g_in_tick == 0) {
      if (g_ticks_left <= 0) break;
      g_ticks_left--;
      tick();
      g_in_tick = BmxSamplesPerTick;
      if (g_in_tick <= 0) break;
    }
    n = frames - done;
    if (n > g_in_tick) n = g_in_tick;
    if (n > BUFSIZE) n = BUFSIZE;
    BmxSmartMix(n, g_mix);
    for (i = 0; i < n * 2; ++i) out[done * 2 + i] = g_mix[i] / 32768.0f;
    done += n;
    g_in_tick -= n;
  }
  return done;
}

void pcw_buzz_close(void) {
  free(g_data);
  g_data = NULL;
}
