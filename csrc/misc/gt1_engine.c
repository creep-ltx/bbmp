/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bbmp-misc: GoatTracker 1's play routine without the editor.
 *
 * Version 1 is one source file, goattrk.c, editor and play routine together. The build
 * script cuts the play routine, its filter function and the frequency tables out of it
 * into gt1_play.inc; this file is the song in memory as the routine expects it, a
 * loader that checks what it reads, and the small interface the helper uses.
 *
 * Compiled a second time with GT1_STEREO, against the sources of GoatTracker 1 Stereo
 * (gstereo.c, cut into gt1s_play.inc): six voices on two SIDs, same file format with
 * six order lists per song. */

#include <stdio.h>
#include <string.h>

#include "goattrk.h"
#include "gt_engine.h"

#ifdef GT1_STEREO
#define API(name) gt1sw_##name
#define playroutine gt1s_playroutine
#define setfilter gt1s_setfilter
#define swapnybbles gt1s_swapnybbles
#define freqtbllo gt1s_freqtbllo
#define freqtblhi gt1s_freqtblhi
#else
#define API(name) gt1w_##name
#define playroutine gt1_playroutine
#define setfilter gt1_setfilter
#define swapnybbles gt1_swapnybbles
#define freqtbllo gt1_freqtbllo
#define freqtblhi gt1_freqtblhi
#endif

static CHN chn[MAX_CHN];
static INSTR instr[MAX_INSTR];
/* a row of MAX_WAVELEN * 2 bytes and room behind it: the play routine reads up to
   wavetable[..][position + 2] with an 8-bit position, 257 at most (a damaged table that
   runs on to its end read past the array: ASan) */
static unsigned char wavetable[MAX_INSTR][MAX_WAVELEN * 2 + 4];
static unsigned char songorder[MAX_SONGS][MAX_CHN][MAX_SONGLEN + 2];
static unsigned char pattern[MAX_PATT][MAX_PATTROWS * 3 + 3];
static int pattlen[MAX_PATT];
static int songlen[MAX_SONGS][MAX_CHN];
static unsigned char filtertable[256];
static unsigned char songname[MAX_STR + 1];
static unsigned char authorname[MAX_STR + 1];
static unsigned char copyrightname[MAX_STR + 1];
static unsigned adparam = 0x0f00;
static int espos[MAX_CHN];
static int esnum;
static int followplay;
static unsigned char masterfader = 0xff;
static unsigned char filterctrl, filtertype, filtercutoff, filtercutoffadd, filtertime, filterstep;
static unsigned char songinit;
static int playsongnum;
static unsigned multiplier = 1;
static unsigned gatetimer = 2;
static int timemin, timesec, timeframe;
static unsigned ntsc;
static unsigned char sidreg[32];

static unsigned char swapnybbles(unsigned char n);
#ifdef GT1_STEREO
static unsigned char filter2ctrl, filter2type, filter2cutoff, filter2cutoffadd, filter2time, filter2step;
static unsigned char sidreg2[32];
static void setfilter(unsigned char filternum, int c);
#include "gt1s_play.inc"
#else
static void setfilter(unsigned char filternum);
#include "gt1_play.inc"
#endif

static int wrapped;

static int exists(int song) {
  int c;
  for (c = 0; c < MAX_CHN; c++)
    if (!songlen[song][c]) return 0;
  return 1;
}

static void countlengths(void) {
  int c, d, e;
  for (c = 0; c < MAX_PATT; c++) {
    for (d = 0; d <= MAX_PATTROWS; d++)
      if (pattern[c][d * 3] == ENDPATT) break;
    pattlen[c] = d;
  }
  for (e = 0; e < MAX_SONGS; e++) {
    for (c = 0; c < MAX_CHN; c++) {
      for (d = 0; d < MAX_SONGLEN; d++)
        if (songorder[e][c][d] >= LOOPSONG) break;
      songlen[e][c] = d;
    }
  }
}

