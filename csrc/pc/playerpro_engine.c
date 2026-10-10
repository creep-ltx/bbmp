/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bbmp-pc, engine "playerpro": PlayerPRO's MAD modules (*.mad), played by
 * PlayerPRO's own driver (PlayerPROCore, public domain, by Antoine Rosset,
 * maintained by C.W. Betts). MADK is the current format; MAD-F and MAD-G, MADH and
 * MADI are older ones, read by PlayerPRO's importers.
 *
 * PlayerPROCore finds its importers as shared libraries in a folder. Here they are
 * linked in, and this file has the functions of its Lin-PlugImport.c for that. */

#include "engine.h"

#include "RDriver.h"
#include "RDriverInt.h"
#include "MADFileUtils.h"
#include "MADPrivate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

MADErr mainMADfg(MADFourChar, char*, MADMusic*, MADInfoRec*, MADDriverSettings*);
MADErr mainMADH(MADFourChar, char*, MADMusic*, MADInfoRec*, MADDriverSettings*);
MADErr mainMADI(MADFourChar, char*, MADMusic*, MADInfoRec*, MADDriverSettings*);

/* ---- what PlayerPROCore wants of a plug-in loader ---- */

static const struct { const char* type; const char* name; MADPLUGFUNC main; } kPlugs[] = {
  {"MADF", "MAD-FG", mainMADfg},   /* reads both MADF and MADG */
  {"MADH", "MADH", mainMADH},
  {"MADI", "MADI", mainMADI},
};
#define N_PLUGS ((int)(sizeof(kPlugs) / sizeof(kPlugs[0])))

void MInitImportPlug(MADLibrary* lib, const char* folder) {
  int i;
  (void)folder;
  lib->ThePlug = (PlugInfo*)calloc(MAXPLUG, sizeof(PlugInfo));
  lib->TotalPlug = 0;
  for (i = 0; i < N_PLUGS; ++i) {
    PlugInfo* p = &lib->ThePlug[i];
    strncpy(p->type, kPlugs[i].type, sizeof(p->type) - 1);
    strncpy(p->MenuName, kPlugs[i].name, sizeof(p->MenuName) - 1);
    p->mode = MADPlugImport;
    p->IOPlug = kPlugs[i].main;
    lib->TotalPlug++;
  }
}

void CloseImportPlug(MADLibrary* lib) { free(lib->ThePlug); lib->ThePlug = NULL; }

static MADErr call(MADLibrary* lib, int n, MADFourChar order, char* file, MADMusic* music,
                   MADInfoRec* info) {
  MADDriverSettings settings;
  memset(&settings, 0, sizeof(settings));
  return (*lib->ThePlug[n].IOPlug)(order, file, music, info, &settings);
}

MADErr CheckMADFile(const char* name) {
  char head[4] = {0};
  UNFILE f = iFileOpenRead(name);
  if (!f) return MADReadingErr;
  iRead(4, head, f);
  iClose(f);
  return memcmp(head, "MADK", 4) == 0 ? MADNoErr : MADFileNotSupportedByThisPlug;
}

MADErr PPMADInfoFile(const char* file, MADInfoRec* info) {
  MADSpec* spec;
  UNFILE f;
  MADErr err = CheckMADFile(file);
  if (err != MADNoErr) return err;
  spec = (MADSpec*)calloc(1, sizeof(MADSpec) + 200);
  f = iFileOpenRead(file);
  if (!f) { free(spec); return MADReadingErr; }
  info->fileSize = iGetEOF(f);
  iRead(sizeof(MADSpec), spec, f);
  iClose(f);
  spec->name[sizeof(spec->name) - 1] = 0;
  strcpy(info->internalFileName, spec->name);
  info->signature = 'MADK';
  strcpy(info->formatDescription, "MADK");
  free(spec);
  return MADNoErr;
}

