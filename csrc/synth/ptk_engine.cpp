// SPDX-License-Identifier: GPL-3.0-or-later
// ProTrekkr / NoiseTrekker (.ptk, .ntk) through ProTrekkr's own replay routine, as
// rePlayer has it (source/Replays/ProTrekkr/protrekkr: the tracker's loader and
// replayer without its user interface, reading the song from memory).

#include "engine.h"

#include <include/version.h>
#include <replay/include/replay.h>
#include <files/include/files.h>

#include <windows.h>   // ptk_win/windows.h: the ACM calls of samples_unpack.cpp

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <vector>

// ---- what the tracker's user interface would have defined ----

int AUDIO_Play_Flag;
char artist[20];
char style[20];
char SampleName[128][16][64];
int Midiprg[128];
char nameins[128][20];
int Chan_Midi_Prg[MAX_TRACKS];
char Chan_History_State[256][MAX_TRACKS];
int done = 0;          // the replayer sets it when the song has played through

extern Uint8* Mod_Memory;
extern Synth_Parameters PARASynth[128];
extern char replayerPtkName[20];
int Calc_Length(void);
Uint32 STDCALL Mixer(Uint8* Buffer, Uint32 Len);
int Alloc_Patterns_Pool(void);

void Set_Default_Channels_Polyphony(void) {
  for (int i = 0; i < MAX_TRACKS; i++) Channels_Polyphony[i] = DEFAULT_POLYPHONY;
}

// ---- samples packed with a Windows codec ----
//
// ProTrekkr on Windows can pack samples as GSM 6.10, MP3 or ATRAC3 with the codecs
// of the Audio Compression Manager. Those are not decoded here: a song that has
// such a sample is refused, because its samples would come out as noise.

static int g_acm_used;      // the wFormatTag of the first such sample, 0 when none

struct ptk_acm_stream { int tag; };
static ptk_acm_stream g_stream;

MMRESULT acmFormatSuggest(void*, LPWAVEFORMATEX, LPWAVEFORMATEX, DWORD, DWORD) { return 0; }

MMRESULT acmStreamOpen(HACMSTREAM* stream, void*, LPWAVEFORMATEX src, LPWAVEFORMATEX,
                       void*, DWORD_PTR, DWORD_PTR, DWORD) {
  g_stream.tag = src->wFormatTag;
  if (!g_acm_used) g_acm_used = src->wFormatTag;
  *stream = &g_stream;
  return 0;
}

MMRESULT acmStreamSize(HACMSTREAM, DWORD input, DWORD* output, DWORD) {
  *output = input * 32 + 65536;
  return 0;
}

MMRESULT acmStreamPrepareHeader(HACMSTREAM, ACMSTREAMHEADER*, DWORD) { return 0; }
MMRESULT acmStreamConvert(HACMSTREAM, ACMSTREAMHEADER* header, DWORD) {
  header->cbDstLengthUsed = 0;
  return 0;
}
MMRESULT acmStreamUnprepareHeader(HACMSTREAM, ACMSTREAMHEADER*, DWORD) { return 0; }
MMRESULT acmStreamClose(HACMSTREAM, DWORD) { return 0; }

// ---- how long the song is ----
//
// ProTrekkr's own Calc_Length() leaves out the pattern loops (effect 06): it comes
// out short for songs that have them. This walks through the song as the replayer
// does (Sp_Player in replay.cpp: the effects of a row that change the flow or the
// tempo, then the step to the next row) and adds up the rows. In frames at
// MIX_RATE; 0 when the song doesn't come to an end.

