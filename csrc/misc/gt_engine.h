/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bbmp-misc: GoatTracker's play routines (version 2 and version 1) behind one small
 * interface each. The routines are the editor's own, fetched by tools/build-misc.sh. */
#ifndef BBMP_GT_ENGINE_H
#define BBMP_GT_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  unsigned multiplier;        /* calls per frame; 0 is every second frame (25 Hz) */
  unsigned adparam;           /* attack/decay and sustain/release of the hard restart */
  unsigned finevibrato;       /* version 2, for songs older than its speed table */
  unsigned optimizepulse;     /* version 2: pulse table skipped on tick 0 */
  unsigned optimizerealtime;  /* version 2: effects skipped on tick 0 */
  unsigned hardrestart;       /* version 1: frames of hard restart, 1 or 2 */
  unsigned oldwaves;          /* version 2, GTS2: wavetable values 08-0F are waveforms (the
                                 editor before 2.18), not delays */
} GtOptions;

typedef struct {
  const char* name;
  const char* author;
  const char* copyright;
  int songs;                  /* subsongs that have all three order lists */
  int instruments;
  int firstTempo;             /* the first tempo set on the first rows, 0 if none */
} GtSong;

/* What decides how the song goes on: two moments with the same state play the same. */
typedef struct {
  unsigned char bytes[64];
} GtState;

/* version 2: GTS5, GTS4, GTS3, and GTS2 as the editor imports it. The file has to be
 * checked before (sizes within the editor's limits): the loader itself trusts it. */
int gt2w_load(const unsigned char* data, unsigned long size, const GtOptions* options, GtSong* song);
const char* gt2w_instrument(int number);         /* from 1 */
void gt2w_start(int song);                        /* counted over the songs that exist */
int gt2w_tick(unsigned char* sidreg);             /* 25 registers; 0 when the song stopped */
void gt2w_state(GtState* state);
int gt2w_wrapped(void);                          /* bit per voice: its order list has jumped back */

/* version 1: GTS! */
int gt1w_load(const unsigned char* data, unsigned long size, const GtOptions* options, GtSong* song);
const char* gt1w_instrument(int number);
void gt1w_start(int song);
int gt1w_tick(unsigned char* sidreg);
void gt1w_state(GtState* state);
int gt1w_wrapped(void);

/* version 1 stereo: GTS! with six order lists per song; 50 registers, of two SIDs */
int gt1sw_load(const unsigned char* data, unsigned long size, const GtOptions* options, GtSong* song);
const char* gt1sw_instrument(int number);
void gt1sw_start(int song);
int gt1sw_tick(unsigned char* sidreg);
void gt1sw_state(GtState* state);
int gt1sw_wrapped(void);

/* reSID as GoatTracker drives it: the registers are written once per call of the play
 * routine, in version 2 in the order and with the delays of its C64 routine (gsid.cpp of
 * version 2), in version 1 in groups (gsid.cpp of version 1). `chip` is 0, or 1 for
 * the second SID of a stereo song. */
void gtw_sid_init(int rate, int model8580, int ntsc);
int gtw_sid_render(const unsigned char* sidreg, short* mono, int samples);
int gtw_sid_render1(int chip, const unsigned char* sidreg, short* mono, int samples);

#ifdef __cplusplus
}
#endif

#endif