MADErr PPInfoFile(MADLibrary* lib, char* kind, char* file, MADInfoRec* info) {
  int i;
  MADMusic music;
  if (!strcmp(kind, "MADK")) return PPMADInfoFile(file, info);
  for (i = 0; i < lib->TotalPlug; i++)
    if (!strcmp(kind, lib->ThePlug[i].type)) return call(lib, i, MADPlugInfo, file, &music, info);
  return MADCannotFindPlug;
}

MADErr PPImportFile(MADLibrary* lib, char* kind, char* file, MADMusic** music) {
  int i;
  MADInfoRec info;
  for (i = 0; i < lib->TotalPlug; i++) {
    if (!strcmp(kind, lib->ThePlug[i].type)) {
      *music = (MADMusic*)calloc(sizeof(MADMusic), 1);
      if (!*music) return MADNeedMemory;
      return call(lib, i, MADPlugImport, file, *music, &info);
    }
  }
  return MADCannotFindPlug;
}

MADErr PPIdentifyFile(MADLibrary* lib, char* type, char* file) {
  int i;
  MADInfoRec info;
  UNFILE f = iFileOpenRead(file);
  strcpy(type, "!!!!");
  if (!f) return MADReadingErr;
  if (iGetEOF(f) < 100) { iClose(f); return MADIncompatibleFile; }
  iClose(f);
  if (CheckMADFile(file) == MADNoErr) { strcpy(type, "MADK"); return MADNoErr; }
  for (i = 0; i < lib->TotalPlug; i++) {
    if (call(lib, i, MADPlugTest, file, NULL, &info) == MADNoErr) {
      strcpy(type, lib->ThePlug[i].type);
      return MADNoErr;
    }
  }
  return MADCannotFindPlug;
}

bool MADPlugAvailable(const MADLibrary* lib, const char* kind) {
  int i;
  if (!strcmp(kind, "MADK")) return true;
  for (i = 0; i < lib->TotalPlug; i++)
    if (!strcmp(kind, lib->ThePlug[i].type)) return true;
  return false;
}

MADErr PPExportFile(MADLibrary* lib, char* kind, char* file, MADMusic* music) {
  (void)lib; (void)kind; (void)file; (void)music;
  return MADCannotFindPlug;
}

MADErr PPTestFile(MADLibrary* lib, char* kind, char* file) {
  int i;
  MADMusic music;
  MADInfoRec info;
  for (i = 0; i < lib->TotalPlug; i++)
    if (!strcmp(kind, lib->ThePlug[i].type)) return call(lib, i, MADPlugTest, file, &music, &info);
  return MADCannotFindPlug;
}

MADFourChar GetPPPlugType(MADLibrary* lib, short id, MADFourChar mode) {
  (void)lib; (void)id; (void)mode;
  return '!!!!';
}

/* ---- the engine ---- */

static MADLibrary* g_lib;
static MADDriverRec* g_driver;
static MADMusic* g_music;
static char* g_block;          /* one block of the driver's output */
static int g_block_size;       /* frames the driver writes at once */
static int g_block_frames, g_block_at;
static int g_ended;

/* A pattern packed as "MAD1" by MAD-G, as the importer unpacks it: for each of its
 * rows x tracks commands a byte 0 to 3, then 0, 2, 2 or 4 bytes. The importer reads
 * on past the pattern's end when the commands take more bytes than there are, and
 * gives up (and then reads through a null pointer) at any other byte. The count of
 * commands is a short in the importer. 1 when the pattern unpacks. */
static int unpacks_mad1(const uint8_t* p, size_t bytes, size_t commands) {
  static const size_t kMore[4] = {0, 2, 2, 4};
  size_t at = 0, i;
  if (commands > 32767) return 0;
  for (i = 0; i < commands; ++i) {
    if (at >= bytes || p[at] > 3) return 0;
    at += 1 + kMore[p[at]];
    if (at > bytes) return 0;
  }
  return 1;
}

