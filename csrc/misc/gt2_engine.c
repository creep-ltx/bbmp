/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bbmp-misc: GoatTracker 2's play routine and song loader without the editor.
 *
 * gplay.c, gsong.c, gtable.c and ginstr.c are compiled as they are; this file is what
 * they need of the rest of the editor (its settings and its cursor positions), and the
 * small interface the helper uses. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "goattrk2.h"
#include "gt_engine.h"

/* the editor's settings (goattrk2.c) */
int menu = 0;
int editmode = EDIT_PATTERN;
int recordmode = 0;
int followplay = 0;
int hexnybble = -1;
int stepsize = 4;
int autoadvance = 0;
int defaultpatternlength = 64;
int cursorflash = 0;
int exitprogram = 0;
int eacolumn = 0;
int eamode = 0;
unsigned keypreset = KEY_TRACKER;
unsigned playerversion = 0;
int fileformat = 0;
int zeropageadr = 0xfc;
int playeradr = 0x1000;
unsigned sidmodel = 0;
unsigned multiplier = 1;
unsigned adparam = 0x0f00;
unsigned ntsc = 0;
unsigned patternhex = 0;
unsigned sidaddress = 0xd400;
unsigned finevibrato = 1;
unsigned optimizepulse = 1;
unsigned optimizerealtime = 1;
unsigned usefinevib = 0;
unsigned interpolate = 0;
unsigned residdelay = 0;
unsigned customclockrate = 0;
char loadedsongfilename[MAX_FILENAME];
char songfilename[MAX_FILENAME];
char instrfilename[MAX_FILENAME];
char songfilter[MAX_FILENAME];
char songpath[MAX_PATHNAME];
char instrfilter[MAX_FILENAME];
char instrpath[MAX_PATHNAME];
char packedpath[MAX_PATHNAME];
char textbuffer[MAX_PATHNAME];
char* programname = "bbmp-misc";

/* keyboard (gconsole.c) */
int key = 0;
int rawkey = 0;
int shiftpressed = 0;

/* cursor positions (gorder.c, gpattern.c) */
int espos[MAX_CHN];
int esend[MAX_CHN];
int eseditpos;
int esview;
int escolumn;
int eschn;
int esnum;
int esmarkchn = -1;
int esmarkstart;
int esmarkend;
int enpos;
int epnum[MAX_CHN];
int eppos;
int epview;
int epcolumn;
int epchn;
int epoctave = 2;
int epmarkchn = -1;
int epmarkstart;
int epmarkend;

/* the relocator's tables (greloc.c), which the table functions fill */
unsigned char pattused[MAX_PATT];
unsigned char instrused[MAX_INSTR];
unsigned char tableused[MAX_TABLES][MAX_TABLELEN + 1];
int tableerror;

void songchange(void) {
  int c;
  for (c = 0; c < MAX_CHN; c++) {
    espos[c] = 0;
    esend[c] = 0;
    epnum[c] = c;
  }
  stopsong();
}

void editstring(char* buffer, int maxlength) { (void)buffer; (void)maxlength; }
void resettime(void) {}
void incrementtime(void) {}
void sound_suspend(void) {}
void sound_flush(void) {}

/* BME's file functions (bme_end.c) */
void fwrite8(FILE* file, unsigned data) { (void)file; (void)data; }
void fwritele16(FILE* file, unsigned data) { (void)file; (void)data; }
void fwritele32(FILE* file, unsigned data) { (void)file; (void)data; }
unsigned fread8(FILE* file) {
  unsigned char byte = 0;
  if (fread(&byte, 1, 1, file) != 1) return 0;
  return byte;
}
unsigned freadle16(FILE* file) {
  const unsigned low = fread8(file);
  return low | (fread8(file) << 8);
}
unsigned freadle32(FILE* file) {
  const unsigned low = freadle16(file);
  return low | (freadle16(file) << 16);
}

/* gsong.c is compiled with fopen renamed to this: the song comes from memory, so that
 * its path doesn't have to fit into the editor's 60 characters. */
static const unsigned char* filedata;
static unsigned long filesize;

FILE* gtw_fopen(const char* name, const char* mode) {
  (void)name;
  if (!filedata || mode[0] != 'r') return NULL;
  return fmemopen((void*)filedata, filesize, "rb");
}

/* in gplay.c */
extern int songinit;
extern int psnum;
extern unsigned char funktable[2];

static int wrapped;

static int exists(int song) {
  return songlen[song][0] && songlen[song][1] && songlen[song][2];
}

