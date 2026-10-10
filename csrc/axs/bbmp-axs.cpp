// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-axs: play a song of AXS (.axs), as raw PCM on stdout.
//
//   bbmp-axs [--clean] <file> <track> <samplerate> [start-ms]
//   bbmp-axs [--clean] --ref <file>    the song at 44100 Hz, no header line, in whole
//                                      blocks of 576 frames, as AXS's Winamp plug-in
//                                      gives it
//
// AXS (Resolution Audio, 1998-2001) is a Windows synthesizer and tracker: seven
// two-oscillator synthesizers and a drum sampler, a pool of 64 voices, a tempo-synced
// delay, a reverb and a tracker of up to 16 tracks. This is a new player, written for
// bbmp from a description of how AXS's Winamp plug-in sounds (a clean room: its code was
// not looked at), and checked against that plug-in's output sample for sample.
//
// By default it sounds exactly as the plug-in, quirk included: the plug-in's smooth
// triangle wave starts its averaging from a leftover number (the oscillator's memory
// address), which adds a small constant offset to that wave. --clean leaves it out.
//
// The engine runs at 44100 Hz; other rates are resampled. A song plays once, to the end
// of the 576-frame block in which it ends; a song that never ends (it jumps back for
// ever) stops at the length AXS itself reports for it.
//
// Same protocol as the other helpers: one JSON line describing the track, then signed
// 16-bit little-endian stereo until the song ends. A damaged file is refused with the
// reason; nothing in it is trusted.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "axs_tables.h"
#include "resample.h"