static void clear(void) {
  int c, d;
  memset(chn, 0, sizeof chn);
  memset(songorder, 0, sizeof songorder);
  for (c = 0; c < MAX_CHN; c++) {
    chn[c].tempo = 5;
    for (d = 0; d < MAX_SONGS; d++) songorder[d][c][0] = LOOPSONG;
  }
  memset(songname, 0, sizeof songname);
  memset(authorname, 0, sizeof authorname);
  memset(copyrightname, 0, sizeof copyrightname);
  for (c = 0; c < MAX_PATT; c++) {
    memset(pattern[c], 0, sizeof pattern[c]);
    for (d = 0; d < 64; d++) pattern[c][d * 3] = REST;
    for (d = 64; d <= MAX_PATTROWS; d++) pattern[c][d * 3] = ENDPATT;
  }
  memset(instr, 0, sizeof instr);
  memset(wavetable, 0, sizeof wavetable);
  for (c = 0; c < MAX_INSTR; c++) {
    wavetable[c][2] = 0xff;
    wavetable[c][MAX_WAVELEN * 2 - 2] = 0xff;
  }
  for (c = 0; c < MAX_FILT; c++) {
    filtertable[c * 4] = 0x80;
    filtertable[c * 4 + 1] = 0xf;
    filtertable[c * 4 + 2] = 0;
    filtertable[c * 4 + 3] = 0;
  }
}

typedef struct {
  const unsigned char* data;
  unsigned long size, pos;
  int failed;
} Reader;

static unsigned get8(Reader* r) {
  if (r->pos >= r->size) {
    r->failed = 1;
    return 0;
  }
  return r->data[r->pos++];
}

static void getbytes(Reader* r, unsigned char* to, unsigned long count, unsigned long room) {
  if (count > room || r->pos + count > r->size) {
    r->failed = 1;
    return;
  }
  memcpy(to, r->data + r->pos, count);
  r->pos += count;
}

/* As the editor loads a song (F10 in goattrk.c), with every length checked. */
int API(load)(const unsigned char* data, unsigned long size, const GtOptions* options, GtSong* song) {
  Reader r;
  unsigned amount, length, c, d;

  if (size < 4 || memcmp(data, "GTS!", 4)) return 0;
  r.data = data;
  r.size = size;
  r.pos = 4;
  r.failed = 0;
  clear();
  getbytes(&r, songname, MAX_STR, MAX_STR);
  getbytes(&r, authorname, MAX_STR, MAX_STR);
  getbytes(&r, copyrightname, MAX_STR, MAX_STR);
  amount = get8(&r);
  if (amount > MAX_SONGS) return 0;
  for (d = 0; d < amount; d++) {
    for (c = 0; c < MAX_CHN; c++) {
      length = get8(&r);
      getbytes(&r, songorder[d][c], length + 1, MAX_SONGLEN + 2);
      if (r.failed) return 0;
      if (songorder[d][c][length] == 0xfe) songorder[d][c][length] = LOOPSONG;   /* the old end mark */
    }
  }
  for (c = 1; c < MAX_INSTR; c++) {
    instr[c].ad = get8(&r);
    instr[c].sr = get8(&r);
    instr[c].pulse = get8(&r);
    instr[c].pulseadd = get8(&r);
    instr[c].pulselimitlow = get8(&r);
    instr[c].pulselimithigh = get8(&r);
    instr[c].filtertype = get8(&r);
    length = get8(&r);
    instr[c].wavetableindex = 1;
    getbytes(&r, instr[c].name, MAX_INSTRNAMELEN, MAX_INSTRNAMELEN);
    getbytes(&r, wavetable[c], length, MAX_WAVELEN * 2);
    if (r.failed) return 0;
  }
  amount = get8(&r);
  if (amount > MAX_PATT) return 0;
  for (c = 0; c < amount; c++) {
    length = get8(&r);
    getbytes(&r, pattern[c], length, MAX_PATTROWS * 3 + 3);
    if (r.failed) return 0;
    /* a pattern has to end: the play routine looks for the end mark */
    for (d = 0; d <= MAX_PATTROWS; d++)
      if (pattern[c][d * 3] == ENDPATT) break;
    if (d > MAX_PATTROWS) return 0;
  }
  countlengths();
  /* songs of the first versions end here and have the editor's empty filter table */
  if (r.pos < r.size) getbytes(&r, filtertable, 256, 256);
  if (r.failed || r.pos != r.size) return 0;

  multiplier = options->multiplier ? options->multiplier : 1;
  if (multiplier > 32) multiplier = 32;
  adparam = options->adparam & 0xffff;
  gatetimer = multiplier * (options->hardrestart == 1 ? 1 : 2);
  ntsc = 0;

  song->name = (const char*)songname;
  song->author = (const char*)authorname;
  song->copyright = (const char*)copyrightname;
  song->songs = 0;
  for (c = 0; c < MAX_SONGS; c++)
    if (exists(c)) song->songs++;
  song->instruments = 0;
  for (c = 1; c < MAX_INSTR; c++)
    if (instr[c].ad || instr[c].sr || instr[c].name[0]) song->instruments = c;
  song->firstTempo = 0;
  for (c = 0; c < MAX_CHN && !song->firstTempo; c++) {
    const unsigned char* order = songorder[0][c];
    int p = 0;
    while (p < MAX_SONGLEN && order[p] >= REPEAT && order[p] < LOOPSONG) p++;
    if (order[p] >= MAX_PATT) continue;
    for (d = 0; d < (unsigned)pattlen[order[p]] && d < 4; d++) {
      const unsigned char* row = &pattern[order[p]][d * 3];
      if ((row[1] & 7) == CMD_SETTEMPO && (row[2] & 0x7f) >= 3) {
        song->firstTempo = row[2] & 0x7f;
        break;
      }
    }
  }
  return 1;
}

