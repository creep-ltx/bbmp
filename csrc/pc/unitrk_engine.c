/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bbmp-pc, engine "unitrk": MikMod's own module format (UNITRK, *.uni), played
 * by libmikmod itself. The files are what MikMod's converter MikCvt made of XM,
 * S3M, MOD and other modules, for games and demos that used MikMod. */

#include "engine.h"

#include <mikmod.h>

#include <stdio.h>
#include <string.h>

static MODULE* g_mod;
static int g_started;
static long long g_frames, g_limit;

static void fail(pc_info* info, const char* what) {
  snprintf(info->error, sizeof(info->error), "%s: %s", what, MikMod_strerror(MikMod_errno));
}

/* The length of the song in frames, found by playing it once without sound. */
static long long measure(const uint8_t* data, size_t size, int rate) {
  static SBYTE buf[16384];
  long long frames = 0;
  const long long most = 3600LL * rate;
  MODULE* m = Player_LoadMem((const char*)data, (int)size, 128, 0);
  if (!m) return 0;
  m->loop = 0;
  m->wrap = 0;
  m->fadeout = 0;
  Player_Start(m);
  while (Player_Active() && frames < most) {
    const ULONG got = VC_WriteBytes(buf, sizeof(buf));
    if (!got) break;
    frames += got / 4;
  }
  Player_Stop();
  Player_Free(m);
  return frames;
}

int pcw_unitrk_open(const char* path, const uint8_t* data, size_t size, int track, pc_info* info) {
  int i;
  (void)path;
  if (size < 4 || memcmp(data, "UN0", 3) != 0 || data[3] < '4' || data[3] > '6') {
    /* APUN is the later UNI of libmikmod 3.0's converter */
    if (size < 6 || memcmp(data, "APUN\01", 5) != 0) {
      snprintf(info->error, sizeof(info->error), "not a MikMod UNITRK module");
      return 0;
    }
  }
  if (track != 0) {
    snprintf(info->error, sizeof(info->error), "no such track");
    return 0;
  }
  MikMod_RegisterDriver(&drv_nos);
  MikMod_RegisterLoader(&load_uni);
  md_mode = DMODE_16BITS | DMODE_STEREO | DMODE_SOFT_MUSIC | DMODE_SOFT_SNDFX | DMODE_INTERP;
  md_mixfreq = info->rate;
  md_reverb = 0;
  md_pansep = 128;
  if (MikMod_Init("")) { fail(info, "libmikmod doesn't start"); return 0; }
  g_started = 1;
  g_limit = measure(data, size, info->rate);
  g_mod = Player_LoadMem((const char*)data, (int)size, 128, 0);
  if (!g_mod) { fail(info, "libmikmod can't load the file"); pcw_unitrk_close(); return 0; }
  g_mod->loop = 0;      /* a jump backwards ends the song */
  g_mod->wrap = 0;
  g_mod->fadeout = 0;
  info->tracks = 1;
  info->duration_ms = (long)(g_limit * 1000 / info->rate);
  snprintf(info->title, sizeof(info->title), "%s", g_mod->songname ? g_mod->songname : "");
  snprintf(info->type, sizeof(info->type), "%s", g_mod->modtype ? g_mod->modtype : "MikMod UNITRK");
  snprintf(info->comment, sizeof(info->comment), "%s", g_mod->comment ? g_mod->comment : "");
  if (g_mod->instruments) {
    for (i = 0; i < g_mod->numins && i < PC_MAX_NAMES; ++i) {
      const char* n = g_mod->instruments[i].insname;
      snprintf(info->names[i], sizeof(info->names[i]), "%s", n ? n : "");
      info->n_names = i + 1;
    }
  } else if (g_mod->samples) {
    for (i = 0; i < g_mod->numsmp && i < PC_MAX_NAMES; ++i) {
      const char* n = g_mod->samples[i].samplename;
      snprintf(info->names[i], sizeof(info->names[i]), "%s", n ? n : "");
      info->n_names = i + 1;
    }
  }
  g_frames = 0;
  Player_Start(g_mod);
  return 1;
}

int pcw_unitrk_render(float* out, int frames) {
  static SWORD buf[4096 * 2];
  int done = 0, i;
  while (done < frames && g_mod && Player_Active()) {
    int want = frames - done;
    ULONG got;
    if (want > 4096) want = 4096;
    got = VC_WriteBytes((SBYTE*)buf, (ULONG)want * 4) / 4;
    if (!got) break;
    for (i = 0; i < (int)got * 2; ++i) out[done * 2 + i] = buf[i] / 32768.0f;
    done += (int)got;
    g_frames += got;
  }
  return done;
}

void pcw_unitrk_close(void) {
  if (g_mod) { Player_Stop(); Player_Free(g_mod); g_mod = NULL; }
  if (g_started) { MikMod_Exit(); g_started = 0; }
}