int gt2w_load(const unsigned char* data, unsigned long size, const GtOptions* options, GtSong* song) {
  int c, d;
  multiplier = options->multiplier > 16 ? 16 : options->multiplier;
  adparam = options->adparam & 0xffff;
  finevibrato = options->finevibrato;
  optimizepulse = options->optimizepulse ? 1 : 0;
  optimizerealtime = options->optimizerealtime ? 1 : 0;
  usefinevib = 0;
  if (finevibrato == 1 && multiplier < 2) usefinevib = 1;
  if (finevibrato > 1) usefinevib = 1;

  initchannels();
  clearsong(1, 1, 1, 1, 1);
  filedata = data;
  filesize = size;
  strcpy(songfilename, "song.sng");
  loadedsongfilename[0] = 0;
  loadsong();
  filedata = NULL;
  if (strcmp(loadedsongfilename, songfilename) != 0) return 0;
  /* The play routine takes a row's instrument as an index into its 64 instruments
     (gplay.c: instr[cptr->instr]); the editor never writes more, a damaged file may. */
  for (c = 0; c < MAX_PATT; c++)
    for (d = 0; d < pattlen[c]; d++)
      if (pattern[c][d * 4 + 1] >= MAX_INSTR) return 0;
  /* Up to 2.17 the wavetable's values 08-0F were waveforms (the test bit and gate);
     from 2.18 they are delays, and those waveforms are E8-EF. The file format stayed
     GTS2 until 2.2, and the editor loads GTS2 files as they are ("old songs have to be
     converted by hand"); GoatTracker 1's songs are converted when loaded. */
  if (options->oldwaves && size >= 4 && !memcmp(data, "GTS2", 4))
    for (c = 0; c < MAX_TABLELEN; c++)
      if (ltable[WTBL][c] >= 0x08 && ltable[WTBL][c] <= 0x0f) ltable[WTBL][c] |= 0xe0;

  song->name = songname;
  song->author = authorname;
  song->copyright = copyrightname;
  song->songs = 0;
  for (c = 0; c < MAX_SONGS; c++)
    if (exists(c)) song->songs++;
  song->instruments = 0;
  for (c = 1; c < MAX_INSTR; c++)
    if (instr[c].ad || instr[c].sr || instr[c].ptr[WTBL] || instr[c].name[0]) song->instruments = c;
  song->firstTempo = 0;
  for (c = 0; c < MAX_CHN && !song->firstTempo; c++) {
    const unsigned char* order = songorder[0][c];
    int p = 0;
    while (p < MAX_SONGLEN && order[p] >= REPEAT && order[p] < LOOPSONG) p++;
    if (order[p] >= MAX_PATT) continue;
    for (d = 0; d < pattlen[order[p]] && d < 4; d++) {
      const unsigned char* row = &pattern[order[p]][d * 4];
      if (row[2] == CMD_SETTEMPO && (row[3] & 0x7f) >= 3) {
        song->firstTempo = row[3] & 0x7f;
        break;
      }
    }
  }
  return 1;
}

const char* gt2w_instrument(int number) {
  static char name[MAX_INSTRNAMELEN + 1];
  if (number < 1 || number >= MAX_INSTR) return "";
  memcpy(name, instr[number].name, MAX_INSTRNAMELEN);
  name[MAX_INSTRNAMELEN] = 0;
  return name;
}

void gt2w_start(int song) {
  int c, n = 0, found = 0;
  for (c = 0; c < MAX_SONGS; c++) {
    if (!exists(c)) continue;
    if (n == song) {
      found = c;
      break;
    }
    n++;
  }
  memset(sidreg, 0, NUMSIDREGS);
  initchannels();
  initsong(found, PLAY_BEGINNING);
  playroutine();                 /* the call that sets the song up */
  wrapped = 0;
}

int gt2w_wrapped(void) { return wrapped; }

int gt2w_tick(unsigned char* regs) {
  int c, atjump = 0;
  if (songinit == PLAY_STOPPED) return 0;
  /* a voice whose pattern has ended takes the next from its order list in this call */
  for (c = 0; c < MAX_CHN; c++)
    if (chn[c].pattptr == 0x7fffffff && chn[c].advance &&
        songorder[psnum][c][chn[c].songptr] == LOOPSONG)
      atjump |= 1 << c;
  playroutine();
  for (c = 0; c < MAX_CHN; c++)
    if ((atjump & (1 << c)) && chn[c].pattptr != 0x7fffffff) wrapped |= 1 << c;
  memcpy(regs, sidreg, NUMSIDREGS);
  return songinit != PLAY_STOPPED && songinit != PLAY_STOP;
}

void gt2w_state(GtState* state) {
  unsigned char* p = state->bytes;
  int c;
  memset(state, 0, sizeof *state);
  for (c = 0; c < MAX_CHN; c++) {
    *p++ = chn[c].songptr;
    *p++ = chn[c].pattnum;
    *p++ = chn[c].pattptr & 0xff;
    *p++ = (chn[c].pattptr >> 8) & 0xff;
    *p++ = (chn[c].pattptr >> 24) & 0xff;
    *p++ = chn[c].repeat;
    *p++ = chn[c].trans;
    *p++ = chn[c].tick;
    *p++ = chn[c].tempo;
    *p++ = chn[c].gatetimer;
  }
  *p++ = funktable[0];
  *p++ = funktable[1];
}