const char* API(instrument)(int number) {
  static char name[MAX_INSTRNAMELEN + 1];
  if (number < 1 || number >= MAX_INSTR) return "";
  memcpy(name, instr[number].name, MAX_INSTRNAMELEN);
  name[MAX_INSTRNAMELEN] = 0;
  return name;
}

void API(start)(int song) {
  int c, n = 0, found = 0;
  for (c = 0; c < MAX_SONGS; c++) {
    if (!exists(c)) continue;
    if (n == song) {
      found = c;
      break;
    }
    n++;
  }
  memset(sidreg, 0, sizeof sidreg);
  memset(chn, 0, sizeof chn);
  for (c = 0; c < MAX_CHN; c++) chn[c].tempo = 5;
  filterctrl = filtertype = filtercutoff = filtercutoffadd = filtertime = filterstep = 0;
  /* the C64 routine starts at full volume, until a filter step sets it */
  filtertype = sidreg[0x18] = 0x0f;
#ifdef GT1_STEREO
  memset(sidreg2, 0, sizeof sidreg2);
  filter2ctrl = filter2type = filter2cutoff = filter2cutoffadd = filter2time = filter2step = 0;
  filter2type = sidreg2[0x18] = 0x0f;
#endif
  masterfader = 0xff;
  esnum = found;
  songinit = 0x01;
  playroutine();                 /* the call that sets the song up */
  wrapped = 0;
}

int API(wrapped)(void) { return wrapped; }

int API(tick)(unsigned char* regs) {
  int c, atjump = 0;
  if (songinit == 0x80) return 0;
  /* a voice whose pattern has ended takes the next from its order list in this call */
  for (c = 0; c < MAX_CHN; c++)
    if (chn[c].pattptr == ENDPATT && chn[c].advance && !chn[c].repeat &&
        songorder[playsongnum][c][chn[c].songptr] >= LOOPSONG)
      atjump |= 1 << c;
  playroutine();
  for (c = 0; c < MAX_CHN; c++)
    if ((atjump & (1 << c)) && chn[c].pattptr != ENDPATT) wrapped |= 1 << c;
  memcpy(regs, sidreg, 25);
#ifdef GT1_STEREO
  memcpy(regs + 25, sidreg2, 25);
#endif
  return songinit != 0x80 && songinit != 0x04;
}

void API(state)(GtState* state) {
  unsigned char* p = state->bytes;
  int c;
  memset(state, 0, sizeof *state);
  for (c = 0; c < MAX_CHN; c++) {
    *p++ = chn[c].songptr;
    *p++ = chn[c].pattnum;
    *p++ = chn[c].pattptr;
    *p++ = chn[c].repeat;
    *p++ = chn[c].trans;
    *p++ = chn[c].tick;
    *p++ = chn[c].tempo;
    *p++ = chn[c].advance;
  }
}