static long long song_frames() {
  int bpm = Beats_Per_Min, tpb = Ticks_Per_Beat, shuffle = shuffle_amount;
  if (bpm < 1 || tpb < 1) return 0;
  int per_row = (60 * MIX_RATE) / (bpm * tpb);
  int shuffle_switch = -1;
  auto shuffle_step = [&] {
    return shuffle_switch == 1 ? -((per_row * shuffle) / 200) : (per_row * shuffle) / 200;
  };
  int step = shuffle_step();
  int position = 0, line = 0, jump = -1;
  int loop_pos = 0, loop_counter = 0, loop_in = 0;
  long long total = 0;
  const long long longest = 4LL * 3600 * MIX_RATE;
  const int tracks = Song_Tracks > MAX_TRACKS ? MAX_TRACKS : Song_Tracks;

  for (long rows = 0; rows < 4000000 && total < longest; ++rows) {
    if (line < 0 || line >= MAX_ROWS) return 0;
    const Uint8* row = RawPatterns + pSequence[position] * PATTERN_LEN + line * PATTERN_ROW_LEN;
    bool stopped = false;
    for (int t = 0; t < tracks; ++t) {
      const Uint8* cell = row + t * PATTERN_BYTES;
      for (int j = 0; j < Channels_Effects[t] && j < MAX_FX; ++j) {
        const int fx = cell[PATTERN_FX + j * 2], data = cell[PATTERN_FXDATA + j * 2];
        if (fx == 0x25) {
          shuffle = static_cast<int>(static_cast<float>(data) * 0.39216f);
          step = shuffle_step();
        } else if (fx == 0xf0) {
          if (data >= 20) {
            bpm = data;
            per_row = (60 * MIX_RATE) / (bpm * tpb);
          }
        } else if (fx == 0x0f) {
          if (data == 0) { stopped = true; continue; }
          tpb = data > 32 ? 32 : data;
          per_row = (60 * MIX_RATE) / (bpm * tpb);
          step = shuffle_step();
        }
      }
    }
    if (stopped) return total + 1;
    int pattern_break = 255;
    for (int t = 0; t < tracks; ++t) {
      if (!Chan_Active_State[position][t]) continue;
      const Uint8* cell = row + t * PATTERN_BYTES;
      const int columns = Channels_Effects[t] < MAX_FX ? Channels_Effects[t] : MAX_FX;
      for (int j = 0; j < columns; ++j)
        if (cell[PATTERN_FX + j * 2] == 0xd && cell[PATTERN_FXDATA + j * 2] < MAX_ROWS)
          pattern_break = cell[PATTERN_FXDATA + j * 2];
      for (int j = 0; j < columns; ++j)
        if (cell[PATTERN_FX + j * 2] == 0x1f) {
          if (pattern_break >= MAX_ROWS) pattern_break = 0;
          jump = cell[PATTERN_FXDATA + j * 2];
        }
      for (int j = 0; j < columns; ++j) {
        if (cell[PATTERN_FX + j * 2] != 0x6) continue;
        const int data = cell[PATTERN_FXDATA + j * 2];
        if (!data) {
          if (loop_counter == 0) loop_pos = line;
        } else if (loop_pos != -1) {
          if (loop_counter == 0) {
            loop_counter = data;
            loop_pos = line - loop_pos;
            loop_in = 1;
          } else if (--loop_counter == 0) {
            loop_pos = -1;
            loop_in = 0;
          } else {
            loop_in = 1;
          }
        }
      }
    }
    total += per_row + step + 1;

    // the step to the next row
    shuffle_switch = -shuffle_switch;
    step = shuffle_step();
    bool ended = false;
    if (loop_in) {
      line -= loop_pos;
      if (line < 0) line = 0;
      loop_in = 0;
      continue;
    }
    if (pattern_break > 127) {
      ++line;
    } else {
      if (jump >= 0) {
        if (jump <= position) ended = true;
        position = jump;
        jump = -1;
      } else {
        ++position;
      }
      line = pattern_break;
      if (position >= Song_Length) { position = 0; ended = true; }
      loop_pos = loop_counter = loop_in = 0;
    }
    if (line == patternLines[pSequence[position]]) {
      if (jump > 0) {
        if (jump <= position) ended = true;
        position = jump;
        jump = -1;
      } else {
        ++position;
      }
      line = 0;
      if (position >= Song_Length) { position = 0; ended = true; }
      loop_pos = loop_counter = loop_in = 0;
    }
    if (ended) return total;
  }
  return 0;
}

// ---- the engine ----

static bool g_open, g_ended;

// ---- the loader's reading, bounded ----
//
// From version 9 on the loader unpacks the song into memory (Depack_Data) and reads
// it from there without looking at its end; tools/build-synth.sh points those
// reads here. What is past the end reads as zeros.

extern int Mod_Mem_Pos;
static long g_mod_limit;        // bytes of the unpacked song the loader has
static bool g_damaged;          // a sample is longer than what is left of the file

void modland_read_mem(void* to, long bytes) {
  if (bytes <= 0) return;
  long have = 0;
  if (Mod_Memory && Mod_Mem_Pos >= 0 && Mod_Mem_Pos < g_mod_limit)
    have = std::min<long>(bytes, g_mod_limit - Mod_Mem_Pos);
  if (have > 0) std::memcpy(to, Mod_Memory + Mod_Mem_Pos, static_cast<size_t>(have));
  if (have < bytes) std::memset(static_cast<char*>(to) + have, 0, static_cast<size_t>(bytes - have));
}