namespace {

using namespace axs_tables;

const int RATE = 44100;
const int BLOCK = 576;                  // the plug-in's output block, in frames
const size_t MAX_FILE = 64u << 20;      // the largest known song is 4.4 MB
const long long MAX_SECONDS = 60 * 60;  // a song still playing after this never ends
const int VOICES = 64;
const int32_t MAX_SAMPLE = 0x7FF00000;  // a sample's length, at most

// ---- small arithmetic helpers (the engine's fixed point: 2^24 stands for 1.0)

inline int32_t mulShift(int32_t a, int32_t b, int n) {
  return int32_t(uint32_t(uint64_t((int64_t(a) * int64_t(b)) >> n)));
}
inline int32_t P24(int32_t a, int32_t b) { return mulShift(a, b, 24); }
inline int32_t P23(int32_t a, int32_t b) { return mulShift(a, b, 23); }
inline int32_t P30(int32_t a, int32_t b) { return mulShift(a, b, 30); }
inline int32_t P7(int32_t a, int32_t b) { return mulShift(a, b, 7); }
inline uint32_t UHI(uint32_t a, uint32_t b) { return uint32_t((uint64_t(a) * b) >> 32); }
inline int32_t add(int32_t a, int32_t b) { return int32_t(uint32_t(a) + uint32_t(b)); }
inline int32_t sub(int32_t a, int32_t b) { return int32_t(uint32_t(a) - uint32_t(b)); }
inline int32_t shl(int32_t a, int n) { return int32_t(uint32_t(a) << n); }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
// An index into a table of 128 (the songs stay inside; damaged ones are held to it)
inline int ix(long long i) { return i < 0 ? 0 : i > 127 ? 127 : int(i); }
inline float single(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
// Truncate toward zero; out of range or not a number gives -2^31 (as the x87 unit does)
inline int32_t trunc32(double x) {
  if (!(x > -2147483649.0 && x < 2147483648.0)) return INT32_MIN;
  return int32_t(x);
}
// Round to the nearest integer, ties to even; out of range or not a number gives -2^31
// (what the x87 unit's integer store gives)
inline int32_t rintq(double r) {
  const double n = std::nearbyint(r);
  if (!(n >= -2147483648.0 && n <= 2147483647.0)) return INT32_MIN;
  return int32_t(n);
}

// ---- the file

struct Track {
  std::vector<uint8_t> rows;    // 4 bytes a row
};

struct Block {
  int tracks = 0, rows = 0;
  uint8_t trackByte[16] = {};
  Track track[16];
  int filled = 0;
  const uint8_t* row(int t, int r) const { return &track[t].rows[size_t(r) * 4]; }
};

struct DrumNote {
  int32_t pitch = 64, attack = 0, decay = 127, send = 5, amplitude = 64, sample = 0;
  int32_t filter = 0, flags = 0x41, cutoff = 127, resonance = 0;
};

struct Sample {
  std::vector<int16_t> data;    // what the file holds of it
  int32_t length = 0, rate = 0; // the length it says it has
  int32_t at(uint32_t j) const { return j < data.size() ? data[j] : 0; }
};

struct Song {
  uint8_t header[20] = {};
  bool shortHeader = false;
  int length = 0;
  std::vector<uint32_t> posBlock, posMask;
  std::vector<Block> blocks;
  uint8_t inst[8][128] = {};
  bool haveInst[8] = {};
  std::string names[8];
  DrumNote drum[128];
  std::map<int32_t, Sample> samples;
  std::string user;
};

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

// The reader is a small state machine that reads fixed amounts for the chunks it
// knows, as AXS does. A file cut short in its last chunk is read as far as it goes (the
// missing bytes count as zero), as AXS reads it: two of the known songs end so. Returns
// "" or the reason the file can't be played.
std::string load(const std::vector<uint8_t>& f, Song& s) {
  const size_t size = f.size();
  if (size < 12 || std::memcmp(f.data(), "FORM", 4) || std::memcmp(f.data() + 8, "AXSF", 4))
    return "not an AXS song";
  size_t pos = 8;
  int state = 0, instNo = 0, nameNo = 0;
  bool ended = false;
  std::vector<uint8_t> scratch;
  // n bytes at pos (zeros past the end of the file, which then ends the reading)
  auto get = [&](size_t n) -> const uint8_t* {
    if (size - pos >= n) {
      const uint8_t* p = &f[pos];
      pos += n;
      return p;
    }
    scratch.assign(n, 0);
    std::memcpy(scratch.data(), &f[pos], size - pos);
    pos = size;
    ended = true;
    return scratch.data();
  };
  while (!ended && size - pos >= 4) {
    const uint8_t* id = &f[pos];
    pos += 4;
    auto is = [&](const char* name) { return !std::memcmp(id, name, 4); };
    if (is("AXSF")) {
      state = 1;
      continue;
    }
    const bool known = is("SHDR") || is("SONG") || is("BLOK") || is("TRAK") || is("INST") ||
                       is("INNM") || is("DNOT") || is("SAMP");
    const bool right = is("SHDR") ? state == 1 : is("SONG") ? state == 2 : state == 3;
    if (known && !right) continue;    // its size is then read as the next id
    const uint32_t chunk = be32(get(4));
    if (ended) break;
    if (is("SHDR")) {
      const size_t n = chunk == 19 ? 19 : 20;
      const uint8_t* p = get(20);
      std::memcpy(s.header, p, n);
      s.shortHeader = n == 19;
      s.length = s.header[11];
      state = 2;
    } else if (is("SONG")) {
      if (s.length == 0) return "the song has no positions";
      const uint8_t* p = get(size_t(s.length) * 8);
      for (int i = 0; i < s.length; ++i) {
        s.posBlock.push_back(le32(p + 8 * i));
        s.posMask.push_back(le32(p + 8 * i + 4));
      }
      state = 3;
    } else if (is("BLOK")) {
      const uint8_t* p = get(24);
      if (s.blocks.size() < 128) {
        Block b;
        const int32_t t = int32_t(le32(p)), r = int32_t(le32(p + 4));
        b.tracks = clampi(t, 0, 16);
        b.rows = clampi(r, 0, 256);
        std::memcpy(b.trackByte, p + 8, 16);
        for (auto& tr : b.track) tr.rows.assign(size_t(b.rows ? b.rows : 1) * 4, 0);
        for (int i = 0; i < b.tracks; ++i)
          if ((b.trackByte[i] & 15) > 7)
            return "a track plays instrument " + std::to_string(b.trackByte[i] & 15) +
                   "; AXS has 8 (0-7)";
        s.blocks.push_back(std::move(b));
      }
    } else if (is("TRAK")) {
      if (s.blocks.empty()) return "a track comes before any block";
      Block& b = s.blocks.back();
      const size_t n = size_t(b.rows) * 4;
      const uint8_t* p = get(n);
      if (b.filled < 16 && n) std::memcpy(b.track[b.filled].rows.data(), p, n);
      if (b.filled < 16) b.filled++;
    } else if (is("INST")) {
      const uint8_t* p = get(128);
      if (instNo < 8) {
        std::memcpy(s.inst[instNo], p, 128);
        s.haveInst[instNo++] = true;
      }
    } else if (is("INNM")) {
      const char* c = reinterpret_cast<const char*>(get(9));
      if (nameNo < 8) s.names[nameNo++] = std::string(c, strnlen(c, 9));
    } else if (is("DNOT")) {
      const uint8_t* p = get(40);
      const int32_t n = int32_t(le32(p));
      if (n < 0 || n > 127) return "a drum note numbered " + std::to_string(n) + " (0-127 exist)";
      int32_t v[9];
      for (int i = 0; i < 9; ++i) v[i] = int32_t(le32(p + 4 + 4 * i));
      DrumNote& d = s.drum[n];
      d.pitch = v[0];
      d.attack = v[1];
      d.decay = v[2];
      d.send = v[3];
      d.amplitude = v[4];
      d.sample = v[5];
      d.filter = std::min<int32_t>(v[6] & 0xFFFF, 5);
      d.flags = (uint32_t(v[6]) >> 16) & 0xF1;
      d.cutoff = v[7];
      d.resonance = v[8];
    } else if (is("SAMP")) {
      const uint8_t* p = get(176);
      const int32_t no = int32_t(le32(p));
      const int32_t n = int32_t(le32(p + 4 + 160));
      const int32_t rate = int32_t(le32(p + 4 + 164));
      if (n < 0 || n > MAX_SAMPLE) return "a sample of an impossible length";
      Sample& sm = s.samples[no];
      sm.rate = rate;
      sm.length = n;
      // the samples the file holds; those cut off at its end count as zero
      const size_t have = std::min(size_t(n), (size - pos) / 2);
      sm.data.resize(have);
      for (size_t i = 0; i < have; ++i) sm.data[i] = int16_t(f[pos + 2 * i] | f[pos + 2 * i + 1] << 8);
      if (have < size_t(n)) ended = true;
      pos += have * 2;
    } else {
      if (is("USER") && chunk <= 256 && size - pos >= chunk) {
        const char* c = reinterpret_cast<const char*>(&f[pos]);
        s.user = std::string(c, strnlen(c, chunk));
      }
      if (size - pos < chunk) break;    // past the end: the reading ends
      pos += chunk;
    }
  }
  if (state < 3) return "not a complete AXS song (no song header or position list)";
  if (s.blocks.empty()) return "the song has no blocks";
  for (int i = 0; i < s.length; ++i)
    if (s.posBlock[size_t(i)] >= s.blocks.size())
      return "position " + std::to_string(i) + " plays block " + std::to_string(s.posBlock[size_t(i)]) +
             ", which isn't in the file";
  return "";
}

// ---- envelopes

// The exponential envelope (amplitude and filter)
struct ExpEnv {
  int stage = 0;
  int32_t level = 0;
  uint32_t x = 0, a = 0, d = 0, r = 0;
  int32_t s = 0, A = 0, SL = 0, DS = 0, RL = 0;

  void derive() {
    SL = P24(s, A);
    DS = P24(sub(0x1000000, s), A);
  }
  void setS(int32_t v) { s = v; derive(); }
  void setA(int32_t v) { A = v; derive(); }
  void start() { stage = 5; }
  void release() {
    if (stage > 1) {
      stage = 1;
      RL = level;
      x = 0x42000000;
    }
  }
  // One step; true when a release has just ended (the voice is then free)
  bool step() {
    switch (stage) {
      case 5:
        x = 0x2000000;
        stage = 4;
        [[fallthrough]];
      case 4:
        x += UHI(x, a);
        if (x >= 0x42000000u) {
          stage = 3;
          x = 0x42000000;
          level = A;
        } else {
          level = P30(int32_t(x - 0x2000000u), A);
        }
        return false;
      case 3:
        x = UHI(x, d);
        if (x > 0x2000000u) {
          level = add(P30(int32_t(x - 0x2000000u), DS), SL);
        } else {
          stage = 2;
          if (SL > 0) level = SL;
          else stage = 0, level = 0;
        }
        return false;
      case 2:
        level = SL;
        return false;
      case 1:
        x = UHI(x, r);
        if (int32_t(x - 0x2000000u) < 0x100000) {
          stage = 0;
          level = 0;
          return true;
        }
        level = P30(int32_t(x - 0x2000000u), RL);
        return false;
      default:
        level = 0;
        return false;
    }
  }
};

int32_t slopeOf(int32_t range, int32_t t) {
  const int32_t div = int32_t((uint32_t(44100) * uint32_t(t)) >> 10);
  if (div == 0) return range;       // never with the engine's times
  if (range == INT32_MIN && div == -1) return range;
  return range / div;
}

// The linear envelope (pitch)
struct LinEnv {
  int stage = 0;
  int32_t tA = 0, tD = 0, S = 0, tR = 0, slope = 0, level = 0, amount = 0, output = 0;

  void start() { stage = 5; }
  void endOfAttack() {
    if (tD != 0) {
      slope = slopeOf(sub(0x1000000, S), tD);
      level = 0x1000000;
    } else {
      level = S;
    }
    stage = 3;
  }
  void step() {
    switch (stage) {
      case 5:
        level = 0;
        stage = 4;
        if (tA == 0) {
          endOfAttack();
          break;
        }
        slope = slopeOf(0x1000000, tA);
        [[fallthrough]];
      case 4: {
        const int32_t n = add(level, slope);
        if (n < 0x1000000) level = n;
        else endOfAttack();
        break;
      }
      case 3: {
        const int32_t n = sub(level, slope);
        if (n > S) level = n;
        else level = S, stage = 2;
        break;
      }
      case 2:
        output = P24(S, amount);
        return;
      case 1: {
        const int32_t n = sub(level, slope);
        if (n <= 0) level = 0, stage = 0;
        else level = n;
        break;
      }
      default:
        return;
    }
    output = P24(level, amount);
  }
  void release() {
    if (stage > 1) {
      slope = slopeOf(level, tR);
      if (slope == 0) slope = 1;
      stage = 1;
    }
  }
  void setA(int32_t t) {
    tA = t;
    if (stage == 4) slope = t ? slopeOf(0x1000000, t) : t;
  }
  void setD(int32_t t) {
    tD = t;
    if (stage == 3) slope = t ? slopeOf(sub(0x1000000, S), t) : t;
  }
  void setR(int32_t t) {
    tR = t;
    if (stage == 1) slope = slopeOf(level, t);
  }
};

// ---- oscillators

struct OscSet {          // what a voice copies from its instrument
  int wave = 0;
  int32_t fine = 0, coarse = 0, note = 0, pwBase = 0;
};

struct OscRun {          // what each voice keeps of its own (the output starts at 0)
  uint32_t phase = 0, inc = 0;
  int32_t y = 0, pw = 0;
};

uint32_t oscInc(int32_t extra, const OscSet& o) {
  const int32_t q = add(add(extra, o.fine), shl(add(o.note, o.coarse), 8));
  if (q < 0) return 0;
  const int i = (q >> 8) > 383 ? 383 : (q >> 8);
  const int64_t h = (int64_t(PITCH_COARSE[i]) * int64_t(PITCH_FINE[q & 255])) >> 32;
  return uint32_t(uint64_t((h * 1024) / 44100));
}

int32_t Z(uint32_t u, uint32_t w) {
  if (u < w) return int32_t(uint32_t(((uint64_t(u >> 8)) << 32) / w) - 0x800000u);
  return int32_t(uint32_t((uint64_t((0x1000000u - u) >> 8) << 32) / uint32_t(0x1000000u - w)) - 0x800000u);
}

// One step of a waveform: stores the new phase in r.phase and returns the output.
// seed: the noise generator shared by every oscillator and LFO.
int32_t oscStep(OscRun& r, int wave, uint32_t w, uint32_t addr, uint32_t& seed) {
  const uint32_t p = r.phase, i = r.inc;
  const uint32_t p2 = (p + i) & 0xFFFFFF;
  int32_t y = r.y;
  switch (wave) {
    case 0: {
      int32_t c = int32_t((p + 0x400000) & 0xFFFFFF) - 0x800000;
      if (c > 0) c = -c;
      y = 2 * c + 0x800000;
      r.phase = p2;
      break;
    }
    case 1:
      y = int32_t(p ^ 0x800000) - 0x800000;
      r.phase = p2;
      break;
    case 2:
      y = int32_t(p - w) < 0 ? 0x7FFFFF : -0x800000;
      r.phase = p2;
      break;
    case 3:
    case 9:
      r.phase = p2;
      if ((p2 ^ (p2 + i)) & 0xFF100000u) {
        seed = seed * 0x41C64E6Du + 0x3039u;
        y = int32_t(seed & 0xFFFFFF) - 0x800000;
      }
      break;
    case 4:
      r.phase = p2;
      y = Z((p2 + (w >> 1)) & 0xFFFFFF, w);
      break;
    case 6: {
      uint32_t x = (p + 0x400000) & 0xFFFFFF, sum = addr;
      for (int k = 0; k < 8; ++k) {
        int32_t c = int32_t(x) - 0x800000;
        if (c > 0) c = -c;
        sum += uint32_t(2 * c + 0x800000);
        x = (x + (i >> 3)) & 0xFFFFFF;
      }
      y = int32_t(sum) >> 3;
      r.phase = (x - 0x400000) & 0xFFFFFF;
      break;
    }
    case 7: {
      const uint32_t a = p ^ 0x800000, b = p2 ^ 0x800000;
      if (a < b) {
        y = int32_t((a + b) >> 1) - 0x800000;
      } else {
        const uint32_t s = i >> 4;
        uint32_t sum = a;
        for (uint32_t j = 1; j <= 15; ++j) sum += (a + j * s) & 0xFFFFFF;
        y = int32_t(sum >> 4) - 0x800000;
      }
      r.phase = p2;
      break;
    }
    case 8: {
      const uint32_t s = i << 4, lim = w << 8;
      uint32_t x = p << 8;
      int count = 0;
      for (int k = 0; k < 16; ++k) {
        if (x < lim) ++count;
        x += s;
      }
      r.phase = x >> 8;
      y = (count - 8) * (1 << 20);
      break;
    }
    case 10: {
      uint32_t x = p;
      int32_t sum = 0;
      for (int k = 0; k < 8; ++k) {
        sum = add(sum, Z((x + (w >> 1)) & 0xFFFFFF, w));
        x = (x + (i >> 3)) & 0xFFFFFF;
      }
      r.phase = x;
      y = sum >> 3;
      break;
    }
    default:     // 5, 11: sine
      y = SINE[p >> 10];
      r.phase = p2;
      break;
  }
  r.y = y;
  return y;
}

// ---- the filter

struct Filter {
  int32_t cutoff = 0;
  float b = 0, a1 = 0, a2 = 0;
  int32_t x1 = 0, x2 = 0, x1b = 0, x2b = 0;
  float y1 = 0, y2 = 0, y1b = 0, y2b = 0;

  void clear() {
    x1 = x2 = x1b = x2b = 0;
    y1 = y2 = y1b = y2b = 0;
  }
  void coefficients(int type, int32_t res) {
    const int32_t F = clampi(cutoff, 60, 21922);
    const double W = double(single(W0[0]));
    const double w = double(F) * W;
    if (type <= 2) {
      const int q = ix(res);
      double t = std::tan(w);
      if (type == 2) t = 1.0 / t;
      const double Q = double(single(type == 1 ? Q24[q] : Q12[q]));
      const double g = double(single(type == 1 ? GAIN24[q] : GAIN12[q]));
      const double t2 = t * t, qt = Q * t;
      const double D = (t2 + qt) + 1.0;
      a2 = float(((t2 - qt) + 1.0) / D);
      b = float((t2 / D) * g);
      if (type == 2) a1 = float(((1.0 - t2) + (1.0 - t2)) / D);
      else a1 = float(((t2 + (-1.0)) + (t2 + (-1.0))) / D);
      // where the original's tangent is one bit off the C library's
      if (F == 11025 && type == 0 && q == 77) a1 = single(0x34498AA5u);
      if (F == 11025 && type == 1 && q == 70) a1 = single(0x343C648Cu);
    } else if (type == 3) {
      const double c = std::cos(w) * (-2.0);
      const double k = 1.0 / std::tan(double(res) * W);
      const double n = 1.0 / (1.0 + k);
      a2 = float((k - 1.0) * n);
      a1 = float((k * c) * n);
      b = float(n);
    } else {
      const double c = std::cos(w) * (-2.0);
      const double t = std::tan(double(res) * W);
      const double n = 1.0 / (t + 1.0);
      a2 = float((1.0 - t) * n);
      b = float(n);
      a1 = float(c * n);
    }
  }
  int32_t run(int type, int32_t x) {
    switch (type) {
      case 0:
      case 2: {
        const int32_t s = type == 0 ? add(add(x, shl(x1, 1)), x2) : add(sub(x, shl(x1, 1)), x2);
        x2 = x1;
        x1 = x;
        const double r = (double(s) * b - double(y1) * a1) - double(y2) * a2;
        y2 = y1;
        y1 = float(r);
        return rintq(r);
      }
      case 1: {
        int32_t s = add(add(x, shl(x1, 1)), x2);
        x2 = x1;
        x1 = x;
        double r = (double(s) * b - double(y1) * a1) - double(y2) * a2;
        y2 = y1;
        y1 = float(r);
        const int32_t mid = rintq(r);
        s = add(add(mid, shl(x1b, 1)), x2b);
        x2b = x1b;
        x1b = mid;
        r = (double(s) * b - double(y1b) * a1) - double(y2b) * a2;
        y2b = y1b;
        y1b = float(r);
        return rintq(r);
      }
      case 3: {
        const int32_t s = sub(x, x2);
        x2 = x1;
        x1 = x;
        const double r = (double(s) * b - double(y1) * a1) - double(y2) * a2;
        y2 = y1;
        y1 = float(r);
        return rintq(r);
      }
      default: {
        const int32_t s = add(x, x2);
        x2 = x1;
        const double r = (double(s) * b - (double(y1) - double(x1)) * a1) - double(y2) * a2;
        x1 = x;
        y2 = y1;
        y1 = float(r);
        return rintq(r);
      }
    }
  }
};

// ---- instruments and voices

// An instrument's sound setup, which a new voice copies
struct Setup {
  OscSet osc[2];
  int32_t mixBase = 0, ringAmt = 0, driveAmt = 0, cutBase = 0, res = 0, send = 0;
  int32_t glideTime = 0, keyTrackAmt = 0;
  int ftype = 0, bus = 0, penvTargets = 0;
  bool andMode = false, driveSoft = false, keyFollow = false, sync = false;
  bool fenvVel = false, volVel = false, glideOn = false;
  ExpEnv amp, fenv;
  LinEnv penv;
};

struct Lfo {
  OscRun run;
  int shape = 0, target = 0;
  int32_t depth = 0;
  uint32_t pw = 0x1000000;
};

struct Instrument {
  uint8_t raw[128] = {};
  int32_t volume = 0;
  Setup s;
  bool mono = false, lfo1Restart = false, lfo2Restart = false;
  Lfo lfo[2];
  int32_t L1 = 0, L2 = 0;
  int held = 0, sounding = 0;
  int32_t lastNote = 0, bend = 0;
};

struct Voice {
  int inst = 0;
  int note = 0;
  bool pending = false;
  int32_t velocity = 0, Vol = 0, volStep = 0;
  uint32_t counter = 0;
  int32_t keyTrack = 0, velFactor = 0;
  int32_t glideFrom = 0, glideCount = 0, glideTotal = 0;
  OscRun osc[2];
  int32_t mix = 0;
  Filter filter;
  Setup s;
  // the drum sampler
  uint32_t pos = 0;
  int32_t inc = 0, end = 0;
  const Sample* sample = nullptr;
  bool filtered = false;
};

const int INST_LIST[53] = {5, 8, 9, 17, 12, 13, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
                           30, 31, 33, 34, 35, 80, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 50, 51,
                           52, 53, 54, 55, 65, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 90, 94};

int defaultOf(int c) {
  switch (c) {
    case 8: case 19: case 24: case 33: case 34: case 43: case 45: case 54: case 70:
    case 77: case 78: case 79: return 64;
    case 22: case 27: case 37: case 74: return 127;
    case 25: case 76: return 3;
    case 44: return 60;
    default: return 0;
  }
}

int32_t lfoInc(int v) {
  return int32_t(0x1000000u / ((uint32_t(44100) * uint32_t(LFO_PERIOD[ix(v)])) >> 10));
}

const int32_t* depthCurve(int code) {
  switch (code) {
    case 1: return DEPTH_B;
    case 2: return DEPTH_C;
    case 3: case 4: case 9: return DEPTH_D;
    default: return DEPTH_A;
  }
}

int32_t pwFor(int wave, int v) { return wave == 4 ? PW_SLOPE[ix(v)] : PW_PULSE[ix(v)]; }
int32_t lfoWidth(int shape, int v) {
  return (shape == 2 || shape == 4) ? int32_t(((13 * v) << 20) / 128 + 0x100000) : 0x1000000;
}
int32_t v127(int v) { return v == 127 ? 128 : v; }

// ---- the engine

struct TrackState {
  int inst = 0, note1 = 0;
};

class Engine {
 public:
  Engine(const Song& song, bool clean, bool dry) : song_(song), clean_(clean), dry_(dry) {
    // the start state
    for (int k = 0; k < 7; ++k)
      for (int c : INST_LIST) setCtrl(k, c, defaultOf(c));
    // the song
    const uint8_t* h = song.header;
    setTempo(h[2]);
    tpr_ = h[3];
    setCtrl(0, 15, h[4]);
    setCtrl(0, 11, h[5]);
    setCtrl(0, 16, h[6]);
    setCtrl(0, 10, song.shortHeader ? h[7] >> 1 : h[7] & 0x7F);
    setCtrl(0, 14, h[8]);
    setCtrl(0, 89, h[9]);
    for (int k = 0; k < 8; ++k) setCtrl(k, 7, 127);
    for (int k = 0; k < 8; ++k) {
      if (!song.haveInst[k]) continue;
      std::memcpy(ins_[k].raw, song.inst[k], 128);
      for (int c : INST_LIST) setCtrl(k, c, song.inst[k][c]);
    }
    // play
    block_ = song.posBlock[0];
    mask_ = song.posMask[0];
  }

  bool playing() const { return playing_; }

  // One output frame
  void frame(int16_t* out) {
    if (dry_) {
      advance();
      return;
    }
    for (int k = 0; k < 7; ++k) stepLfos(k);
    int32_t dry = 0, bus[2] = {0, 0};
    for (int j = 0; j < VOICES; ++j) {
      Voice& v = voice_[j];
      if (v.s.amp.stage == 0) continue;
      const int32_t o = v.inst == 7 ? drumSample(v) : synthSample(v, j);
      dry = add(dry, o);
      bus[v.s.bus & 1] = add(bus[v.s.bus & 1], P24(o, v.s.send));
    }
    // the delay
    const int32_t X = delayRing_[leftRead_];
    leftRead_ = (leftRead_ + 1) & 0xFFFF;
    const int32_t x = add(bus[0], P24(sub(shl(X, 1), bus[0]), feedback_));
    delayRing_[delayWrite_] = x;
    delayWrite_ = (delayWrite_ + 1) & 0xFFFF;
    // the reverb
    const int32_t R = reverb(bus[1]);
    out[0] = int16_t(clampi(add(add(R, x), dry), -0x800000, 0x7FFFFF) >> 8);
    // the right side: the dry mix negated and late, the reverb and its own delay tap
    const int32_t E = stereoRing_[stereoRead_];
    stereoRead_ = (stereoRead_ + 1) & 0xFFFF;
    stereoRing_[stereoWrite_] = sub(0, dry);
    stereoWrite_ = (stereoWrite_ + 1) & 0xFFFF;
    const int32_t X2 = delayRing_[rightRead_];
    rightRead_ = (rightRead_ + 1) & 0xFFFF;
    const int32_t x2 = add(bus[0], P24(sub(shl(X2, 1), bus[0]), feedback_));
    out[1] = int16_t(clampi(add(add(E, R), x2), -0x800000, 0x7FFFFF) >> 8);
    advance();
  }

 private:
  const Song& song_;
  bool clean_, dry_;
  Instrument ins_[8];
  Voice voice_[VOICES];
  TrackState ts_[16];
  uint32_t seed_ = 0xDEADFEED;
  // time and the sequencer
  double t_ = 0, D_ = 0;
  int bpm_ = 134, tpr_ = 6;
  int pos_ = 0, row_ = 0, tick_ = 0;
  uint32_t block_ = 0, mask_ = 0;
  int jump_ = -1;
  bool break_ = false, playing_ = true;
  // the delay
  std::vector<int32_t> delayRing_ = std::vector<int32_t>(65536, 0);
  uint32_t delayWrite_ = 1000, leftRead_ = 0, rightRead_ = 0;
  int32_t feedback_ = 0, spread_ = 0;
  int delayTime_ = 5, syncDiv_ = 1;
  bool delaySync_ = false;
  // the pseudo-stereo ring
  std::vector<int32_t> stereoRing_ = std::vector<int32_t>(65536, 0);
  uint32_t stereoRead_ = 0, stereoWrite_ = 441;
  // the reverb
  struct Ring {
    std::vector<int32_t> buf = std::vector<int32_t>(8192, 0);
    uint32_t w = 0, r = 0;
    int32_t last = 0;
    int32_t push(int32_t v) {
      buf[w] = v;
      w = (w + 1) & 8191;
      last = buf[r];
      r = (r + 1) & 8191;
      return last;
    }
  };
  Ring ringA_, ringB_, comb_[3];
  bool reverbOn_ = false;
  int32_t gA_ = 0, gB_ = 0, g_[3] = {}, damp_ = 0, z_ = 0, rs_ = 0;

  // ---- time

  void setTempo(int v) {
    bpm_ = clampi(v, 32, 255);
    D_ = 60.0 / double(bpm_ * 24);
    recomputeDelay();
  }

  void advance() {
    t_ += 1.0 / 44100;
    if (t_ > D_) {
      t_ -= D_;
      tick();
    }
  }

  // ---- delay and reverb

  void recomputeDelay() {
    static const int DIV[6] = {3, 4, 6, 8, 9, 12};
    int32_t len;
    if (delaySync_) {
      const double inv60 = 1.0 / 60, inv12 = 1.0 / 12;
      len = int32_t(((1.0 / (double(bpm_) * inv60)) * double(DIV[clampi(syncDiv_, 1, 6) - 1]) * 44100.0) * inv12);
    } else {
      len = delayTime_ * 44100 * 5 / 1000;
    }
    rightRead_ = (delayWrite_ - uint32_t(len)) & 0xFFFF;
    leftRead_ = (delayWrite_ - uint32_t(P24(add(shl(spread_, 1), 0x1000000), len))) & 0xFFFF;
  }

  static int prime(long long n) {
    if (n <= 2) return 2;
    if (n >= 8191) return 8191;
    for (int p : PRIMES)
      if (p >= n) return p;
    return 8191;
  }

  void applyReverb(int preset) {
    const int32_t* p = &REVERB_PRESETS[12 * preset];
    for (Ring* r : {&ringA_, &ringB_, &comb_[0], &comb_[1], &comb_[2]}) {
      std::fill(r->buf.begin(), r->buf.end(), 0);
      r->w = r->r = 0;
      r->last = 0;
    }
    z_ = 0;
    reverbOn_ = p[0] != 0;
    const double a = 0.27566444771 / double(p[11]);
    damp_ = int32_t(a * 16777216.0 / (1.0 / 44100 + a));
    auto len = [&](int32_t ms) { return prime((long long)ms * 44100 / 1000); };
    ringA_.r = uint32_t(-len(p[8])) & 8191;
    ringB_.r = uint32_t(-len(p[10])) & 8191;
    for (int i = 0; i < 3; ++i) {
      comb_[i].r = uint32_t(-len(p[2 + 2 * i])) & 8191;
      double g = std::pow(10.0, (double(p[2 + 2 * i]) * -3.0) / double(p[1 + 2 * i]));
      if (g > 0.9) g = 0.9;
      g_[i] = int32_t(g * 16777216.0);
    }
    gA_ = p[7];
    gB_ = p[9];
  }

  int32_t reverb(int32_t u) {
    if (!reverbOn_) return 0;
    const int32_t pA = sub(ringA_.last, P24(gA_, u));
    ringA_.push(add(u, P24(gA_, pA)));
    const int32_t pB = sub(ringB_.last, P24(gB_, pA));
    z_ = add(pB, P24(sub(z_, pB), damp_));
    ringB_.push(add(pA, P24(gB_, z_)));
    int32_t c[3];
    for (int i = 0; i < 3; ++i) c[i] = comb_[i].push(add(z_, P24(g_[i], comb_[i].last)));
    rs_ = sub(add(add(add(c[0], c[1]), c[2]) >> 5, rs_ >> 4), rs_);
    return shl(rs_, 3);
  }

  // ---- controllers

  template <class F>
  void eachSounding(int k, F fn) {
    for (Voice& v : voice_)
      if (v.inst == k && v.s.amp.stage != 0) fn(v);
  }

  void setCtrl(int k, int c, int v) {
    if (dry_) return;
    c &= 127;
    v &= 127;
    Instrument& in = ins_[k];
    in.raw[c] = uint8_t(v);
    switch (c) {      // global and shared
      case 7: in.volume = v; break;
      case 10: spread_ = v127(v) << 16; recomputeDelay(); break;
      case 11: syncDiv_ = clampi(v, 1, 6); recomputeDelay(); break;
      case 14: feedback_ = v127(v) << 16; break;
      case 15: delaySync_ = v >= 64; recomputeDelay(); break;
      case 16: delayTime_ = clampi(v, 5, 99); recomputeDelay(); break;
      case 89: applyReverb(clampi(v, 0, 15)); break;
      case 90:
        in.s.bus = v >= 64;
        eachSounding(k, [&](Voice& x) { x.s.bus = v >= 64; });
        break;
      default: break;
    }
    if (k == 7) return;
    if (c == 1) c = 74;
    Setup& s = in.s;
    switch (c) {
      case 5: s.glideTime = P24(GLIDE_TIME[v], 44100); break;
      case 8:
        s.mixBase = v << 17;
        eachSounding(k, [&](Voice& x) { x.s.mixBase = v << 17; });
        break;
      case 9:
        s.driveAmt = v << 17;
        eachSounding(k, [&](Voice& x) { x.s.driveAmt = v << 17; });
        break;
      case 12: in.mono = v >= 64; break;
      case 13:
        s.ringAmt = v << 17;
        eachSounding(k, [&](Voice& x) { x.s.ringAmt = v << 17; });
        break;
      case 17:
        s.driveSoft = v >= 1;
        eachSounding(k, [&](Voice& x) { x.s.driveSoft = v >= 1; });
        break;
      case 18: in.lfo1Restart = v >= 64; break;
      case 19: in.lfo[0].run.inc = uint32_t(lfoInc(v)); break;
      case 20: in.lfo[0].shape = v == 0 ? 0 : v == 1 ? 1 : 5; break;
      case 21: {
        static const int CODE[8] = {0, 1, 2, 3, 5, 7, 8, 4};
        in.lfo[0].target = CODE[clampi(v, 0, 7)];
        in.lfo[0].depth = depthCurve(in.lfo[0].target)[ix(in.raw[22])];
        break;
      }
      case 22: in.lfo[0].depth = depthCurve(in.lfo[0].target)[v]; break;
      case 23: in.lfo[0].pw = 0x1000000; break;
      case 24: in.lfo[1].run.inc = uint32_t(lfoInc(v)); break;
      case 25: {
        static const int SHAPE[4] = {2, 3, 4, 5};
        in.lfo[1].shape = SHAPE[clampi(v, 0, 3)];
        in.lfo[1].pw = uint32_t(lfoWidth(in.lfo[1].shape, in.raw[28]));
        break;
      }
      case 26: {
        static const int CODE[8] = {0, 1, 2, 4, 6, 5, 9, 3};
        in.lfo[1].target = CODE[clampi(v, 0, 7)];
        in.lfo[1].depth = depthCurve(in.lfo[1].target)[ix(in.raw[27])];
        break;
      }
      case 27: in.lfo[1].depth = depthCurve(in.lfo[1].target)[v]; break;
      case 28: in.lfo[1].pw = uint32_t(lfoWidth(in.lfo[1].shape, v)); break;
      case 29: in.lfo2Restart = v >= 64; break;
      case 30:
      case 31: {
        const int o = c - 30;
        s.osc[o].wave = clampi(v, 0, 11);
        s.osc[o].pwBase = pwFor(s.osc[o].wave, in.raw[78 + o]);
        eachSounding(k, [&](Voice& x) {
          x.s.osc[o].wave = s.osc[o].wave;
          x.s.osc[o].pwBase = s.osc[o].pwBase;
        });
        break;
      }
      case 33: {
        const int32_t d = DETUNE[v] >> 1;
        s.osc[0].fine = -d;
        s.osc[1].fine = d;
        eachSounding(k, [&](Voice& x) {
          x.s.osc[0].fine = -d;
          x.s.osc[1].fine = d;
          x.osc[0].inc = oscInc(0, x.s.osc[0]);
          x.osc[1].inc = oscInc(0, x.s.osc[1]);
        });
        break;
      }
      case 34: s.keyFollow = v >= 64; break;
      case 35:
        s.sync = v >= 64;
        eachSounding(k, [&](Voice& x) { x.s.sync = v >= 64; });
        break;
      case 36:
        s.amp.d = AMP_DECAY[v];
        eachSounding(k, [&](Voice& x) { x.s.amp.d = AMP_DECAY[v]; });
        break;
      case 37: {
        const int32_t sus = int32_t(int64_t(v) * 0x1000000 / 128);
        s.amp.setS(sus);
        eachSounding(k, [&](Voice& x) { x.s.amp.setS(sus); });
        break;
      }
      case 38:
        s.fenv.a = FENV_ATTACK[v];
        eachSounding(k, [&](Voice& x) { x.s.fenv.a = FENV_ATTACK[v]; });
        break;
      case 39:
        s.fenv.d = FENV_DECAY[v];
        eachSounding(k, [&](Voice& x) { x.s.fenv.d = FENV_DECAY[v]; });
        break;
      case 40:
        s.fenv.setS(SUSTAIN_LEVEL[v]);
        eachSounding(k, [&](Voice& x) { x.s.fenv.setS(SUSTAIN_LEVEL[v]); });
        break;
      case 41:
        s.fenv.r = FENV_DECAY[v];
        eachSounding(k, [&](Voice& x) { x.s.fenv.r = FENV_DECAY[v]; });
        break;
      case 42:
        s.fenvVel = v >= 64;
        eachSounding(k, [&](Voice& x) {
          x.s.fenvVel = s.fenvVel;
          x.velFactor = s.fenvVel ? VELOCITY_CURVE[ix(x.velocity)] : 0x1000000;
          x.s.fenv.setA(P24(x.velFactor, s.fenv.A));
        });
        break;
      case 43:
        s.fenv.setA(FENV_AMOUNT[v]);
        eachSounding(k, [&](Voice& x) { x.s.fenv.setA(P24(x.velFactor, s.fenv.A)); });
        break;
      case 44:
        in.lastNote = v;
        s.osc[0].note = s.osc[1].note = v;
        break;
      case 45:
        s.volVel = v >= 64;
        eachSounding(k, [&](Voice& x) { x.s.volVel = v >= 64; });
        break;
      case 50:
        s.penv.setA(ENV_TIME_MS[v]);
        eachSounding(k, [&](Voice& x) { x.s.penv.setA(ENV_TIME_MS[v]); });
        break;
      case 51:
        s.penv.setD(ENV_TIME_MS[v]);
        eachSounding(k, [&](Voice& x) { x.s.penv.setD(ENV_TIME_MS[v]); });
        break;
      case 52:
        s.penv.S = v127(v) << 17;
        eachSounding(k, [&](Voice& x) { x.s.penv.S = v127(v) << 17; });
        break;
      case 53:
        s.penv.setR(ENV_TIME_MS[v] + 2);
        eachSounding(k, [&](Voice& x) { x.s.penv.setR(ENV_TIME_MS[v] + 2); });
        break;
      case 54: {
        const int32_t amt = int32_t(uint32_t(((v127(v) - 64) * 9 * 1024) / 64) & ~0xFFu);
        s.penv.amount = amt;
        eachSounding(k, [&](Voice& x) { x.s.penv.amount = amt; });
        break;
      }
      case 55:
        s.penvTargets = clampi(v, 0, 3);
        eachSounding(k, [&](Voice& x) { x.s.penvTargets = s.penvTargets; });
        break;
      case 64:
        if (v < 64) {
          eachSounding(k, [&](Voice& x) {
            if (!x.pending) return;
            x.s.amp.release();
            x.s.fenv.release();
            x.s.penv.release();
            x.pending = false;
            in.held--;
          });
        }
        break;
      case 65: s.glideOn = v >= 64; break;
      case 70:
        s.amp.setA(VELOCITY_CURVE[v]);
        eachSounding(k, [&](Voice& x) { x.s.amp.setA(VELOCITY_CURVE[v]); });
        break;
      case 71:
      case 75: {
        if (c == 75) s.ftype = clampi(v, 0, 4);
        const int rv = in.raw[71];
        s.res = s.ftype < 3 ? rv : BANDWIDTH_HZ[ix(rv)];
        eachSounding(k, [&](Voice& x) {
          x.s.ftype = s.ftype;
          x.s.res = s.res;
        });
        break;
      }
      case 72:
        s.amp.r = AMP_DECAY[v];
        eachSounding(k, [&](Voice& x) { x.s.amp.r = AMP_DECAY[v]; });
        break;
      case 73:
        s.amp.a = AMP_ATTACK[v];
        eachSounding(k, [&](Voice& x) { x.s.amp.a = AMP_ATTACK[v]; });
        break;
      case 74:
        s.cutBase = CUTOFF_HZ[v];
        eachSounding(k, [&](Voice& x) { x.s.cutBase = CUTOFF_HZ[v]; });
        break;
      case 76: s.keyTrackAmt = clampi(v, 0, 3); break;
      case 77: {
        const int32_t co = ((v127(v) - 64) * 24) / 64;
        s.osc[1].coarse = co;
        eachSounding(k, [&](Voice& x) {
          x.s.osc[1].coarse = co;
          x.osc[1].inc = oscInc(0, x.s.osc[1]);
        });
        break;
      }
      case 78:
      case 79: {
        const int o = c - 78;
        s.osc[o].pwBase = pwFor(s.osc[o].wave, v);
        eachSounding(k, [&](Voice& x) { x.s.osc[o].pwBase = s.osc[o].pwBase; });
        break;
      }
      case 80:
        s.andMode = v >= 64;
        eachSounding(k, [&](Voice& x) { x.s.andMode = v >= 64; });
        break;
      case 94:
        s.send = v << 17;
        eachSounding(k, [&](Voice& x) { x.s.send = v << 17; });
        break;
      default: break;
    }
  }

  // ---- notes

  Voice* findVoice(int k) {
    if (ins_[k].mono)
      for (Voice& v : voice_)
        if (v.inst == k && v.s.amp.stage >= 2) return &v;
    for (Voice& v : voice_)
      if (v.s.amp.stage == 0) return &v;
    return nullptr;
  }

  void startNote(int k, int note, int vel) {
    if (dry_) return;
    Voice* vp = findVoice(k);
    if (!vp) return;
    Voice& v = *vp;
    const bool fresh = v.s.amp.stage == 0;
    v.inst = k;
    v.note = note & 127;
    v.pending = false;
    if (k == 7) {
      startDrum(v, vel);
      return;
    }
    Instrument& in = ins_[k];
    if (fresh) {
      v.s = in.s;
      v.velocity = vel;
      v.Vol = shl(in.volume, 17);
      if (v.s.volVel) v.Vol = P7(v.Vol, vel);
      v.volStep = 0;
    }
    v.counter = 0;
    v.keyTrack = int32_t((PITCH_COARSE[v.note] >> 16) * uint32_t(v.s.keyTrackAmt)) / 3;
    v.velFactor = v.s.fenvVel ? VELOCITY_CURVE[ix(v.velocity)] : 0x1000000;
    v.s.fenv.setA(P24(v.s.fenv.A, v.velFactor));
    if (v.s.glideOn && in.held > 0 && in.lastNote >= 0) {
      v.glideFrom = in.lastNote;
      v.glideCount = v.glideTotal = in.s.glideTime;
    } else {
      v.glideCount = 0;
    }
    if (v.s.keyFollow) {
      v.s.osc[0].note = v.s.osc[1].note = v.note;
      in.lastNote = v.note;
      in.raw[44] = uint8_t(v.note);
    } else {
      v.s.osc[0].note = v.s.osc[1].note = in.raw[44];
    }
    if (fresh) {
      in.held++;
      in.sounding++;
      v.osc[0].phase = v.osc[1].phase = 0;
      v.osc[0].y = v.osc[1].y = 0;     // a noise wave's held value starts from 0
      if (in.lfo1Restart) in.lfo[0].run.phase = 0;
      if (in.lfo2Restart) in.lfo[1].run.phase = 0;
      v.filter.clear();
      v.s.amp.start();
      v.s.fenv.start();
      v.s.penv.start();
    }
  }

  void startDrum(Voice& v, int vel) {
    const DrumNote& d = song_.drum[v.note];
    if (d.sample <= 0) return;
    auto it = song_.samples.find(d.sample);
    if (it == song_.samples.end() || it->second.length == 0) return;
    const Sample& sm = it->second;
    const int32_t N = sm.length;
    v.velocity = vel;
    v.sample = &sm;
    v.inc = shl(trunc32(double(sm.rate) * double(single(PITCH_RATIO[ix(d.pitch)]))), 8) / 44100;
    v.pos = 0;
    v.end = N + 0x4FFF;
    ExpEnv& e = v.s.amp;
    e.a = AMP_ATTACK[ix(d.attack)];
    e.d = AMP_DECAY[ix((d.flags & 0x20) ? (int64_t(vel) * d.decay) >> 7 : d.decay)];
    e.r = AMP_DECAY[2];
    const int32_t vol7 = ins_[7].volume;
    const long long j = (d.flags & 0x40) ? (int64_t(vol7) * d.amplitude * vel) >> 14
                                         : (int64_t(vol7) * d.amplitude) >> 7;
    e.s = 0;
    e.A = shl(VELOCITY_CURVE[ix(j)], 1);
    e.derive();
    v.counter = 0;
    e.start();
    v.s.bus = d.flags & 1;
    v.s.send = shl(d.send, 17);
    if (d.flags & 0x10) v.pos = uint32_t(N - 1) * uint32_t(127 - vel);
    if (d.filter > 0) {
      v.filtered = true;
      v.s.ftype = d.filter - 1;
      v.filter.cutoff = CUTOFF_HZ[ix((d.flags & 0x80) ? (int64_t(vel) * d.cutoff) >> 7 : d.cutoff)];
      v.s.res = v.s.ftype < 3 ? d.resonance : BANDWIDTH_HZ[ix(d.resonance)];
      v.filter.coefficients(v.s.ftype, v.s.res);
      v.filter.clear();
    } else {
      v.filtered = false;
    }
  }

  void releaseNote(int k, int note) {
    if (dry_) return;
    Instrument& in = ins_[k];
    const bool pedal = in.raw[64] >= 64;
    for (Voice& v : voice_) {
      if (v.inst != k || v.note != note || v.s.amp.stage < 2) continue;
      if (pedal) {
        v.pending = true;
        continue;
      }
      v.s.amp.release();
      v.s.fenv.release();
      v.s.penv.release();
      in.held--;
      if (in.lastNote == note) in.lastNote = -1;
    }
  }

  void drumCommand(int note, int cmd, int par) {
    if (dry_) return;
    const DrumNote& d = song_.drum[note & 127];
    const int32_t vol7 = ins_[7].volume;
    for (Voice& v : voice_) {
      if (v.inst != 7 || v.note != (note & 127) || v.s.amp.stage <= 1) continue;
      const int64_t vel = v.velocity;
      switch (cmd) {
        case 7:
          v.s.amp.setA(shl(VELOCITY_CURVE[ix((int64_t(d.amplitude) * vol7 * par) >> 14)], 1));
          break;
        case 9:
          if (v.sample) v.pos = uint32_t(v.sample->length - 1) * uint32_t(par);
          break;
        case 20:
          v.s.amp.d = AMP_DECAY[ix((d.flags & 0x20) ? (int64_t(d.decay) * vel * par) >> 14
                                                    : (int64_t(d.decay) * par) >> 7)];
          break;
        case 25:
          if (v.sample) {
            const int32_t r1 = trunc32(double(v.sample->rate) * double(single(PITCH_RATIO[ix(d.pitch)])));
            const int32_t r2 = trunc32(double(r1) * double(single(PITCH_RATIO[ix(par)])));
            v.inc = shl(r2, 8) / 44100;
          }
          break;
        case 29:
        case 26:
          if (d.filter <= 0) break;
          if (cmd == 29) {
            v.filter.cutoff = CUTOFF_HZ[ix((d.flags & 0x80) ? (int64_t(d.cutoff) * vel * par) >> 14
                                                            : (int64_t(d.cutoff) * par) >> 7)];
            v.filter.coefficients(v.s.ftype, v.s.res);
          }
          v.s.res = d.filter < 4 ? int32_t((int64_t(d.resonance) * par) >> 7)
                                 : BANDWIDTH_HZ[ix((int64_t(d.resonance) * par) >> 7)];
          v.filter.coefficients(v.s.ftype, v.s.res);
          break;
      }
    }
  }

  // ---- the voices' sound

  void stepLfos(int k) {
    Instrument& in = ins_[k];
    if (in.sounding == 0) return;
    Lfo& a = in.lfo[0];
    Lfo& b = in.lfo[1];
    in.L1 = P24(oscStep(a.run, a.shape, a.pw, 0, seed_), a.depth);
    if (a.target == 8) b.run.inc = uint32_t(lfoInc(clampi(in.raw[24] + (in.L1 >> 18), 0, 127)));
    in.L2 = P24(oscStep(b.run, b.shape, b.pw, 0, seed_), b.depth);
  }

  void control(Voice& v) {
    const Instrument& C = ins_[v.inst];
    int32_t T = shl(C.volume, 17);
    if (v.s.volVel) T = P7(T, v.velocity);
    v.volStep = sub(T, v.Vol) >> 10;
    v.osc[0].pw = v.s.osc[0].pwBase;
    v.osc[1].pw = v.s.osc[1].pwBase;
    v.mix = v.s.mixBase;
    int32_t cut = add(v.s.fenv.level, v.s.cutBase);
    int32_t add1 = 0, add2 = 0, common = 0;
    const int32_t L1 = C.L1, L2 = C.L2;
    switch (C.lfo[0].target) {
      case 1: cut = add(cut, P24(cut, shl(L1, 1))); break;
      case 3: add1 = int32_t(3 * int64_t(L1)) >> 13; break;
      case 4: add2 = int32_t(3 * int64_t(L1)) >> 13; break;
      case 5: v.osc[0].pw = add(v.osc[0].pw, P23(v.s.osc[0].pwBase, sub(L1, L1 >> 4))); break;
      case 7: {
        const int64_t m = int64_t(v.s.mixBase) + 2 * int64_t(L1);
        v.mix = m < 0 ? 0 : m > 0x1000000 ? 0x1000000 : int32_t(m);
        break;
      }
    }
    switch (C.lfo[1].target) {
      case 1: cut = add(cut, P24(cut, shl(L2, 1))); break;
      case 3: add1 = add(add1, int32_t(3 * int64_t(L2)) >> 13); break;
      case 4: add2 = add(add2, int32_t(3 * int64_t(L2)) >> 13); break;
      case 5: v.osc[0].pw = add(v.osc[0].pw, P23(v.osc[0].pw, sub(L2, L2 >> 4))); break;
      case 6: v.osc[1].pw = add(v.osc[1].pw, P23(v.osc[1].pw, sub(L2, L2 >> 4))); break;
      case 9: common = int32_t(3 * int64_t(L2)) >> 13; break;
    }
    if (v.glideCount > 0)
      common = add(common, shl(int32_t(uint32_t(v.glideFrom - v.note) * uint32_t(v.glideCount)), 8) / v.glideTotal);
    common = add(common, C.bend);
    const int32_t pe = v.s.penv.output;
    v.osc[0].inc = oscInc(add(add(add1, common), (v.s.penvTargets & 1) ? pe : 0), v.s.osc[0]);
    v.osc[1].inc = oscInc(add(add(add2, common), (v.s.penvTargets & 2) ? pe : 0), v.s.osc[1]);
    v.filter.cutoff = add(v.keyTrack, cut);
    v.filter.coefficients(v.s.ftype, v.s.res);
  }

  int32_t synthSample(Voice& v, int j) {
    Instrument& C = ins_[v.inst];
    if (v.s.amp.step()) C.sounding--;
    if ((v.counter & 7) == 0) v.s.fenv.step();
    v.s.penv.step();
    int32_t amp = P24(v.Vol, v.s.amp.level);
    v.Vol = add(v.Vol, v.volStep);
    if (C.lfo[0].target == 2) amp = add(amp, P24(amp, shl(C.L1, 1)));
    if (C.lfo[1].target == 2) amp = add(amp, P24(amp, shl(C.L2, 1)));
    if (v.glideCount > 0) v.glideCount--;
    const uint32_t c = v.counter++;
    if ((c & 15) == 0) control(v);
    const uint32_t addr1 = clean_ ? 0 : 0x00BAD70Cu + 444u * uint32_t(j);
    const uint32_t addr2 = clean_ ? 0 : addr1 + 52;
    int32_t a;
    if (v.s.sync) {
      const uint32_t before = v.osc[0].phase;
      a = oscStep(v.osc[0], v.s.osc[0].wave, uint32_t(v.osc[0].pw), addr1, seed_);
      if (v.osc[0].phase < before) v.osc[1].phase = 0;
    } else {
      a = oscStep(v.osc[0], v.s.osc[0].wave, uint32_t(v.osc[0].pw), addr1, seed_);
    }
    const int32_t b = oscStep(v.osc[1], v.s.osc[1].wave, uint32_t(v.osc[1].pw), addr2, seed_);
    const int32_t x = add(a, P24(sub(b, a), v.mix));
    const int32_t ring = P23(a, b);
    const int32_t y = v.s.andMode ? (a & b) : ring;
    const int32_t z = add(x, P24(sub(y, x), v.s.ringAmt));
    const int32_t f = v.filter.run(v.s.ftype, z);
    int32_t g;
    if (v.s.driveSoft) {
      g = int32_t(std::nearbyint(std::atan(double(f) / 2516582.5) * 6710886.5));
    } else {
      g = clampi(shl(f, 3), -0x800000, 0x7FFFFF);
    }
    const int32_t h = add(f, P24(sub(g, f), v.s.driveAmt));
    return P24(amp, h);
  }

  int32_t drumSample(Voice& v) {
    v.s.amp.step();
    const uint32_t j = v.pos >> 8;
    const int32_t fr = int32_t(v.pos & 255);
    const Sample* sm = v.sample;
    if (!sm || j >= uint32_t(sm->length) + 0x5000u - 1) {    // only a damaged song gets here
      v.s.amp.stage = 0;
      v.s.amp.level = 0;
      return 0;
    }
    const int32_t s0 = sm->at(j), s1 = sm->at(j + 1);
    const int32_t s = add(shl(s0, 8), (s1 - s0) * fr);
    v.pos += uint32_t(v.inc);
    if (int32_t((v.pos >> 8) - uint32_t(v.end)) >= 0) v.s.amp.stage = 0;
    if (v.filtered) return P24(v.s.amp.level, v.filter.run(v.s.ftype, s));
    return P24(v.s.amp.level, s);
  }

  // ---- the sequencer

  void noteOff(int t) {
    if (ts_[t].note1 > 0) {
      releaseNote(ts_[t].inst, ts_[t].note1 - 1);
      ts_[t].note1 = 0;
    }
  }

  void play(int t, int k, int n, int vel) {
    if (ts_[t].inst == k && ts_[t].note1 == n) {
      noteOff(t);
      startNote(k, n - 1, vel);
    } else {
      startNote(k, n - 1, vel);
      noteOff(t);
    }
    ts_[t].inst = k;
    ts_[t].note1 = n;
  }

  void timed(int t, int k, int o, const uint8_t* row) {
    const int n = row[0], vel = row[1], cmd = row[2], par = row[3];
    const int d = tick_ - o;
    if (cmd == 13) {
      if (par == d && n > 0) {
        noteOff(t);
        if (n <= 128) play(t, k, n, vel);
      }
    } else if (cmd == 14) {
      if (par > 0 && d % par == 0 && n > 0) {
        noteOff(t);
        if (n <= 128) play(t, k, n, vel);
      }
    } else if (cmd == 27) {
      if (par + o >= tpr_) {
        if (tick_ == tpr_ - 1) noteOff(t);
      } else if (par == d) {
        noteOff(t);
      }
    }
  }

  void tick() {
    if (!playing_) return;
    if (tick_ >= tpr_) {
      tick_ = 0;
      ++row_;
      const Block& old = song_.blocks[block_];
      if (row_ >= old.rows || break_) {
        row_ = 0;
        break_ = false;
        ++pos_;
        if (jump_ >= 0) pos_ = jump_, jump_ = -1;
        if (pos_ >= song_.length) {
          playing_ = false;
          pos_ = 0;
          for (int t = 0; t < old.tracks; ++t) noteOff(t);
        }
        const uint32_t newMask = song_.posMask[size_t(pos_)];
        for (int t = 0; t < old.tracks; ++t)
          if (newMask >> t & 1) noteOff(t);
        mask_ = newMask;
        block_ = song_.posBlock[size_t(pos_)];
      }
    }
    const Block& b = song_.blocks[block_];
    if (row_ >= b.rows && b.rows > 0) row_ = 0;     // never with the songs
    // pass 1
    for (int t = 0; t < b.tracks; ++t) {
      if (mask_ >> t & 1) continue;
      const int k = b.trackByte[t] & 15;
      const int o = (row_ & 1) ? b.trackByte[t] >> 4 : 0;
      const uint8_t* row = b.row(t, row_);
      const int n = row[0], vel = row[1], cmd = row[2], par = row[3];
      if (k == 7) {
        if (tick_ == o) {
          if (n > 0 && !(cmd == 13 && par != 0)) {
            if (n <= 128) play(t, 7, n, vel);
            else noteOff(t);
          }
          if (cmd == 27 && par == 0) noteOff(t);
        } else {
          timed(t, k, o, row);
        }
      } else if (tick_ == o) {
        int c = -1, val = par;
        switch (cmd) {
          case 1: c = 22; break;
          case 2: c = 27; break;
          case 3: c = 19; break;
          case 4: c = 24; break;
          case 5: c = 65; val = par ? 64 : 0; break;
          case 6: c = 5; break;
          case 7: c = 50; break;
          case 8: c = 51; break;
          case 9: c = 54; break;
          case 10: c = 37; break;
          case 12: c = 72; break;
          case 16: c = 38; break;
          case 17: c = 39; break;
          case 18: c = 9; break;
          case 19: c = 73; break;
          case 20: c = 36; break;
          case 21: c = 17; break;
          case 23: c = 40; break;
          case 24: c = 41; break;
          case 26: c = 71; break;
          case 29: c = 74; break;
          case 30: c = 43; break;
          case 31: c = 7; break;
          case 33: c = 94; break;
        }
        if (c >= 0) setCtrl(k, c, val);
      }
    }
    // pass 2
    for (int t = 0; t < b.tracks; ++t) {
      const int k = b.trackByte[t] & 15;
      const int o = (row_ & 1) ? b.trackByte[t] >> 4 : 0;
      const uint8_t* row = b.row(t, row_);
      const int n = row[0], vel = row[1], cmd = row[2], par = row[3];
      if (tick_ == o && cmd == 15) {
        if (par >= 1 && par <= 32) tpr_ = par;
        else if (par > 32) setTempo(par);
      }
      if (mask_ >> t & 1) continue;
      if (tick_ == o) {
        if (k != 7 && n > 0 && !(cmd == 13 && par != 0)) {
          if (n <= 128) play(t, k, n, vel);
          else noteOff(t);
        }
        if (k == 7) {
          if (cmd == 7 || cmd == 9 || cmd == 20 || cmd == 25 || cmd == 26 || cmd == 29) {
            if (ts_[t].note1 > 0) drumCommand(ts_[t].note1 - 1, cmd, par);
          } else if (cmd == 31) {
            setCtrl(7, 7, par);
          }
        }
        if (cmd == 11) break_ = true;
        else if (cmd == 22) {
          if (par < b.tracks) {
            noteOff(par);
            mask_ |= 1u << par;
          }
        } else if (cmd == 28) {
          if (par < song_.length - 1) jump_ = par, break_ = true;
        } else if (cmd == 34) {
          setCtrl(k, 14, par);
        } else if (cmd == 35) {
          setCtrl(k, 10, par);
        }
        if (k != 7 && cmd == 27 && par == 0) noteOff(t);
      } else if (k != 7) {
        timed(t, k, o, row);
      }
    }
    ++tick_;
  }
};

// The length AXS reports for a song (from its song header, ignoring jumps and tempo
// changes), in milliseconds
long long reportedMs(const Song& s) {
  double sum = 0;
  for (int i = 0; i < s.length; ++i)
    sum += double(s.blocks[s.posBlock[size_t(i)]].rows) * double(s.header[3]) / 24.0;
  return (long long)(1000.0 * sum * (60.0 / double(clampi(s.header[2], 32, 255))));
}

// The number of frames the song plays: whole blocks up to the one in which it ends; a
// song that has not ended within an hour stops at its reported length (in whole blocks).
long long measure(const Song& s, bool& ends) {
  Engine e(s, false, true);
  const long long most = MAX_SECONDS * RATE;
  int16_t tmp[2];
  long long frames = 0;
  while (frames < most) {
    for (int i = 0; i < BLOCK; ++i) e.frame(tmp);
    frames += BLOCK;
    if (!e.playing()) {
      ends = true;
      return frames;
    }
  }
  ends = false;
  const long long ms = reportedMs(s);
  long long blocks = (ms * RATE + 1000LL * BLOCK - 1) / (1000LL * BLOCK);
  if (blocks < 1) blocks = 1;
  return blocks * BLOCK;
}

std::string latin1(const std::string& in) {
  std::string out;
  for (unsigned char c : in) {
    if (c == 0) break;
    if (c < 0x80) out += char(c);
    else {
      out += char(0xC0 | c >> 6);
      out += char(0x80 | (c & 0x3F));
    }
  }
  return out;
}

std::string trim(std::string t) {
  while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
  while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r' || t.back() == '\n')) t.pop_back();
  return t;
}

void json(const std::string& t) {
  std::putchar('"');
  for (unsigned char c : t) {
    if (c == '"' || c == '\\') std::printf("\\%c", c);
    else if (c < 0x20 || c == 0x7f) std::printf("\\u%04x", c);
    else std::putchar(c);
  }
  std::putchar('"');
}

int fail(const std::string& why) {
  std::printf("{\"tracks\":0,\"error\":");
  json(why);
  std::printf("}\n");
  std::fflush(stdout);
  return 1;
}

bool readFile(const char* path, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  out.resize(MAX_FILE + 1);
  const size_t n = std::fread(out.data(), 1, out.size(), f);
  std::fclose(f);
  out.resize(n);
  return true;
}

void source(void* user, int16_t* frame) { static_cast<Engine*>(user)->frame(frame); }

}  // namespace

#ifndef AXS_NO_MAIN
int main(int argc, char** argv) {
  bool ref = false, clean = false;
  int a = 1;
  for (; a < argc && argv[a][0] == '-' && argv[a][1] == '-'; ++a) {
    if (!std::strcmp(argv[a], "--ref")) ref = true;
    else if (!std::strcmp(argv[a], "--clean")) clean = true;
    else break;
  }
  if (ref ? argc - a < 1 : argc - a < 3) {
    std::fprintf(stderr, "usage: %s [--clean] <file> <track> <samplerate> [start-ms]\n"
                         "       %s [--clean] --ref <file>   (44100 Hz, no header)\n",
                 argv[0], argv[0]);
    return 2;
  }
  std::vector<uint8_t> file;
  if (!readFile(argv[a], file) || file.empty()) return fail("can't read the file");
  if (file.size() > MAX_FILE) return fail("the file is too big for an AXS song");
  Song song;
  const std::string why = load(file, song);
  if (!why.empty()) return fail(why);
  file.clear();
  if (!ref && std::atoi(argv[a + 1]) != 0) return fail("an AXS song has one track");
  int rate = ref ? RATE : std::atoi(argv[a + 2]);
  if (rate < 8000 || rate > 192000) rate = RATE;
  long long startMs = !ref && argc - a > 3 ? std::atoll(argv[a + 3]) : 0;
  if (startMs < 0) startMs = 0;

  bool ends = true;
  const long long total = measure(song, ends);

  if (!ref) {
    int used = 0;
    for (const Block& b : song.blocks) used = std::max(used, b.tracks);
    std::printf("{\"tracks\":1,\"track\":0,\"title\":\"\",\"type\":\"AXS\",\"duration_ms\":%lld,\"facts\":{",
                total * 1000 / RATE);
    std::printf("\"Song\":");
    json(std::to_string(song.length) + " positions, " + std::to_string(song.blocks.size()) +
         " blocks, up to " + std::to_string(used) + " tracks");
    std::printf(",\"Tempo\":");
    json(std::to_string(clampi(song.header[2], 32, 255)) + " BPM, " + std::to_string(song.header[3]) +
         " ticks a row");
    std::printf(",\"Samples\":");
    json(std::to_string(song.samples.size()));
    const std::string user = trim(latin1(song.user));
    if (user == "Free version") {
      std::printf(",\"Saved with\":\"AXS's free version\"");
    } else if (!user.empty()) {
      std::printf(",\"Registered to\":");
      json(user);
    }
    std::printf(",\"Synthesizer\":");
    json(clean ? "AXS's, 44100 Hz, without the smooth triangle's offset"
               : "AXS's, 44100 Hz, as its Winamp plug-in plays it");
    if (!ends) {
      std::printf(",\"Length\":");
      json("the song repeats for ever; it stops at the length AXS gives it");
    }
    std::printf("},\"comment\":\"\",\"names\":[");
    int last = 7;
    while (last >= 0 && trim(latin1(song.names[last])).empty()) --last;
    for (int k = 0; k <= last; ++k) {
      if (k) std::putchar(',');
      json(trim(latin1(song.names[k])));
    }
    std::printf("]}\n");
    std::fflush(stdout);
  }

  Engine engine(song, clean, false);
  std::vector<int16_t> buf(4096 * 2);
  if (ref) {
    long long left = total;
    while (left > 0) {
      const size_t n = left > 4096 ? 4096 : size_t(left);
      for (size_t i = 0; i < n; ++i) engine.frame(&buf[2 * i]);
      if (std::fwrite(buf.data(), 4, n, stdout) != n) return 0;
      left -= (long long)n;
    }
    std::fflush(stdout);
    return 0;
  }

  long long skip = startMs >= total * 1000 / RATE ? total : startMs * RATE / 1000;
  const long long out = total - skip;
  int16_t tmp[2];
  for (long long i = 0; i < skip; ++i) engine.frame(tmp);

  if (rate == RATE) {
    long long left = out;
    while (left > 0) {
      const size_t n = left > 4096 ? 4096 : size_t(left);
      for (size_t i = 0; i < n; ++i) engine.frame(&buf[2 * i]);
      if (std::fwrite(buf.data(), 4, n, stdout) != n) return 0;
      left -= (long long)n;
    }
  } else {
    Resampler rs(RATE, rate, source, &engine);
    long long left = (long long)((unsigned long long)out * (unsigned long long)rate / RATE);
    while (left > 0) {
      const size_t n = left > 4096 ? 4096 : size_t(left);
      for (size_t i = 0; i < n; ++i) rs.frame(&buf[2 * i]);
      if (std::fwrite(buf.data(), 4, n, stdout) != n) return 0;
      left -= (long long)n;
    }
  }
  std::fflush(stdout);
  return 0;
}
#endif