/* PlayerPRO's importer of MAD-F and MAD-G reads what the header promises without
 * looking at the file's size. 1 when the file is as long as its header says. */
static int whole_madfg(const uint8_t* d, size_t size) {
  const size_t header = 36 + 64 * 52 + 131 + 1;    /* oldMADSpec, packed to words */
  size_t at = header, i;
  unsigned patterns, tracks;
  if (size < header) return 0;
  patterns = d[36 + 64 * 52];
  tracks = d[36 + 64 * 52 + 130];
  if (tracks < 1 || tracks > 32) return 0;
  for (i = 0; i < patterns; ++i) {
    if (d[3] == 'F') {
      at += (size_t)tracks * 64 * 4;
    } else {
      uint32_t rows, bytes;
      if (at + 36 > size) return 0;
      rows = (uint32_t)d[at] << 24 | d[at + 1] << 16 | d[at + 2] << 8 | d[at + 3];
      bytes = (uint32_t)d[at + 28] << 24 | d[at + 29] << 16 | d[at + 30] << 8 | d[at + 31];
      if (rows < 1 || rows > 1024 || bytes > size) return 0;
      if (memcmp(d + at + 4, "MAD1", 4) == 0 &&
          (at + 36 + (size_t)bytes > size || !unpacks_mad1(d + at + 36, bytes, (size_t)tracks * rows)))
        return 0;
      at += 36 + (memcmp(d + at + 4, "MAD1", 4) == 0 ? bytes : (size_t)tracks * rows * 4);
    }
    if (at > size) return 0;
  }
  for (i = 0; i < 64; ++i) {
    const uint8_t* s = d + 36 + 52 * i + 32;
    const uint32_t bytes = (uint32_t)s[0] << 24 | s[1] << 16 | s[2] << 8 | s[3];
    /* the loop, start and length, which the driver plays as they are: a loop
     * past the sample's end was read past it */
    const uint32_t loop = (uint32_t)s[12] << 24 | s[13] << 16 | s[14] << 8 | s[15];
    const uint32_t loop_length = (uint32_t)s[16] << 24 | s[17] << 16 | s[18] << 8 | s[19];
    if (bytes > size || at + bytes > size) return 0;
    if (bytes > 0 && (loop > bytes || loop_length > bytes - loop)) return 0;
    at += bytes;
  }
  return 1;
}

static int fail(pc_info* info, const char* what, MADErr err) {
  snprintf(info->error, sizeof(info->error), "%s (PlayerPRO error %d)", what, (int)err);
  pcw_playerpro_close();
  return 0;
}