// Whether a sample of `length` 16-bit frames is there to be read. Its length is a
// 32-bit number of the file, of which the loader allocates and clears the memory.
int modland_sample_fits(long length, ReplayerFile& in) {
  const long left = Mod_Memory ? g_mod_limit - Mod_Mem_Pos
                               : static_cast<long>(in.maxSize) - static_cast<long>(in.pos);
  if (length >= 0 && length <= left / 2) return 1;
  g_damaged = true;
  return 0;
}

// The unpacked song may be no larger than this. The largest of the 67 songs on
// Modland unpacks to 9.5 MB (118 times its file); zlib packs up to 1032 to 1.
static const unsigned long long kMostUnpacked = 64ull << 20;

// From version 9 on a song is packed with zlib behind its first nine bytes, from
// version B on with its unpacked size in front. The loader unpacks without looking
// at what zlib says, and waits for ever when the data is damaged; so it is unpacked
// here first, which also checks zlib's checksum of the song.
static bool unpacks(const uint8_t* data, size_t size, char version) {
  const bool sized = version >= 'B';
  const size_t start = sized ? 13 : 9;
  if (size <= start + 8) return false;
  uint32_t expected = 0;
  if (sized) std::memcpy(&expected, data + 9, 4);
  if (sized && (expected < 1024 || expected > kMostUnpacked)) return false;
  z_stream z;
  std::memset(&z, 0, sizeof(z));
  if (inflateInit(&z) != Z_OK) return false;
  std::vector<uint8_t> out(1 << 16);
  z.next_in = const_cast<Bytef*>(data + start);
  z.avail_in = static_cast<uInt>(size - start);
  int result = Z_OK;
  unsigned long long total = 0;
  while (result == Z_OK) {
    z.next_out = out.data();
    z.avail_out = static_cast<uInt>(out.size());
    result = inflate(&z, Z_NO_FLUSH);
    total += out.size() - z.avail_out;
    if (total > kMostUnpacked) break;
  }
  inflateEnd(&z);
  if (result != Z_STREAM_END) return false;
  if (sized && total != expected) return false;
  // what Depack_Data will have: it unpacks into a buffer of the size the file says,
  // or ten times the packed size in versions 9 and A
  g_mod_limit = static_cast<long>(sized ? expected : std::min<unsigned long long>(total, (size - 9) * 10ull));
  return true;
}

// What the replayer takes from the loaded song as it is, looked at: the tempo it
// divides by, the patterns the sequence names, their lengths in rows. An
// instrument number of a note that no instrument has (128 to 254; 255 is none)
// would be looked up past the tables of instruments: such notes lose it.
static bool plausible_song() {
  if (Beats_Per_Min < 1 || Beats_Per_Min > 999 || Ticks_Per_Beat < 1 || Ticks_Per_Beat > 32) return false;
  if (Song_Length < 1 || Song_Length > MAX_SEQUENCES) return false;
  for (int i = 0; i < Song_Length; ++i) {
    const int p = pSequence[i];
    if (p < 0 || p >= MAX_PATTERNS || patternLines[p] < 1 || patternLines[p] > MAX_ROWS) return false;
  }
  for (int p = 0; p < MAX_PATTERNS; ++p)
    for (int row = 0; row < MAX_ROWS; ++row)
      for (int t = 0; t < MAX_TRACKS; ++t) {
        Uint8* cell = RawPatterns + p * PATTERN_LEN + row * PATTERN_ROW_LEN + t * PATTERN_BYTES;
        for (int i = 0; i < MAX_POLYPHONY; ++i)
          if (cell[PATTERN_INSTR1 + i * 2] >= MAX_INSTRS && cell[PATTERN_INSTR1 + i * 2] != 255)
            cell[PATTERN_INSTR1 + i * 2] = 255;
      }
  return true;
}

static void copy_text(char* to, size_t size, const char* from, size_t length) {
  size_t n = 0;
  while (n < length && n + 1 < size && from[n]) { to[n] = from[n]; ++n; }
  while (n && (to[n - 1] == ' ' || to[n - 1] == '\r' || to[n - 1] == '\n')) --n;
  to[n] = 0;
}

// ---- NoiseTrekker's songs ----
//
// NoiseTrekker's songs come out far too loud through ProTrekkr's replayer: their
// synthesizer instruments, and the master volume the songs were made with, drive
// the mix 2 to 8 times over full scale, and ProTrekkr's mixer clips at full scale
// (up to 14 % of the samples of a song). How loud NoiseTrekker itself played them
// is not known: its source isn't to be found, and ProTrekkr's author says of the
// three NoiseTrekker 1 songs he took over that they "really don't sound like in
// the original one as i had to rework them". So:
//
// - NoiseTrekker 1 has no volume for a synthesizer instrument, and the loader gives
//   them all 127, full volume. The loader's own value for an instrument of
//   NoiseTrekker 2 that has none is 64, as is the tracker's for a synth preset
//   loaded without one; that is taken here too.
// - The master volume of every NoiseTrekker song is taken down by 14 dB, so that
//   11 of the 12 songs on Modland stay under full scale (the 12th, robot-ballet,
//   has a few spikes 8 times over it). The player evens out the loudness of songs
//   afterwards, so only the clipping matters, not the level.
static const float kNoiseTrekkerMaster = 0.2f;

static void quieter_noisetrekker(char version) {
  if (version == '1')
    for (int i = 0; i < 128; ++i) PARASynth[i].glb_volume = 64;
  mas_vol *= kNoiseTrekkerMaster;
}

extern "C" int synthw_protrekkr_open(const char*, const uint8_t* data, size_t size, int track,
                                      synth_info* info) {
  if (size < 64 || (std::memcmp(data, "PROTREK", 7) != 0 && std::memcmp(data, "TWNNSNG", 7) != 0)) {
    std::snprintf(info->error, sizeof(info->error), "not a ProTrekkr or NoiseTrekker song");
    return 0;
  }
  // "TWNNSNG" is NoiseTrekker's signature, which ProTrekkr 1 kept; the character
  // after it counts the versions of the format: 1 and 2 are NoiseTrekker's
  const char version = static_cast<char>(data[7]);
  const bool ntk = version == '1' || version == '2';
  if (!((version >= '1' && version <= '9') || (version >= 'A' && version <= 'V'))) {
    std::snprintf(info->error, sizeof(info->error), "a version of the format this replayer doesn't know");
    return 0;
  }
  if (track != 0) {
    std::snprintf(info->error, sizeof(info->error), "the song has one track");
    return 0;
  }
  if ((version == '9' || version >= 'A') && !unpacks(data, size, version)) {
    std::snprintf(info->error, sizeof(info->error), "the song is damaged or cut short");
    return 0;
  }
  Ptk_InitDriver();
  if (!Alloc_Patterns_Pool()) {
    std::snprintf(info->error, sizeof(info->error), "out of memory");
    return 0;
  }
  g_acm_used = 0;
  g_damaged = false;
  if (!Load_Ptk({data, size})) {
    std::snprintf(info->error, sizeof(info->error), "the song can't be loaded");
    return 0;
  }
  if (g_damaged || !plausible_song()) {
    std::snprintf(info->error, sizeof(info->error), "the song is damaged");
    return 0;
  }
  if (g_acm_used) {
    const char* codec = g_acm_used == WAVE_FORMAT_GSM610 ? "GSM" :
                        g_acm_used == WAVE_FORMAT_MPEGLAYER3 ? "MP3" : "ATRAC3";
    std::snprintf(info->error, sizeof(info->error),
                  "its samples are packed with a Windows codec (%s)", codec);
    return 0;
  }
  if (Song_Length <= 0) {
    std::snprintf(info->error, sizeof(info->error), "the song is empty");
    return 0;
  }
  info->rate = MIX_RATE;
  info->tracks = 1;
  const long long frames = song_frames();
  info->duration_ms = frames > 0 ? static_cast<long>(frames * 1000 / MIX_RATE) : Calc_Length();
  copy_text(info->title, sizeof(info->title), replayerPtkName, sizeof(replayerPtkName));
  std::snprintf(info->type, sizeof(info->type), "%s", ntk ? "NoiseTrekker" : "ProTrekkr");
  for (int i = 0; i < 128 && info->n_names < SYNTH_MAX_NAMES; ++i) {
    bool used = false;
    for (int k = 0; k < MAX_INSTRS_SPLITS; ++k) used = used || SampleType[i][k];
    used = used || Synthprg[i];
    if (!used && (!nameins[i][0] || std::strncmp(nameins[i], "Untitled", 8) == 0)) continue;
    copy_text(info->names[info->n_names++], sizeof(info->names[0]), nameins[i], sizeof(nameins[i]));
  }
  if (ntk) quieter_noisetrekker(version);
  done = 0;
  Ptk_Play();
  g_open = true;
  g_ended = false;
  return 1;
}

extern "C" int synthw_protrekkr_render(float* out, int frames) {
  if (!g_open || g_ended) return 0;
  const int got = static_cast<int>(Mixer(reinterpret_cast<Uint8*>(out), frames));
  if (got < frames) g_ended = true;
  return got;
}

extern "C" void synthw_protrekkr_close(void) { g_open = false; }