int pcw_playerpro_open(const char* path, const uint8_t* data, size_t size, int track,
                       pc_info* info) {
  MADDriverSettings init;
  MADErr err;
  char type[8] = {0};
  long full = 0, now = 0;
  int i;
  if (size < 100 || memcmp(data, "MAD", 3) != 0 || !strchr("FGHIK", data[3])) {
    snprintf(info->error, sizeof(info->error), "not a PlayerPRO MAD module");
    return 0;
  }
  if (track != 0) {
    snprintf(info->error, sizeof(info->error), "no such track");
    return 0;
  }
  if ((data[3] == 'F' || data[3] == 'G') && !whole_madfg(data, size)) {
    snprintf(info->error, sizeof(info->error), "the MAD module is cut short or damaged");
    return 0;
  }
  if (info->rate > 48000) info->rate = 48000;     /* the driver's limits */
  if (info->rate < 5000) info->rate = 44100;

  /* PlayerPRO's own defaults (MADGetBestDriver), but for the end of the song */
  memset(&init, 0, sizeof(init));
  MADGetBestDriver(&init);
  init.outPutRate = (unsigned)info->rate;
  init.driverMode = NoHardwareDriver;
  init.repeatMusic = false;

  if ((err = MADInitLibrary(NULL, &g_lib)) != MADNoErr) return fail(info, "PlayerPRO doesn't start", err);
  if ((err = MADCreateDriver(&init, g_lib, &g_driver)) != MADNoErr)
    return fail(info, "PlayerPRO's driver doesn't start", err);
  if ((err = MADMusicIdentifyCString(g_lib, type, (char*)path)) != MADNoErr)
    return fail(info, "PlayerPRO doesn't know this file", err);
  if ((err = MADLoadMusicFileCString(g_lib, &g_music, type, (char*)path)) != MADNoErr)
    return fail(info, "PlayerPRO can't load the file", err);
  if ((err = MADAttachDriverToMusic(g_driver, g_music, NULL)) != MADNoErr)
    return fail(info, "PlayerPRO can't play the file", err);
  MADStartDriver(g_driver);
  MADPlayMusic(g_driver);
  MADGetMusicStatus(g_driver, &full, &now);

  info->tracks = 1;
  info->duration_ms = full > 0 ? full * 1000 / 60 : 0;
  snprintf(info->type, sizeof(info->type), "PlayerPRO %.4s", (const char*)data);
  if (g_music->header) {
    snprintf(info->title, sizeof(info->title), "%.*s", (int)sizeof(g_music->header->name),
             g_music->header->name);
    /* the importers of the old formats put their own name there */
    if (strncmp(g_music->header->infos, "Converted by PlayerPRO", 22) != 0)
      snprintf(info->comment, sizeof(info->comment), "%.*s", (int)sizeof(g_music->header->infos),
               g_music->header->infos);
  }
  if (g_music->fid) {
    int last = 0;
    for (i = 0; i < MAXINSTRU && i < PC_MAX_NAMES; ++i)
      if (g_music->fid[i].numSamples > 0 || g_music->fid[i].name[0]) last = i + 1;
    for (i = 0; i < last; ++i)
      snprintf(info->names[i], sizeof(info->names[i]), "%.*s", (int)sizeof(g_music->fid[i].name),
               g_music->fid[i].name);
    info->n_names = last;
  }
  g_block_size = MADAudioLength(g_driver);      /* in frames, whatever its name says */
  if (g_block_size <= 0) return fail(info, "PlayerPRO's driver has no buffer", MADParametersErr);
  g_block = (char*)calloc((size_t)g_block_size + 64, 4);
  g_block_frames = g_block_at = 0;  /* empty */
  g_ended = 0;
  return 1;
}

int pcw_playerpro_render(float* out, int frames) {
  int done = 0;
  while (done < frames) {
    if (g_block_at >= g_block_frames) {
      if (g_ended || !g_driver) break;
      memset(g_block, 0, (size_t)g_block_size * 4);
      if (!MADDirectSave(g_block, NULL, g_driver)) g_ended = 1;   /* this block is the last */
      g_block_frames = MADAudioLength(g_driver);    /* the last block is shorter */
      if (g_block_frames > g_block_size) g_block_frames = g_block_size;
      if (g_block_frames < 0) g_block_frames = 0;
      g_block_at = 0;
    }
    {
      const int16_t* in = (const int16_t*)g_block + g_block_at * 2;
      int n = g_block_frames - g_block_at, i;
      if (n > frames - done) n = frames - done;
      for (i = 0; i < n * 2; ++i) out[done * 2 + i] = in[i] / 32768.0f;
      done += n;
      g_block_at += n;
    }
  }
  return done;
}

void pcw_playerpro_close(void) {
  if (g_driver) {
    MADStopMusic(g_driver);
    MADStopDriver(g_driver);
  }
  if (g_music) MADDisposeMusic(&g_music, g_driver);
  if (g_driver) MADDisposeDriver(g_driver);
  if (g_lib) MADDisposeLibrary(g_lib);
  g_music = NULL;
  g_driver = NULL;
  g_lib = NULL;
  free(g_block);
  g_block = NULL;
}
