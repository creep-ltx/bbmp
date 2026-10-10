// SPDX-License-Identifier: GPL-3.0-or-later
// Beepola songs (.bbsong): ZX Spectrum beeper music for eight player routines.
//
// Written for bbmp in a clean room, from a description of what the players do in
// time (no Z80 emulation and no memory images): the song file is read, laid out in
// a 64 KB memory as the players expect it, and each player is a timed step machine.
// Every step takes a number of T-states (3.5 MHz); some write to the speaker. The
// speaker changes are turned into samples by a box filter and a windowed sinc.
//
// Three pieces of data are not part of this program and are read at run time from
// the folder named by BBMP_BEEPOLA_DATA, each checked by size and SHA-256:
//   zx48.rom            the Spectrum's ROM; some drums play its bytes as noise
//   p1d-samples.bin     the drum samples of the Phaser1 "digital drums" player
//   svg-table-tail.bin  the bytes after the Savage player's frequency table
// A song that needs one of them while it is missing is refused; the others play.
//
// BBMP_BEEPOLA_DUMP=<prefix> writes <prefix>.edges, .ticks, .marks and .layout
// (speaker changes, tick times, marks and the end, the laid out bytes) for tests.

#include "engine.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ay {
namespace {

using u8 = uint8_t;
using u16 = uint16_t;
using i64 = int64_t;

// ---------------------------------------------------------------------------
// SHA-256, for checking the data files.

struct Sha256 {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void block(const u8* p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  static std::string hex(const std::vector<u8>& data) {
    Sha256 s;
    std::vector<u8> m(data);
    const uint64_t bits = uint64_t(data.size()) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; --i) m.push_back(u8(bits >> (8 * i)));
    for (size_t i = 0; i < m.size(); i += 64) s.block(&m[i]);
    std::string out;
    char buf[9];
    for (uint32_t v : s.h) { std::snprintf(buf, sizeof(buf), "%08x", v); out += buf; }
    return out;
  }
};

// ---------------------------------------------------------------------------
// The data that is fetched, not shipped.

struct DataFile {
  const char* name;
  size_t size;
  const char* sha256;
};
const DataFile kRomFile = {"zx48.rom", 16384,
                           "d55daa439b673b0e3f5897f99ac37ecb45f974d1862b4dadb85dec34af99cb42"};
const DataFile kSampleFile = {"p1d-samples.bin", 1024,
                              "552bef0cd0ad3890b542692248071489f91964f5c5aad3d3278a36b1760025ff"};
const DataFile kTailFile = {"svg-table-tail.bin", 136,
                            "41ffa3729b07d6bd64370f93b880611cd8987d9ad257e81c0edddd18f8f231b5"};

// The file's bytes when BBMP_BEEPOLA_DATA has it with the right size and checksum.
bool load_data(const DataFile& f, std::vector<u8>& out) {
  const char* dir = std::getenv("BBMP_BEEPOLA_DATA");
  if (!dir || !*dir) return false;
  const std::string path = std::string(dir) + "/" + f.name;
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return false;
  std::vector<u8> d(f.size + 1);
  const size_t n = std::fread(d.data(), 1, d.size(), fp);
  std::fclose(fp);
  if (n != f.size) return false;
  d.resize(n);
  if (Sha256::hex(d) != f.sha256) return false;
  out.swap(d);
  return true;
}

// ---------------------------------------------------------------------------
// Fixed tables.

const u8 kSfxFreq[54] = {253, 238, 225, 212, 200, 189, 178, 168, 159, 150, 142, 134, 126, 119,
                         112, 106, 100, 94,  89,  84,  79,  75,  71,  67,  63,  59,  56,  53,
                         50,  47,  44,  42,  39,  37,  35,  33,  31,  29,  28,  27,  25,  23,
                         22,  21,  19,  18,  17,  16,  15,  14,  13,  12,  1,   0};
const u8 kBoxPeriod[53] = {255, 240, 227, 215, 203, 192, 180, 171, 161, 151, 144, 136, 128, 121,
                           114, 108, 102, 96,  91,  86,  81,  76,  72,  68,  64,  61,  57,  54,
                           51,  48,  45,  43,  40,  38,  36,  34,  32,  30,  28,  27,  25,  24,
                           23,  21,  20,  19,  18,  17,  16,  15,  14,  13,  12};
const u8 kStudioDrum[14] = {1, 30, 28, 27, 25, 24, 23, 21, 20, 19, 18, 17, 16, 0};
const u16 kPhaserFreq[60] = {
    178,  189,  200,  212,  225,  238,  252,  267,  283,  300,  318,  337,  357,  378,  401,
    425,  450,  477,  505,  535,  567,  601,  637,  675,  715,  757,  802,  850,  901,  954,
    1011, 1071, 1135, 1202, 1274, 1350, 1430, 1515, 1605, 1701, 1802, 1909, 2023, 2143, 2270,
    2405, 2548, 2700, 2860, 3030, 3211, 3402, 3604, 3818, 4046, 4286, 4541, 4811, 5097, 5400};
const u16 kSavageFreq[60] = {
    55512, 52224, 192,   46773, 44203, 41633, 39320, 37008, 34952, 33152, 31097, 29554,
    27756, 26214, 24672, 23386, 22101, 20817, 19788, 18504, 17476, 16448, 15676, 14649,
    13878, 13107, 12336, 11821, 11051, 10536, 9766,  9252,  8738,  8224,  7966,  7452,
    6939,  6681,  6168,  5910,  5653,  5140,  4883,  4626,  4369,  4112,  3855,  3854,
    3597,  3340,  3084,  3083,  2826,  2570,  2569,  2313,  2312,  2056,  2055,  1799};
const u16 kPlipFreq[49] = {0,   948, 894, 844, 798, 752, 710, 670, 633, 597, 564, 532, 502,
                           474, 447, 422, 399, 376, 355, 335, 316, 299, 282, 266, 251, 237,
                           224, 211, 199, 188, 178, 168, 158, 149, 141, 133, 126, 118, 112,
                           106, 100, 94,  89,  84,  79,  75,  70,  67,  63};
// ROM Beep: the BEEP of pitch k, as timed: preparation, first edge, half period, edges.
struct BeepTiming { int prep, first, half, edges; };
const BeepTiming kBeep[70] = {
    {63249, 200, 6686, 10},  {63558, 232, 6310, 12},  {63547, 232, 5958, 12},
    {63502, 232, 5622, 12},  {63543, 216, 5306, 14},  {63576, 184, 5010, 14},
    {63624, 232, 4726, 14},  {63682, 200, 4462, 16},  {63558, 184, 4210, 16},
    {63378, 232, 3974, 18},  {63411, 232, 3750, 18},  {63408, 232, 3542, 20},
    {63522, 200, 3342, 20},  {63520, 184, 3154, 22},  {63509, 184, 2978, 24},
    {63464, 216, 2810, 24},  {63505, 200, 2654, 26},  {63538, 232, 2502, 28},
    {63586, 216, 2362, 30},  {63644, 232, 2230, 32},  {63520, 216, 2106, 34},
    {63340, 184, 1986, 36},  {63373, 184, 1874, 38},  {63370, 216, 1770, 40},
    {63484, 232, 1670, 42},  {63482, 232, 1574, 44},  {63471, 200, 1486, 46},
    {63426, 216, 1402, 50},  {63467, 200, 1326, 52},  {63500, 184, 1250, 56},
    {63548, 216, 1178, 60},  {63296, 216, 1114, 62},  {63266, 216, 1050, 66},
    {63086, 200, 990, 70},   {63119, 216, 938, 74},   {63116, 184, 882, 80},
    {63230, 184, 834, 84},   {63228, 184, 786, 88},   {63217, 232, 742, 94},
    {63172, 200, 702, 100},  {63213, 232, 662, 106},  {63306, 200, 622, 112},
    {63354, 200, 590, 118},  {63252, 216, 554, 126},  {63222, 200, 526, 132},
    {63042, 200, 494, 140},  {63075, 184, 466, 150},  {63072, 216, 442, 158},
    {63186, 200, 414, 168},  {63184, 216, 394, 178},  {63239, 184, 370, 188},
    {63194, 200, 350, 200},  {63235, 216, 330, 210},  {63268, 232, 310, 224},
    {63316, 232, 294, 236},  {63214, 232, 278, 250},  {63190, 232, 262, 266},
    {63010, 232, 246, 282},  {63109, 216, 234, 298},  {63106, 216, 218, 316},
    {63220, 200, 206, 334},  {63218, 184, 194, 354},  {63207, 232, 182, 376},
    {63228, 200, 174, 398},  {63269, 184, 162, 422},  {63302, 216, 154, 448},
    {63416, 184, 146, 474},  {63314, 216, 138, 502},  {63576, 184, 130, 532},
    {63559, 216, 122, 564}};

const char kNoFit[] = "the song doesn't fit into the Spectrum's memory";
const char kCantPlay[] = "the song has notes the engine can't play";

// ---------------------------------------------------------------------------
// The song as read from the file.

struct Pattern {
  int rows = 16;
  int tempo = 14;
  std::string name;
  u8 col[2][256];
  u8 drum[256];
  u8 sus[2][256];
  u8 warp[2][256];
  u16 glide[2][256], skew[2][256], skewchg[2][256], orn[2][256];
  Pattern() {
    std::memset(col, 255, sizeof(col));
    std::memset(drum, 0, sizeof(drum));
    std::memset(sus, 255, sizeof(sus));
    std::memset(warp, 0, sizeof(warp));
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < 256; ++i) glide[c][i] = skew[c][i] = skewchg[c][i] = orn[c][i] = 256;
  }
};

struct Instrument {
  u8 multiple = 1;
  int32_t detune = 0;
  u8 phase = 0;
};

struct Ornament {
  int length = 0;
  u8 v[256];
  Ornament() { std::memset(v, 255, sizeof(v)); }
};

struct Song {
  std::string title, author, engine = "SFX";
  int length = 1, loop = 0;
  u8 order[1025];
  std::vector<Pattern> pat = std::vector<Pattern>(256);
  Instrument ins[100];
  Ornament orn[32];
  Song() {
    std::memset(order, 255, sizeof(order));
    order[0] = 0;
    orn[1].length = 3;
    orn[1].v[0] = 0; orn[1].v[1] = 4; orn[1].v[2] = 135;
    orn[2].length = 4;
    orn[2].v[0] = 12; orn[2].v[1] = 0; orn[2].v[2] = 12; orn[2].v[3] = 12;
  }
  // The patterns the order list uses, in increasing number.
  std::vector<int> used() const {
    bool u[256] = {};
    for (int i = 0; i < length; ++i) u[order[i]] = true;
    std::vector<int> out;
    for (int p = 0; p < 256; ++p)
      if (u[p]) out.push_back(p);
    return out;
  }
};

class Reader {
 public:
  explicit Reader(const std::vector<u8>& d) : d_(d) {}
  bool at_end() const { return pos_ >= d_.size(); }
  u8 byte() {
    if (pos_ >= d_.size()) throw Refused("the file ends too early");
    return d_[pos_++];
  }
  int32_t int32() {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(byte()) << (8 * i);
    return int32_t(v);
  }
  u16 word() {
    const u16 lo = byte();
    return u16(lo | (byte() << 8));
  }
  std::string text() {
    std::string s;
    while (pos_ < d_.size() && d_[pos_] != 0 && s.size() < 1024) s += char(d_[pos_++]);
    if (pos_ < d_.size() && d_[pos_] == 0) ++pos_;
    return s;
  }
  size_t pos_ = 0;

 private:
  const std::vector<u8>& d_;
};

void split(const std::string& line, std::string& key, std::string& value) {
  const size_t eq = line.find('=');
  if (eq == std::string::npos) { key.clear(); value = line; return; }
  key = line.substr(0, eq);
  value = line.substr(eq + 1);
}

int number(const std::string& v) {
  if (v.empty() || v.size() > 9) return 0;
  int n = 0;
  for (char c : v) {
    if (c < '0' || c > '9') return 0;
    n = n * 10 + (c - '0');
  }
  return n;
}

int checked_rows(int32_t n) {
  if (n < 0 || n > 256) throw Refused("a pattern is longer than 256 rows");
  return n;
}

void read_song(const std::vector<u8>& data, Song& s) {
  static const u8 kMagic[12] = {'B', 'B', 'S', 'O', 'N', 'G', 0, '0', '0', '0', '1', 0};
  if (data.size() < 12 || std::memcmp(data.data(), kMagic, 12) != 0) throw Refused("not a Beepola song");
  Reader r(data);
  r.text();
  r.text();
  bool any = false;
  std::string key, value;
  while (!r.at_end()) {
    const std::string block = r.text();
    if (block.empty() || block[0] != ':') break;
    any = true;
    while (!r.at_end()) {
      const std::string line = r.text();
      if (line == ":END") break;
      split(line, key, value);
      if (block == ":INFO") {
        if (key == "Title") s.title = value;
        else if (key == "Author") s.author = value;
        else if (key == "Engine") {
          s.engine = value;
          for (char& c : s.engine)
            if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        }
      } else if (block == ":LAYOUT") {
        if (key == "LoopStart") s.loop = number(value);
        else if (key == "Length") {
          const int n = number(value);
          if (n < 1 || n > 1024) throw Refused("the song has no length");
          s.length = n;
          for (int i = 0; i < n; ++i) s.order[i] = r.byte();
        }
      } else if (block == ":PATTERNDATA") {
        if (key != "PatternCount") continue;
        const int count = number(value);
        if (count > 256) throw Refused("more than 256 patterns");
        for (int i = 0; i < count; ++i) {
          std::string k2, v2;
          split(r.text(), k2, v2);
          if (k2 != "PatternName") continue;
          Pattern& p = s.pat[i];
          p.name = v2;
          p.rows = checked_rows(r.int32());
          p.tempo = std::clamp<int32_t>(r.int32(), -1000, 1000);
          for (int j = 0; j < p.rows; ++j) p.col[0][j] = r.byte();
          for (int j = 0; j < p.rows; ++j) p.col[1][j] = r.byte();
          for (int j = 0; j < p.rows; ++j) p.drum[j] = r.byte();
          for (int j = 0; j < p.rows; ++j) p.sus[0][j] = r.byte();
          for (int j = 0; j < p.rows; ++j) p.sus[1][j] = r.byte();
        }
      } else if (block == ":SVGPATTERNDATA" || block == ":SVGWARPDATA") {
        if (key != "PatternCount") continue;
        const int count = number(value);
        if (count > 256) throw Refused("more than 256 patterns");
        for (int i = 0; i < count; ++i) {
          Pattern& p = s.pat[i];
          const int n = checked_rows(r.int32());
          if (block == ":SVGPATTERNDATA") {
            u16* arrays[8] = {p.glide[0], p.glide[1], p.skew[0],    p.skew[1],
                              p.skewchg[0], p.skewchg[1], p.orn[0], p.orn[1]};
            for (u16* a : arrays)
              for (int j = 0; j < n; ++j) a[j] = r.word();
          } else {
            for (int c = 0; c < 2; ++c)
              for (int j = 0; j < n; ++j) p.warp[c][j] = r.byte();
          }
        }
      } else if (block == ":P1INSTR") {
        if (key != "Length") continue;
        const int c = std::min(number(value), 100);
        for (int i = 0; i < c; ++i) {
          s.ins[i].multiple = r.byte();
          s.ins[i].detune = r.int32();
          s.ins[i].phase = r.byte();
        }
      } else if (block == ":SVGORNAMENTS") {
        if (key != "OrnamentCount") continue;
        const int count = number(value);
        if (count > 32) throw Refused("more than 32 ornaments");
        for (int i = 0; i < count; ++i) {
          Ornament& o = s.orn[i];
          o.length = checked_rows(r.int32());
          std::memset(o.v, 255, sizeof(o.v));
          for (int j = 0; j < o.length; ++j) o.v[j] = r.byte();
        }
      }
    }
  }
  if (!any) throw Refused("the song has no data");
  if (s.loop < 0 || s.loop >= s.length) s.loop = 0;
  for (Pattern& p : s.pat)
    if (p.rows < 1) p.rows = 1;
}

// ---------------------------------------------------------------------------
// The 64 KB memory and the layout's writing.

struct Memory {
  u8 m[65536] = {};
  bool written[65536] = {};
  int wp = 0;  // write position (may run past the end, which is refused)
  void set(int a, u8 v) {
    if (a < 0x4000 || a > 0xFFFF) throw Refused(kNoFit);
    m[a] = v;
    written[a] = true;
  }
  void put(int v) { set(wp++, u8(v)); }
  void put_word(int v) { put(v & 255); put((v >> 8) & 255); }
  void set_word(int a, int v) { set(a, u8(v)); set(a + 1, u8(v >> 8)); }
  void put_all(const std::vector<u8>& bytes) {
    for (u8 b : bytes) put(b);
  }
};

// ---------------------------------------------------------------------------
// From speaker changes to samples.

class Sound {
 public:
  explicit Sound(int rate) : rate_(rate) {
    step_ = 3500000.0 / (rate * 4.0);
    edge_ = step_;
    dcrate_ = 1.0 / (rate * 0.05);
    const double cut = std::min(12500.0, 0.45 * rate) / (rate * 4.0);
    double taps[255];
    double total = 0;
    for (int i = 0; i < 255; ++i) {
      const double x = i - 127.0;
      const double s = x == 0 ? 2 * cut : std::sin(2 * M_PI * cut * x) / (M_PI * x);
      const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / 254) + 0.08 * std::cos(4 * M_PI * i / 254);
      taps[i] = s * w;
    }
    for (int i = 0; i < 255; ++i) total += taps[i];
    for (int i = 0; i < 255; ++i) tap_[i] = float(taps[i] / total);
  }
  virtual ~Sound() = default;

  void advance(i64 u) {
    const double t = double(u - base_);
    while (t >= edge_) {
      sum_ += level * (edge_ - now_);
      emit(sum_ / step_);
      sum_ = 0;
      now_ = edge_;
      edge_ += step_;
    }
    sum_ += level * (t - now_);
    now_ = t;
    if (now_ > 1e9) {
      const i64 whole = i64(now_);
      base_ += whole;
      now_ -= double(whole);
      edge_ -= double(whole);
    }
  }
  int level = 0;

 protected:
  virtual void frame(int16_t s) = 0;

 private:
  void emit(double v) {
    line_[at_] = float(v);
    at_ = at_ == 254 ? 0 : at_ + 1;
    if (++phase_ < 4) return;
    phase_ = 0;
    float x = 0;
    int k = at_;
    for (int j = 0; j < 255; ++j) {
      x += tap_[j] * line_[k];
      k = k == 254 ? 0 : k + 1;
    }
    dc_ += (x - dc_) * dcrate_;
    long s = std::lrint((x - dc_) * 16000.0);
    if (s < -32768) s = -32768;
    if (s > 32767) s = 32767;
    frame(int16_t(s));
  }

  int rate_;
  double step_, edge_, now_ = 0, sum_ = 0, dc_ = 0, dcrate_;
  i64 base_ = 0;
  int phase_ = 0;
  float line_[255] = {};
  int at_ = 0;
  float tap_[255];
};

// ---------------------------------------------------------------------------
// The step machine.

struct SongOver {};
struct Unplayable {};   // something the description says never happens
struct Stopped {};      // the player was closed while pass 2 ran

enum : unsigned { kMark = 1, kR2 = 2, kOn = 4, kOff = 8, kWait = 16, kOut = 32 };

struct Needs {
  bool rom = false, samples = false, tail = false;
};

struct Log {
  std::vector<i64> edges, ticks, marks;
  std::vector<size_t> inside;  // ROM Beep: the ticks inside a beep's preparation (moments not exact)
};

class Machine {
 public:
  Machine(const Memory& mem, int length, Needs& needs) : mem_(mem), length_(length), needs_(needs) {}
  virtual ~Machine() = default;

  // Runs until the song ends; the end time is end_time.
  void play() {
    try {
      run();
      for (;;) step(4);  // a routine that returned at the outermost level: nothing more
    } catch (const SongOver&) {
    }
    if (sound) sound->advance(end_time);
  }

  i64 end_time = 0;
  bool end_by_marks = false;
  long changes = 0;
  Sound* sound = nullptr;
  Log* log = nullptr;

 protected:
  virtual void run() = 0;
  virtual void tick() = 0;

  u8 rd(u16 a) {
    if (a < 0x4000) needs_.rom = true;
    else if (a >= sample_lo_ && a <= sample_hi_) needs_.samples = true;
    else if (a >= tail_lo_ && a <= tail_hi_) needs_.tail = true;
    return mem_.m[a];
  }
  u16 rdw(u16 a) { return u16(rd(a) | (rd(u16(a + 1)) << 8)); }

  void finish(bool by_marks) {
    end_time = t_;
    end_by_marks = by_marks;
    throw SongOver{};
  }

  void frame_check() {
    if (t_ >= next_frame_) {
      next_frame_ += 69888;
      if (ticks_on_) tick_wanted_ = true;
      else waiting_ = false;
    }
  }

  void begin(bool mark) {
    frame_check();
    if (mark && !waiting_) {
      ++marks_;
      last_mark_ = t_;
      if (log) log->marks.push_back(t_);
      if (marks_ > length_) finish(true);
    }
    if (t_ - last_mark_ > 2100000000LL || t_ - last_change_ > 70000000LL || t_ > 12600000000LL)
      finish(false);
  }

  void change(i64 at, int lv) {
    if (sound) {
      sound->advance(at);
      sound->level = lv;
    }
    level_ = lv;
    last_change_ = at;
    ++changes;
    if (log) log->edges.push_back(at);
  }

  // One step of d T-states.
  void step(int d, unsigned f = 0, int value = 0) {
    begin(f & kMark);
    const i64 e = t_ + d;
    if (f & kOut) {
      const int lv = (value >> 4) & 1;
      if (lv != level_) change(e, lv);
    }
    if (f & kOn) ticks_on_ = true;
    else if (f & kOff) ticks_on_ = false;
    t_ = e;
    refresh_ += (f & kR2) ? 2 : 1;
    if (f & kWait) waiting_ = true;
    if (tick_wanted_ && ticks_on_ && !(f & kOn)) take_tick();
    else if (sound) sound->advance(t_);
  }
  void out(int d, int value) { step(d, kOut, value); }

  void take_tick() {
    tick_wanted_ = false;
    ticks_on_ = false;
    waiting_ = false;
    t_ += tick_cost_;
    ++refresh_;
    if (log) log->ticks.push_back(t_);
    if (sound) sound->advance(t_);
    tick();
  }

  // A waiting step, then steps of 4 until a tick or a lost frame ends the wait.
  void wait_step() {
    step(4, kWait);
    while (waiting_) {
      frame_check();
      if (!waiting_) break;
      step(4);
    }
  }

  // n times the steps 4 and 12, the last 12 being 7 (n = 0 means 256).
  void count(u8 n) {
    const int k = n ? n : 256;
    for (int i = 1; i <= k; ++i) {
      step(4);
      step(i == k ? 7 : 12);
    }
  }
  // n steps of 13, the last being 8 (n = 0 means 256).
  void hold(u8 n) {
    const int k = n ? n : 256;
    for (int i = 1; i < k; ++i) step(13);
    step(8);
  }
  void steps(std::initializer_list<int> ds) {
    for (int d : ds) step(d);
  }

  const Memory& mem_;
  int length_;
  Needs& needs_;
  int sample_lo_ = 0x10000, sample_hi_ = -1, tail_lo_ = 0x10000, tail_hi_ = -1;

  i64 t_ = 0, next_frame_ = 69888;
  bool ticks_on_ = false, tick_wanted_ = false, waiting_ = false;
  int tick_cost_ = 19;
  int marks_ = 0;
  i64 last_mark_ = 0, last_change_ = 0;
  int level_ = 0;
  unsigned refresh_ = 0;
};

// ---------------------------------------------------------------------------
// SFX: Special FX.

// A column or drum stream of the SFX layout; `notes` gets the note values written.
void sfx_put(std::vector<u8>& out, std::vector<int>* notes, int n, int len) {
  while (len > 255) {
    out.push_back(u8(n));
    out.push_back(255);
    if (notes) notes->push_back(n);
    len -= 255;
  }
  if (len > 0) {
    out.push_back(u8(n));
    out.push_back(u8(len));
    if (notes) notes->push_back(n);
  }
}

std::vector<u8> sfx_column(const Pattern& p, int c, std::vector<int>* notes) {
  std::vector<u8> out;
  const int r = 21 - p.tempo;
  int n = 131, len = 0;
  for (int i = 0; i < p.rows; ++i) {
    if (p.col[c][i] == 255) len += r;
    else {
      if (len > 0) sfx_put(out, notes, n, len);
      n = p.col[c][i];
      len = r;
    }
    if (p.sus[c][i] != 255) {
      out.push_back(129);
      out.push_back(p.sus[c][i]);
    }
  }
  if (len > 0) sfx_put(out, notes, n, len);
  out.push_back(128);
  return out;
}

std::vector<u8> sfx_drums(const Pattern& p) {
  std::vector<u8> out;
  const int r = 21 - p.tempo;
  int n = 133, len = 0;
  for (int i = 0; i < p.rows; ++i) {
    const int d = p.drum[i];
    if (d < 128 || d > 133) len += r;
    else {
      if (len > 0) sfx_put(out, nullptr, n, len);
      n = d;
      len = r;
    }
  }
  if (len > 0) sfx_put(out, nullptr, n, len);
  out.push_back(128);
  return out;
}

void sfx_check(const Song& s) {
  for (int p : s.used())
    for (int c = 0; c < 2; ++c) {
      std::vector<int> notes;
      sfx_column(s.pat[p], c, &notes);
      for (int n : notes)
        if (n >= 132 && n <= 254) throw Refused(kCantPlay);
    }
}

void sfx_layout(const Song& s, Memory& m) {
  for (int i = 0; i < 54; ++i) m.m[0x8290 + i] = kSfxFreq[i];
  m.wp = 0x82C6;
  int A[256] = {}, D[256] = {};
  const std::vector<int> used = s.used();
  for (int p : used) {
    A[p] = m.wp;
    m.wp += 2;
    m.put_all(sfx_column(s.pat[p], 0, nullptr));
    m.set_word(A[p], m.wp);
    m.put_all(sfx_column(s.pat[p], 1, nullptr));
  }
  m.set_word(0x8019, m.wp);
  int L = 0;
  for (int i = 0; i < s.length; ++i) {
    if (i == s.loop) L = m.wp;
    m.put_word(A[s.order[i]]);
  }
  m.put_word(0);
  m.put_word(L);
  for (int p : used) {
    D[p] = m.wp;
    m.put_all(sfx_drums(s.pat[p]));
  }
  m.set_word(0x8020, m.wp);
  for (int i = 0; i < s.length; ++i) {
    if (i == s.loop) L = m.wp;
    m.put_word(D[s.order[i]]);
  }
  m.put_word(0);
  m.put_word(L);
}

class SpecialFx : public Machine {
 public:
  using Machine::Machine;

 private:
  u16 Q1 = 0, Q2 = 0, DQ = 0, ML = 0, DL = 0;
  u8 n1 = 1, n2 = 1, c1 = 0, c2 = 0, P1 = 59, P2 = 59, G1 = 7, G2 = 2, W1 = 1, W2 = 13;
  u8 V1 = 0, V2 = 0, S1 = 0, S2 = 0, K1 = 2, K2 = 4, nd = 1;
  bool flag = false;
  bool first_pattern = true;

  void run() override {
    ML = rdw(0x8019);
    DL = rdw(0x8020);
    t_ = 5747;
    next_pattern();
    first_pattern = false;
    step(17);
    next_drums();
    steps({7, 4, 10});
    step(4, kOn);
    wait_step();
    main_pass();
  }

  void main_pass() {
    u8 x = 0, k = 0;
    u16 h = 0;
  A1:
    if (flag) {
      step(17);
      decay();
    } else step(10);
    x = n1; step(4);
    flag = false; step(4);
    if (x != 0) { step(12); step(7); count(3); goto T1; }
    step(7);
  R1:
    h = Q1; step(10);
  R2:
    x = rd(h); step(7);
    flag = false; step(4);
    step(10);
    if (x >= 128) goto C1;
    steps({17, 11, 11, 10, 4, 7});
    flag = false; step(11);
    x = rd(u16(0x8290 + x)); step(7);
    steps({10, 10, 10});
    P1 = x; step(13);
    step(8); step(8); x = x / 8; step(8);
    c1 = x; step(4);
    W1 = x; step(13);
    flag = false; step(4);
    S1 = 0; step(13);
    step(4);
    G1 = 1; step(13);
    step(13);
    V1 = V1 | 24; flag = false; step(7);
    step(13);
    h++; step(6);
  R5:
    x = rd(h); step(7);
    step(13);
    h++; step(6);
    Q1 = h; step(16);
    n1 = x; step(7);
    step(12);
    goto T1;
  C1:
    h++; step(6);
    step(11);
    k = x & 127; flag = false; step(7);
    steps({17, 10, 4, 4, 4, 7, 6, 7, 4, 4});
    switch (k) {
      case 0: step(10); step(17); next_pattern(); step(10); goto R1;
      case 1: step(10); K1 = rd(h); step(7); step(13); h++; step(6); step(10); goto R2;
      case 2: step(10); step(13); V1 = V1 & 7; flag = false; step(7); step(13); step(10); goto R5;
      case 3: step(10); step(10); goto R5;
      default: throw Unplayable{};
    }
  T1:
    c1--; step(4);
    if (c1 != 0) { step(12); goto U1; }
    step(7);
    c1 = P1; step(7);
    step(7);
    count(G1);
    step(7);
    out(11, V1);
    step(7);
    count(W1);
    step(7);
    out(11, 0);
  U1:
    x = n2; step(4);
    flag = false; step(4);
    if (x != 0) { step(12); step(7); count(3); goto T2; }
    step(7);
    h = Q2; step(10);
  R7:
    x = rd(h); step(7);
    flag = false; step(4);
    step(10);
    if (x >= 128) goto C2;
    steps({17, 11, 11, 10, 4, 7});
    flag = false; step(11);
    x = rd(u16(0x8290 + x)); step(7);
    steps({10, 10, 10});
    P2 = x; step(13);
    step(8); x = x / 4; step(8);
    W2 = x; step(13);
    c2 = x; step(4);
    flag = false; step(4);
    S2 = 0; step(13);
    step(4);
    G2 = 1; step(13);
    step(13);
    V2 = V2 | 24; flag = false; step(7);
    step(13);
    h++; step(6);
  R8:
    x = rd(h); step(7);
    step(13);
    h++; step(6);
    Q2 = h; step(16);
    n2 = x; step(7);
    step(12);
    goto T2;
  C2:
    h++; step(6);
    step(11);
    k = x & 127; flag = false; step(7);
    steps({17, 10, 4, 4, 4, 7, 6, 7, 4, 4});
    switch (k) {
      case 0: step(10); step(17); next_pattern(); step(10); goto R1;
      case 1: step(10); K2 = rd(h); step(7); step(13); h++; step(6); step(12); goto R7;
      case 2: step(10); step(13); V2 = V2 & 7; flag = false; step(7); step(13); step(10); goto R8;
      case 3: step(10); step(10); goto R8;
      default: throw Unplayable{};
    }
  T2:
    c2--; step(4);
    step(10);
    if (c2 != 0) goto A1;
    c2 = P2; step(7);
    step(7);
    count(G2);
    step(7);
    out(11, V2);
    step(7);
    count(W2);
    step(7);
    out(11, 0);
    step(10);
    goto A1;
  }

  void decay() {
    u8 x;
    x = S1; step(7);
    x++; step(4);
    S1 = x; step(13);
    flag = x < K1; step(7);
    if (flag) { step(12); goto D3; }
    step(7);
    flag = false; step(4);
    S1 = 0; step(13);
    step(10);
    W1--; step(11);
    if (W1 == 0) { step(12); W1++; step(11); goto D3; }
    step(7);
    step(10);
    G1++; step(11);
  D3:
    x = S2; step(7);
    x++; step(4);
    S2 = x; step(13);
    flag = x < K2; step(7);
    if (flag) { step(11); return; }
    step(5);
    flag = false; step(4);
    S2 = 0; step(13);
    step(10);
    W2--; step(11);
    if (W2 == 0) { step(12); W2++; step(11); step(10); return; }
    step(7);
    step(10);
    G2++; step(11);
    step(10);
  }

  void next_pattern() {
    u16 h;
    u8 e, d;
    h = ML; step(10, kMark);
  N2:
    e = rd(h); step(7);
    h++; step(6);
    d = rd(h); step(7);
    h++; step(6);
    step(4);
    flag = false; step(4);
    if ((d << 8 | e) != 0) { step(12); goto N3; }
    step(7);
    e = rd(h); step(7);
    h++; step(6);
    d = rd(h); step(7);
    h = u16(d << 8 | e); step(4);
    step(12);
    goto N2;
  N3:
    ML = h; step(16);
    h = u16(d << 8 | e); step(4);
    e = rd(h); step(7);
    h++; step(6);
    d = rd(h); step(7);
    h++; step(6);
    Q1 = h; step(16);
    Q2 = u16(d << 8 | e);
    if (!first_pattern) { c1 = d; c2 = e; }
    step(20);
    step(10);
  }

  void next_drums() {
    u16 h;
    u8 e, d;
    h = DL; step(10);
    for (;;) {
      e = rd(h); step(7);
      h++; step(6);
      d = rd(h); step(7);
      h++; step(6);
      DL = h; step(16);
      DQ = u16(d << 8 | e); step(20);
      step(4); step(4);
      if (DQ != 0) { step(11); return; }
      step(5);
      e = rd(h); step(7);
      h++; step(6);
      d = rd(h); step(7);
      h = u16(d << 8 | e); step(4);
      step(12);
    }
  }

  void tick() override {
    steps({12, 10, 11, 11, 11});
    n2--; step(4);
    n1--; step(4);
    step(4);
    nd--; step(4);
    if (nd == 0) {
      step(17);
      drum();
    } else step(10);
    steps({4, 11, 4, 7, 7, 4, 11, 8, 12, 4, 10, 10, 10});
    flag = true; step(4);
    step(4, kOn);
    step(14);
  }

  void drum() {
    for (;;) {
      u16 h = DQ; step(10);
      const u8 x = rd(h); step(7);
      h++; step(6);
      nd = rd(h); step(7);
      h++; step(6);
      DQ = h; step(16);
      const u8 k = x & 127; step(7);
      steps({17, 10, 4, 4, 4, 7, 6, 7, 4, 4});
      switch (k) {
        case 0:
          step(17);
          next_drums();
          step(12);
          continue;
        case 1: {
          step(7);
          u8 v = 0; step(7);
          u16 g = 0x0100; step(10);
          for (int i = 1; i <= 10; ++i) {
            v ^= 24; step(7);
            out(11, v);
            const u8 b = rd(g); step(7);
            hold(b);
            g++; step(6);
            step(4);
            step(i < 10 ? 12 : 7);
          }
          step(10);
          return;
        }
        case 2: {
          u16 g = 0x005A; step(10);
          for (;;) {
            u8 v = rd(g); step(7);
            step(4);
            if (v == 0) { step(11); return; }
            step(5);
            v &= 24; step(7);
            step(7);
            out(11, v);
            g++; step(6);
            step(12);
          }
        }
        case 3: {
          u16 g = 0x0F18; step(10);
          step(7);
          for (int i = 1; i <= 10; ++i) {
            u8 b = rd(g); step(7);
            hold(b);
            step(7); step(7);
            out(11, 24);
            g++; step(6);
            b = rd(g); step(7);
            hold(b);
            step(7);
            out(11, 0);
            g++; step(6);
            step(4);
            step(i < 10 ? 12 : 7);
          }
          step(10);
          return;
        }
        case 4: {
          u8 e = 63; step(7);
          step(7);
          for (int i = 1; i <= 5; ++i) {
            u8 b = e; step(4);
            hold(b);
            step(7); step(7);
            out(11, 24);
            step(4);
            e = u8(e >> 1 | e << 7); step(4);
            step(4);
            b = e; step(4);
            hold(b);
            step(7);
            out(11, 0);
            step(4);
            step(i < 5 ? 12 : 7);
          }
          step(10);
          return;
        }
        case 5:
          step(10);
          return;
        default:
          throw Unplayable{};
      }
    }
  }
};

// ---------------------------------------------------------------------------
// TMB and MSD: The Music Box and The Music Studio.

u8 box_period(int v, int top) { return v >= 0 && v <= top ? kBoxPeriod[v] : 1; }

void box_layout(const Song& s, Memory& m, bool studio) {
  m.wp = studio ? 0x80CB : 0x80CF;
  int A[256] = {};
  for (int p : s.used()) {
    const Pattern& pt = s.pat[p];
    A[p] = m.wp;
    if (studio) m.put(((64 - pt.tempo * 3) % 256 + 256) % 256);
    else m.put(((pt.tempo * 2 + 210) % 256 + 256) % 256);
    for (int i = 0; i < pt.rows; ++i) {
      if (studio) {
        m.put(box_period(pt.col[0][i], 36));
        const int d = pt.drum[i];
        m.put(d > 128 && d < 142 ? kStudioDrum[d - 128] : box_period(pt.col[1][i], 36));
      } else {
        m.put(box_period(pt.col[0][i], 52));
        m.put(box_period(pt.col[1][i], 52));
      }
    }
    m.put(studio ? 254 : 0);
  }
  for (int i = 0; i < s.length; ++i) m.put_word(A[s.order[i]]);
}

class BoxBase : public Machine {
 public:
  BoxBase(const Memory& mem, int length, Needs& needs, u16 la, int loop)
      : Machine(mem, length, needs), LA(la) {
    EV = u8(length * 2);
    LV = u8(loop * 2);
  }

 protected:
  u16 LA;
  u8 pos = 0, ro = 1, EV, LV, Z = 0;

  void next_pattern(bool tempo) {
    u8 x = pos; step(13, kMark);
    step(4); x += 2; step(4);
    step(7);
    if (x == EV) { step(17); x = LV; step(7); step(10); }
    else step(10);
    pos = x; step(13);
    steps({10, 4, 7, 11, 7, 6});
    const u16 p = rdw(u16(LA + x)); step(7);
    if (tempo) Z = rd(p);
    step(7);
    step(13); step(7);
    ro = 1; step(13);
  }
  void tick() override {}
};

class MusicBox : public BoxBase {
 public:
  using BoxBase::BoxBase;

 private:
  void run() override {
    Z = rd(rdw(LA));
    steps({4, 4, 4, 4, 4});
    step(13, kMark);
    steps({7, 13, 4, 17});
    for (;;) {
      row();
      steps({11, 7, 7, 12, 17});
    }
  }

  void row() {
    for (;;) {
      steps({10, 13, 4, 7, 11, 7, 6});
      const u16 p = rdw(u16(LA + pos)); step(7);
      steps({13, 4, 7});
      const u16 a = u16(p + ro); step(11);
      const u8 x = rd(a); step(7);
      steps({7, 4, 4});
      if (x == 0) { step(12); next_pattern(true); continue; }
      step(7);
      steps({11, 6});
      const u8 y = rd(u16(a + 1)); step(7);
      step(7);
      steps({13, 4, 4});
      ro += 2; step(13);
      steps({10, 4, 4});
      if (x != 1) { step(12); tone(x, y); return; }
      step(7);
      steps({4, 4});
      if (y == 1) { step(12); silence(); return; }
      step(7);
      tone(x, y);
      return;
    }
  }

  void tone(u8 x, u8 y) {
    u8 C = Z; step(13);
    steps({4, 7, 13, 4, 13});
    step(8, kR2);
    u8 P = 0, Q = 0, e = 1, l = 1;
    int b = 0;
    step(7);
    bool skip = false;
    for (;;) {
      if (!skip) { step(4); step(4); }
      skip = false;
      step(4);
      e--; step(4);
      out(11, P);
      if (e != 0) {
        step(12); step(7); step(4);
        l--; step(4);
        step(10);
      } else {
        step(7);
        e = y; step(8, kR2);
        P ^= 16; step(4);
        step(4);
        l--; step(4);
        step(10);
      }
      if (l != 0) {
        out(11, Q);
        step(4); step(4);
      } else {
        out(11, Q);
        l = x; step(4);
        Q ^= 16; step(4);
      }
      if (++b < 256) { step(13); continue; }
      b = 0;
      step(8);
      C++; step(4);
      if (C != 0) { step(10); skip = true; continue; }
      step(10);
      step(10);
      return;
    }
  }

  void silence() {
    u8 c = u8(255 - Z); step(13);
    steps({4, 4, 11, 11, 7});
    for (;;) {
      for (int i = 1; i <= 256; ++i) {
        steps({11, 10, 15, 15, 15, 4, 10});
        step(i < 256 ? 13 : 8);
      }
      c--; step(4);
      if (c != 0) { step(10); continue; }
      steps({10, 10, 10, 10});
      return;
    }
  }
};

class MusicStudio : public BoxBase {
 public:
  using BoxBase::BoxBase;

 private:
  struct Voice { u8 v = 0, D = 0, H = 0, L = 0, E = 0, A = 0; };

  void run() override {
    steps({4, 4, 4, 4, 4, 13, 7, 13, 4, 4});
    step(11, kMark);
    step(17);
    for (;;) {
      row();
      steps({4, 11, 7, 7, 12, 17});
    }
  }

  void setup(Voice& vo, u8 value, bool second) {
    if (second) step(4);
    vo.v = value; vo.D = value; vo.A = 0; step(8, kR2);
    vo.H = value; step(4);
    vo.L = value; step(4);
    vo.L--; step(4);
    vo.E = vo.L; step(4);
    if (vo.L == 0) step(12);
    else { step(7); vo.E = 16; step(7); }
  }

  void half(Voice& vo) {
    step(4); step(4);
    out(11, vo.A);
    vo.H--; step(4);
    if (vo.H != 0) step(12);
    else {
      step(7);
      vo.A ^= vo.E; step(4);
      vo.H = vo.D; step(4);
      step(11);
      step(8, kR2);
      step(7);
      if (vo.v >= 32) step(12);
      else { step(7); vo.D++; step(4); }
      step(10);
    }
    vo.L--; step(4);
    if (vo.L != 0) step(12);
    else {
      step(7);
      vo.A ^= vo.E; step(4);
      vo.L = vo.D; step(4);
      vo.L--; step(4);
    }
  }

  void row() {
    for (;;) {
      steps({10, 13, 4, 7, 11, 7, 6});
      const u16 p = rdw(u16(LA + pos)); step(7);
      steps({13, 4, 7});
      const u16 a = u16(p + ro); step(11);
      const u8 x = rd(a); step(7);
      steps({7, 4, 7});
      if (x == 254) { step(12); next_pattern(false); continue; }
      step(7);
      steps({11, 6});
      const u8 y = rd(u16(a + 1)); step(7);
      step(7);
      steps({13, 4, 4});
      ro += 2; step(13);
      steps({4, 10});
      u8 C = rd(p); step(13);
      steps({4, 7, 13, 4, 13, 4});
      Voice va, vb;
      setup(va, y, false);
      setup(vb, x, true);
      u8 b = 0;
      for (;;) {
        half(va);
        half(vb);
        b--;
        if (b != 0) { step(13); continue; }
        step(8);
        C--; step(4);
        if (C != 0) { step(12); continue; }
        step(7);
        step(10);
        return;
      }
    }
  }
};

// ---------------------------------------------------------------------------
// P1D and P1S: Phaser1.

u8 phaser_convert(u8 v) {
  if (v == 130) return 60;
  if (v < 60) return u8(v + 6);
  if (v >= 101 && v <= 106) return u8(v - 101);
  return v;
}

void phaser_layout(const Song& s0, Memory& m, bool digital) {
  Song s = s0;
  if (digital) {
    for (int i = 0; i < 60; ++i) { m.m[0x8254 + 2 * i] = u8(kPhaserFreq[i]); m.m[0x8255 + 2 * i] = u8(kPhaserFreq[i] >> 8); }
  } else {
    for (int i = 0; i < 60; ++i) { m.m[0x8296 + 2 * i] = u8(kPhaserFreq[i]); m.m[0x8297 + 2 * i] = u8(kPhaserFreq[i] >> 8); }
  }
  int highest = -1;
  for (int i = 0; i < s.length; ++i) {
    const Pattern& p = s.pat[s.order[i]];
    for (int r = 0; r < p.rows; ++r)
      if (p.sus[1][r] < 255) highest = std::max<int>(highest, p.sus[1][r]);
  }
  if (highest < 0) {
    s.pat[s.order[0]].sus[1][0] = 0;
    highest = 0;
  }
  highest = std::min(highest, 99);
  m.wp = 0x8700;
  m.put(s.loop * 2);
  m.put(s.length * 2);
  m.put_word((highest + 1) * 4);
  for (int i = 0; i <= highest; ++i) {
    m.put(s.ins[i].multiple);
    m.put(s.ins[i].detune & 255);
    m.put((s.ins[i].detune >> 8) & 255);
    m.put(s.ins[i].phase);
  }
  const int OL = m.wp;
  m.wp += s.length * 2;
  int A[256] = {};
  for (int pn : s.used()) {
    const Pattern& p = s.pat[pn];
    A[pn] = m.wp;
    const int w = std::max(1, 17 - p.tempo);
    int wait = 0;
    auto idle = [&](int n) {
      while (n > 117) { m.put(117); n -= 117; }
      if (n > 0) m.put(n);
    };
    for (int r = 0; r < p.rows; ++r) {
      u8 a = phaser_convert(p.col[1][r]);
      u8 b = phaser_convert(p.col[0][r]);
      int d = (p.drum[r] - 128 + 256) % 256;
      if (d > 9) d = 0;
      if (a == 255 && b == 255 && p.sus[0][r] == 255 && p.sus[1][r] == 255 && p.drum[r] == 0) {
        wait += w;
        continue;
      }
      if (wait > 0) { idle(wait); wait = 0; }
      if (a == 255) a = 62;
      if (b == 255) b = 62;
      if (p.sus[1][r] != 255) { m.put(189); m.put((p.sus[1][r] % 100) * 2); }
      a = u8(a | (p.sus[0][r] != 255 ? 192 : 128));
      b = u8(b | 128);
      m.put(a);
      if (b != 190) m.put(b);
      if (d > 0) { m.put(d + 117); wait += w - 1; }
      else wait += w;
    }
    if (wait > 0) idle(wait);
    m.put(0);
  }
  for (int i = 0; i < s.length; ++i) m.set_word(OL + 2 * i, A[s.order[i]]);
}

class Phaser : public Machine {
 public:
  Phaser(const Memory& mem, int length, Needs& needs, bool digital)
      : Machine(mem, length, needs), digital_(digital) {
    if (digital) { sample_lo_ = 0x82CC; sample_hi_ = 0x86CB; }
    table_ = digital ? 0x8254 : 0x8296;
  }

 private:
  bool digital_;
  u16 table_;
  u8 pos = 0, EV = 0, LV = 0, y = 0, v1 = 0, v2 = 0;
  u16 off = 0, LA = 0, I = 0x8704, h1 = 0, k1 = 0, s1 = 0, t1 = 0, h2 = 0, s2 = 0;

  void tick() override {}

  void run() override {
    EV = rd(0x8701);
    LV = rd(0x8700);
    LA = u16(0x8704 + rdw(0x8702));
    t_ = 342;
    step(12, kMark);
    for (;;) {
      y = 0; step(11, kR2);
      reading();
    }
  }

  // X2 onwards; returns to X1.
  void reading() {
    for (;;) {
      steps({16, 13, 4, 7, 11, 7, 6});
      const u16 p = rdw(u16(LA + pos)); step(7);
      steps({16, 6});
      const u16 a = u16(p + off);
      off++; step(16);
      step(6);
      step(11);
      u8 x = rd(a); step(7);
      step(4);
      if (x == 0) { step(12); pattern_end(); return; }
      step(7);
      step(8, kR2);
      step(10);
      if (x < 128) {
        x &= 127; step(7);
        step(7);
        if (x < 118) tone(x);
        else if (digital_) sampled_drum(x);
        else made_drum(x);
        return;
      }
      step(8, kR2);
      const u8 k = x & 63; step(7);
      step(7);
      step(10);
      if (k >= 60) {
        step(7);
        if (k == 60) {
          steps({12, 10}); step(8, kR2); step(4);
          if (y == 0) {
            step(7);
            s1 = 0; step(16);
            t1 = 0; step(16);
            step(10);
            v1 = 0; step(15, kR2);
            y = 1; step(11, kR2);
            step(10);
          } else {
            step(12);
            s2 = 0; step(16);
            step(10);
            v2 = 0; step(15, kR2);
            step(10);
          }
          continue;
        }
        step(7); step(7);
        if (k == 62) {
          step(12);
          y = 1; step(11, kR2);
          step(10);
          continue;
        }
        step(7); step(6);
        const u8 q = rd(u16(a + 1)); step(7);
        step(7); step(11); step(20, kR2);
        off++; step(6);
        step(20, kR2); step(10); step(11);
        I = u16(0x8704 + 2 * q); step(16);
        step(10);
        continue;
      }
      steps({4, 7, 4, 10, 11});
      step(7); step(6);
      const u16 s = rdw(u16(table_ + 2 * k)); step(7);
      step(8, kR2); step(4);
      if (y == 0) {
        step(7);
        s1 = s; step(20, kR2);
        u16 z = s; step(4);
        step(14, kR2);
        const u8 mul = rd(I); step(19, kR2);
        step(4);
        if (mul == 0) step(12);
        else {
          step(7); step(4);
          for (int i = 1; i <= mul; ++i) {
            z = u16(z * 2); step(11);
            step(i < mul ? 13 : 8);
          }
        }
        step(19, kR2); step(19, kR2);
        z = u16(z + rdw(u16(I + 1))); step(11);
        t1 = z; step(16);
        y = 1; step(11, kR2);
        step(8, kR2); step(7);
        if (x & 64) {
          step(7); step(10);
          v1 = 0; step(15, kR2);
          step(10);
          h1 = 0; step(16);
          step(19, kR2);
          k1 = u16(rd(u16(I + 3)) << 8); step(16);
          step(12);
        } else step(12);
      } else {
        step(12);
        s2 = s; step(20, kR2);
        step(8, kR2); step(10);
        v2 = 0; step(15, kR2);
        step(10);
        h2 = 0; step(16);
        step(10);
      }
    }
  }

  void pattern_end() {
    u8 v = pos; step(13, kMark);
    step(4); v += 2; step(4);
    step(7);
    if (v == EV) { step(7); v = LV; step(7); }
    else step(12);
    pos = v; step(13);
    step(10);
    off = 0; step(16);
  }

  void unit() {
    step(4); step(4);
    unsigned sum = unsigned(h1) + s1;
    h1 = u16(sum); step(11);
    bool c = sum > 0xFFFF;
    out(11, v1);
    if (c) { step(12); v1 ^= 16; step(7); }
    else { step(7); step(12); }
    sum = unsigned(k1) + t1;
    k1 = u16(sum); step(15, kR2);
    c = sum > 0xFFFF;
    if (c) { step(12); v1 ^= 16; step(7); }
    else { step(7); step(12); }
    step(4);
    out(11, v2);
    step(4);
    sum = unsigned(h2) + s2;
    h2 = u16(sum); step(11);
    c = sum > 0xFFFF;
    if (c) { step(12); v2 ^= 16; step(7); }
    else { step(7); step(12); }
  }

  void tone(u8 n) {
    steps({10, 4, 4, 10}); step(14, kR2); steps({10, 10, 7, 4, 4, 10, 10, 7});
    for (int i = 0; i < n; ++i) {
      steps({4, 4, 11, 7, 4, 7, 4, 7});
      for (int u = 0; u < 1025; ++u) {
        unit();
        if (u < 1024) { step(4); step(10); }
      }
      step(4); step(10);
    }
    steps({16, 13, 4, 4, 16}); step(20, kR2); steps({13, 10});
  }

  void sampled_drum(u8 x) {
    step(10);
    const int B = x - 116; step(7);
    step(4);
    u8 m = 128, c = 0; step(7);
    for (int i = 1; i <= B; ++i) {
      const u8 nc = m >> 7;
      m = u8(m << 1 | c);
      c = nc;
      step(4);
      step(i < B ? 13 : 8);
    }
    steps({13, 7, 4, 10, 10});
    for (int j = 0; j < 1024; ++j) {
      const u8 z = m ? u8(rd(u16(0x82CC + j)) & m) : 0; step(7);
      step(7); step(4);
      if (z) { step(12); step(7); } else { step(7); step(12); }
      out(11, z ? 16 : 0);
      step(7);
      count(4);
      steps({6, 6, 4, 4});
      step(j < 1023 ? 12 : 7);
    }
    step(10);
  }

  void made_drum(u8 x) {
    steps({10, 4, 7, 4, 10, 11, 7, 6, 7, 4, 4});
    if (x <= 123) {
      static const u8 kStep[6] = {16, 12, 8, 6, 4, 2};
      const u8 L = kStep[x - 118];
      step(7);
      if (x != 123) step(12);
      steps({10, 10, 7});
      u8 A = 0, B = 1, C = 1, E = 116, D = 14;
      for (;;) {
        out(11, A);
        B--; step(4);
        if (B != 0) step(12);
        else {
          step(7);
          A ^= 16; step(7);
          B = C; step(4);
          step(4); step(4);
          C = u8(C + L); step(4);
          step(4); step(4);
        }
        E--; step(4);
        if (E != 0) { step(12); continue; }
        step(7);
        D--; step(4);
        if (D != 0) { step(12); continue; }
        step(7);
        step(10);
        return;
      }
    }
    if (x > 126) throw Unplayable{};
    static const u16 kStart[3] = {0x09B0, 0x042E, 0x016D};
    static const u8 kHold[3] = {1, 10, 101};
    u16 g = kStart[x - 124];
    const u8 k = kHold[x - 124];
    step(10); step(11, kR2);
    if (x != 126) step(12);
    steps({4, 4, 7, 4});
    u8 E = u8(g), D = u8(g >> 8);
    for (;;) {
      const u8 z = rd(g) & 16; step(7);
      step(7); step(4);
      out(11, z);
      step(8, kR2);
      hold(k);
      g++; step(6);
      E--; step(4);
      if (E != 0) { step(12); continue; }
      step(7);
      D--; step(4);
      if (D != 0) { step(12); continue; }
      step(7);
      step(10);
      return;
    }
  }
};

// ---------------------------------------------------------------------------
// SVG: Savage.

u8 savage_convert(u8 v) {
  if (v < 60) v = u8(v + 6);
  if (v >= 101 && v <= 106) v = u8(v - 101);
  if (v == 130) v = 128;
  return v;
}

// A column stream of the Savage layout; `notes` gets every note byte written.
std::vector<u8> savage_column(const Pattern& p, int c, std::vector<int>* notes) {
  std::vector<u8> out;
  int note = 128, len = 0, last = -1;
  auto put = [&](int n, int l) {
    while (l > 32) {
      out.push_back(255);
      out.push_back(u8(n));
      if (notes) notes->push_back(n);
      l -= 32;
    }
    if (l > 0) {
      if (l != last) {
        out.push_back(u8(223 + l));
        out.push_back(u8(n));
        last = l;
      } else out.push_back(u8(n));
      if (notes) notes->push_back(n);
    }
  };
  for (int i = 0; i < p.rows; ++i) {
    const u8 v = p.col[c][i];
    if (v == 255 && p.glide[c][i] == 256 && p.skew[c][i] == 256 && p.skewchg[c][i] == 256 &&
        p.orn[c][i] == 256 && p.warp[c][i] == 0) {
      len++;
      continue;
    }
    if (len > 0) put(note, len);
    if (p.warp[c][i] != 0) out.push_back(134);
    if (p.glide[c][i] < 256) { out.push_back(129); out.push_back(u8(p.glide[c][i])); }
    if (p.skew[c][i] < 256) { out.push_back(133); out.push_back(u8(p.skew[c][i])); }
    if (p.skewchg[c][i] < 256) { out.push_back(135); out.push_back(u8(p.skewchg[c][i])); }
    if (p.orn[c][i] < 32) out.push_back(u8(p.orn[c][i] + 192));
    note = savage_convert(v == 255 ? u8(note) : v);
    len = 1;
  }
  if (len > 0) put(note, len);
  out.push_back(130);
  return out;
}

void savage_check(const Song& s) {
  for (int p : s.used())
    for (int c = 0; c < 2; ++c) {
      std::vector<int> notes;
      savage_column(s.pat[p], c, &notes);
      for (int n : notes)
        if (n == 131 || (n >= 136 && n <= 191)) throw Refused(kCantPlay);
    }
}

void savage_layout(const Song& s, Memory& m) {
  for (int i = 0; i < 60; ++i) { m.m[0x80B0 + 2 * i] = u8(kSavageFreq[i]); m.m[0x80B1 + 2 * i] = u8(kSavageFreq[i] >> 8); }
  m.wp = 36000;
  int V1[256] = {}, V2[256] = {}, DR[256] = {};
  for (int pn : s.used()) {
    const Pattern& p = s.pat[pn];
    const int tb = std::max(1, 21 - p.tempo) & 255;
    V1[pn] = m.wp;
    m.put(tb);
    m.put_all(savage_column(p, 0, nullptr));
    V2[pn] = m.wp;
    m.put(tb);
    m.put_all(savage_column(p, 1, nullptr));
    DR[pn] = m.wp;
    int n = 0;
    for (int i = 0; i < p.rows; ++i) {
      const int d = p.drum[i];
      if (d >= 129 && d <= 133) {
        if (n > 0) m.put(n);
        m.put(d - 1);
        n = 0;
      } else n++;
    }
    if (n > 0) m.put(n);
    m.put(0);
  }
  if (m.wp > 0xFE00) throw Refused(kNoFit);
  m.wp = 35000;
  for (int k = 0; k < 3; ++k) {
    m.put(s.length * 2);
    m.put(s.loop * 2);
    m.wp += 2;
  }
  int h = 0;
  for (int i = 0; i < s.length; ++i) {
    const Pattern& p = s.pat[s.order[i]];
    for (int r = 0; r < p.rows; ++r)
      for (int c = 0; c < 2; ++c)
        if (p.orn[c][r] < 32) h = std::max<int>(h, p.orn[c][r]);
  }
  h = std::min(h, 31);
  m.set_word(0x82A2, m.wp);
  u8 o = 0;
  for (int i = 0; i <= h; ++i) {
    m.put(o);
    o = u8(o + s.orn[i].length + 1);
  }
  m.set_word(0x82EF, m.wp);
  m.put(128);
  for (int i = 1; i <= h; ++i) {
    for (int j = 0; j < s.orn[i].length; ++j) m.put(s.orn[i].v[j]);
    m.put(255);
  }
  const int* lists[3] = {V1, V2, DR};
  for (int k = 0; k < 3; ++k) {
    m.set_word(35002 + 4 * k, m.wp);
    for (int i = 0; i < s.length; ++i) m.put_word(lists[k][s.order[i]]);
  }
  if (m.wp > 36000) throw Refused("the song's list of patterns is too long for the Savage engine");
}

struct BackToLoop {};

class Savage : public Machine {
 public:
  Savage(const Memory& mem, int length, Needs& needs) : Machine(mem, length, needs) {
    tail_lo_ = 0x8128;
    tail_hi_ = 0x81AF;
  }

 private:
  struct Voice {
    u16 ptr = 0, list = 0;
    u8 pos = 0, endv = 0, loopv = 0, transpose = 0, rows = 1, notelen = 1, glide = 0, skew = 0,
       on = 16, ornbase = 0, ornstep = 0, note = 0, skewchg = 0;
  };
  struct Bank { u8 B = 0, C = 0, D = 0, E = 0, H = 0, L = 0; };
  enum Target { kPlain, kOrnament };

  Voice vo[2];
  u16 dptr = 0, dlist = 0, OO = 0, OD = 0;
  u8 dpos = 0, dend = 0, dloop = 0, dc = 1, tempo = 0, tc = 0, on1v = 0, on2v = 0;
  Target target = kPlain;
  Bank bank[2];
  u8 acc[2] = {0, 0};
  int cb = 0, ca = 0;

  Bank& K() { return bank[cb]; }
  u8& A() { return acc[ca]; }

  void run() override {
    for (int i = 0; i < 2; ++i) {
      Voice& v = vo[i];
      v.endv = rd(u16(35000 + 4 * i));
      v.loopv = rd(u16(35001 + 4 * i));
      v.list = rdw(u16(35002 + 4 * i));
      v.ptr = rdw(v.list);
      v.note = i == 0 ? 11 : 18;
    }
    dend = rd(35008);
    dloop = rd(35009);
    dlist = rdw(35010);
    dptr = rdw(dlist);
    OO = rdw(0x82A2);
    OD = rdw(0x82EF);
    t_ = 6969;
    refresh_ = 59;
    u16 h = vo[0].ptr; step(16);
    tempo = rd(h); step(7);
    h++; step(6);
    step(13);
    vo[0].ptr = h; step(16);
    h = vo[1].ptr; step(16);
    h++; step(6);
    vo[1].ptr = h; step(16);
    step(12);
    step(17);
    drums();
    quantum1();
    tone_loop();
  }

  void tone_loop() {
    for (;;) {
      try {
        for (;;) {
          // G1
          cb ^= 1; step(4);
          ca ^= 1; step(4);
          K().B--;
          if (K().B != 0) {
            step(13);
            step(4);
            const int c = A() >> 7; step(4);
            if (c == 0) { step(12); step(10); }
            else {
              step(7);
              A() = u8(A() + 128); step(7);
              K().H = K().L; step(4);
              K().L = K().B; step(4);
            }
          } else {
            step(8);
            std::swap(K().D, K().H); std::swap(K().E, K().L); step(4);
            add_hl(); step(11);
            K().B = K().H; step(4);
            A() ^= on1v; step(7);
            step(10);
          }
          // G2
          out(11, A());
          cb ^= 1; step(4);
          ca ^= 1; step(4);
          K().B--;
          if (K().B != 0) {
            step(13);
            step(4);
            const int c = A() >> 7; step(4);
            if (c == 0) { step(12); out(11, A()); step(10); }
            else {
              step(7);
              const u16 hl = u16((K().H << 8 | K().L) - 1);
              K().H = u8(hl >> 8); K().L = u8(hl); step(6);
              step(10); step(10);
            }
          } else {
            step(8);
            std::swap(K().D, K().H); std::swap(K().E, K().L); step(4);
            add_hl(); step(11);
            K().B = K().H; step(4);
            A() ^= on2v; step(7);
            out(11, A());
            step(10);
          }
        }
      } catch (const BackToLoop&) {
      }
    }
  }

  void add_hl() {
    const u16 hl = u16((K().H << 8 | K().L) + (K().B << 8 | K().C));
    K().H = u8(hl >> 8);
    K().L = u8(hl);
  }

  void key() { steps({4, 11, 4, 7, 7, 11, 7, 11}); }

  void tick() override {
    step(12); step(10);
    if (target == kPlain) {
      tc--; step(8, kR2);
      if (tc != 0) { step(7); step(4, kOn); step(10); return; }
      steps({12, 11, 11, 17});
      key();
      step(17);
      drums();
      step(10);
      vo[0].rows--; step(11);
      if (vo[0].rows == 0) {
        step(12); step(14, kR2); step(17);
        row(0);
        quantum2();
        throw BackToLoop{};
      }
      step(7); step(10);
      vo[1].rows--; step(11);
      if (vo[1].rows == 0) {
        step(12); step(10); step(14, kR2); step(17);
        row(1);
        quantum3();
        throw BackToLoop{};
      }
      step(7); step(13);
      tc = tempo; step(8, kR2);
      step(10); step(10);
      step(4, kOn);
      step(10);
      return;
    }
    tc--; step(8, kR2);
    if (tc != 0) {
      step(7); step(20, kR2); step(15, kR2); step(17);
      both();
      step(14, kR2); step(12);
      step(4, kOn);
      throw BackToLoop{};
    }
    step(12); step(17);
    key();
    step(17);
    drums();
    quantum1();
    throw BackToLoop{};
  }

  void drums() {
    step(10);
    dc--; step(11);
    if (dc != 0) { step(11); return; }
    step(5);
    u16 g = dptr; step(16);
    u8 x = rd(g); step(7);
    g++; step(6);
    step(4);
    if (x == 0) {
      steps({7, 11, 11, 4});
      u8 v = dpos; step(16);
      step(4);
      v += 2; step(7);
      step(4);
      if (v != dend) step(12);
      else { step(7); v = dloop; step(13); }
      dpos = v; step(13);
      step(4);
      g = dlist; step(16);
      g = u16(g + v); step(11);
      step(7); step(6); g = rdw(g); step(7);
      step(4);
      x = rd(g); step(7);
      g++; step(6);
      steps({4, 10, 10});
    } else step(12);
    dptr = g; step(16);
    if (x >= 128) {
      step(10); step(10);
      K().B = dc; step(7);
      dc++; step(11);
      steps({11, 11, 4, 10, 11, 7, 11, 13, 4});
      noise(x - 128);
      return;
    }
    step(10);
    dc = x; step(13);
    step(10);
  }

  void noise(int k) {
    static const u8 kCount[5] = {32, 207, 40, 192, 224};
    static const u8 kMask[5] = {15, 63, 63, 161, 31};
    if (k > 4) throw Unplayable{};
    const u8 mask = kMask[k];
    step(10);
    if (k != 2) step(12);
    step(10); step(12);
    u8 a = 0, h = 1, l = 74, b = kCount[k], e = 0;
    goto Z2;
  Z1:
    e = 8; step(7);
  Z1b:
    h--; step(4);
    if (h != 0) {
      step(12);
      e--; step(4);
      if (e != 0) { step(12); goto Z1b; }
      step(7);
      goto Z3;
    }
    step(7);
    a ^= 16; step(7);
  Z2:
    out(11, a);
    step(4);
    {
      u8 z = u8((refresh_ + 2) & 127); step(9, kR2);
      z ^= l; step(4);
      z = u8(z << 1 | z >> 7); step(4);
      l = z; step(4);
      z &= mask; step(4);
      z++; step(4);
      h = z; step(4);
    }
    step(4); step(12);
  Z3:
    b--;
    if (b != 0) { step(13); goto Z1; }
    step(8);
    steps({10, 10, 10});
  }

  void row(int i) {
    Voice& v = vo[i];
    step(7);
    v.glide = 0; step(19, kR2);
    v.on = 16; step(19, kR2);
    step(19, kR2);
    u16 g = v.ptr; step(19, kR2);
    u8 x;
    for (;;) {
      x = rd(g); step(7);
      g++; step(6);
      step(7);
      if (x < 192) {
        step(12); step(4);
        if (x < 128) { step(10); break; }
        steps({10, 4, 10, 11, 7, 11, 4});
        u8 z;
        switch (x - 128) {
          case 0:
            v.on = 0; step(19, kR2);
            goto note;
          case 1:
            z = rd(g); step(7); g++; step(6); v.glide = z; step(19, kR2); step(12);
            continue;
          case 2: {
            u8 p = v.pos; step(19, kR2 | (i == 0 ? unsigned(kMark) : 0u));
            p += 2; step(7);
            step(19, kR2);
            if (p != v.endv) step(12);
            else { step(7); p = v.loopv; step(19, kR2); }
            v.pos = p; step(19, kR2);
            step(19, kR2);
            u16 h = v.list; step(19, kR2);
            step(4);
            h = u16(h + p); step(11);
            step(7); step(6); g = rdw(h); step(7);
            z = rd(g); step(7);
            tempo = z; step(13);
            g++; step(6);
            step(12);
            continue;
          }
          case 4:
            z = rd(g); step(7); g++; step(6); v.transpose = z; step(19, kR2); step(12);
            continue;
          case 5:
            z = rd(g); step(7); g++; step(6); v.skew = z; step(19, kR2); step(12);
            continue;
          case 6:
            v.on = 144; step(19, kR2); step(12);
            continue;
          case 7:
            z = rd(g); step(7); g++; step(6); v.skewchg = z; step(19, kR2); step(12);
            continue;
          default:
            throw Unplayable{};
        }
      }
      step(7);
      u8 z = u8(x + 32); step(7);
      if (z < 32) {
        step(12);
        z++; step(4);
        v.notelen = z; step(19, kR2);
        step(12);
        continue;
      }
      step(7);
      z = u8(z + 32); step(7);
      steps({4, 10, 11});
      z = rd(u16(OO + z)); step(7);
      v.ornbase = z; step(19, kR2);
      step(12);
    }
  note:
    v.note = x; step(19, kR2);
    v.ornstep = 0; step(19, kR2);
    step(19, kR2);
    v.rows = v.notelen; step(19, kR2);
    step(19, kR2);
    v.ptr = g; step(19, kR2);
    step(10);
  }

  void quantum1() {
    step(10);
    vo[0].rows--; step(11);
    step(14, kR2);
    if (vo[0].rows == 0) { step(17); row(0); }
    else step(10);
    quantum2();
  }
  void quantum2() {
    step(10);
    vo[1].rows--; step(11);
    step(10); step(14, kR2);
    if (vo[1].rows == 0) { step(17); row(1); }
    else step(10);
    quantum3();
  }
  void quantum3() {
    step(10);
    u8 z = vo[1].ornbase; step(13);
    z |= vo[0].ornbase; step(7);
    step(10);
    if (z == 0) { step(12); target = kPlain; }
    else { step(7); step(10); target = kOrnament; }
    step(16);
    on1v = vo[0].on; step(13);
    step(13);
    on2v = vo[1].on; step(13);
    step(13);
    step(17);
    both();
    tc = tempo; step(11, kR2);
    step(4, kOn);
  }

  void both() {
    step(14, kR2); step(17);
    setup(vo[0]);
    cb ^= 1; step(4);
    ca ^= 1; step(4);
    step(14, kR2);
    setup(vo[1]);
  }

  void setup(Voice& v) {
    step(19, kR2); step(19, kR2);
    v.skew ^= v.skewchg; step(19, kR2);
    step(4);
    K().D = 0; step(4);
    step(19, kR2); step(10); step(11); step(19, kR2);
    const u16 q = u16(OD + v.ornbase + v.ornstep); step(11);
    u8 o = rd(q); step(7);
    step(4);
    u8 e;
    if (o < 128) { step(10); e = v.ornstep; goto S3b; }
    step(10);
    o++; step(4);
    if (o == 0) {
      step(7); step(10);
      target = kPlain; step(16);
      step(12);
      goto S4;
    }
    step(12);
    o--; step(4);
    o &= 127; step(7);
    e = 255; step(7);
  S3b:
    e++; step(4);
    v.ornstep = e; step(19, kR2);
  S4:
    step(4);
    o = u8(o + v.note); step(19, kR2);
    o = u8(o + v.transpose); step(19, kR2);
    o = u8(o + o); step(4);
    step(10); step(4);
    {
      const u16 f = u16(0x80B0 + o); step(11);
      const u8 f1 = rd(f); step(7);
      step(6);
      const u8 f2 = rd(u16(f + 1)); step(7);
      const u8 s = v.skew; step(19, kR2);
      step(4);
      u8 l = 0; step(4);
      if (s == 0) step(12);
      else {
        step(7);
        int c = s >> 7;
        const u8 m = u8(s << 1); step(4);
        u16 w = u16(m << 8); step(4);
        if (c == 0) step(12);
        else { step(7); w = u16(w + f1); step(11); }
        for (int i = 0; i < 7; ++i) {
          c = w >> 15;
          w = u16(w << 1); step(11);
          if (c == 0) step(12);
          else { step(7); w = u16(w + f1); step(11); }
        }
        l = u8(w >> 8); step(4);
      }
      u8 z = f2; step(4);
      z = u8(z + l); step(4);
      K().H = z; step(4);
      z = f1; step(4);
      z = u8(z - l); step(4);
      K().L = 0; step(4);
      K().E = 0; step(4);
      K().D = z; step(4);
      K().C = v.glide; step(19, kR2);
      A() = 0; step(7);
      K().B = K().H; step(4);
      step(10);
    }
  }
};

// ---------------------------------------------------------------------------
// RMB: ROM Beep.

u8 beep_byte(u8 v) {
  if (v < 60) v = u8(v + 6);
  if (v >= 101 && v <= 106) v = u8(v - 101);
  if (v >= 128) return 1;
  return u8(v + 60);
}

void beep_check(const Song& s) {
  for (int pn : s.used()) {
    const Pattern& p = s.pat[pn];
    for (int r = 0; r < p.rows; ++r)
      for (int c = 0; c < 2; ++c)
        if (beep_byte(p.col[c][r]) >= 130) throw Refused(kCantPlay);
  }
}

void beep_layout(const Song& s, Memory& m) {
  m.wp = 0x8700;
  m.put(s.loop * 2);
  m.put(s.length * 2);
  m.wp += s.length * 2;
  int A[256] = {};
  for (int pn : s.used()) {
    const Pattern& p = s.pat[pn];
    A[pn] = m.wp;
    m.put(std::max(1, 17 - p.tempo) & 255);
    for (int r = 0; r < p.rows; ++r) {
      m.put(beep_byte(p.col[0][r]));
      m.put(beep_byte(p.col[1][r]));
    }
    m.put(0);
  }
  for (int i = 0; i < s.length; ++i) m.set_word(0x8702 + 2 * i, A[s.order[i]]);
}

class RomBeep : public Machine {
 public:
  RomBeep(const Memory& mem, int length, Needs& needs) : Machine(mem, length, needs) { tick_cost_ = 13; }

 private:
  u8 pos = 254, ro = 1, count_ = 0, EV = 0, LV = 0;
  const u16 LA = 0x8702;

  void run() override {
    EV = rd(0x8701);
    LV = rd(0x8700);
    steps({10, 7, 13, 6, 7, 13, 6, 16, 16, 7, 13, 17});
    next_pattern();
    row();
    step(8, kR2);
    step(4, kOn);
    for (;;) {
      step(17);
      row();
      steps({4, 11, 7, 7, 12});
    }
  }

  void tick() override {
    step(868);
    step(4, kOn);
    step(10);
  }

  void next_pattern() {
    u8 z = pos; step(13, kMark);
    step(4); z += 2; step(4);
    step(7);
    if (z != EV) step(12);
    else { step(7); z = LV; step(7); }
    pos = z; step(13);
    steps({10, 4, 7, 11, 7, 6});
    const u16 p = rdw(u16(LA + z)); step(7);
    count_ = rd(p); step(7);
    step(13); step(7);
    ro = 1; step(13);
  }

  void row() {
    for (;;) {
      steps({10, 13, 4, 7, 11, 7, 6});
      const u16 p = rdw(u16(LA + pos)); step(7);
      steps({13, 4, 7});
      const u16 a = u16(p + ro); step(11);
      const u8 x = rd(a); step(7);
      step(4); step(4);
      if (x == 0) { step(12); next_pattern(); continue; }
      step(7);
      step(6);
      const u8 y = rd(u16(a + 1)); step(7);
      steps({13, 4, 4});
      ro += 2; step(13);
      step(13);
      u8 b = count_; step(4);
      for (;;) {
        steps({11, 11, 4, 17});
        beep_or_rest(x);
        steps({10, 11, 4, 17});
        beep_or_rest(y);
        steps({10, 10});
        b--;
        if (b != 0) { step(13); continue; }
        step(8);
        step(10);
        return;
      }
    }
  }

  void beep_or_rest(u8 v) {
    if (v == 1) {
      step(4); step(12);
      wait_step();
      wait_step();
      step(7);
      for (int i = 1; i <= 255; ++i) { step(7); step(i < 255 ? 13 : 8); }
      step(10);
      return;
    }
    if (v < 60 || v > 129) throw Unplayable{};
    const BeepTiming& bt = kBeep[v - 60];
    const i64 s = t_;
    i64 off_at = s + bt.prep;
    if (next_frame_ <= s + bt.prep - 10) {
      const i64 due = std::max(next_frame_, s);
      next_frame_ += 69888;
      if (ticks_on_) {
        off_at += 895;
        ++refresh_;
        if (log) {
          log->inside.push_back(log->ticks.size());
          log->ticks.push_back(due + 13);
        }
      }
    }
    t_ = off_at;
    if (sound) sound->advance(t_);
    step(4, kOff);
    i64 e = 0;
    for (int j = 0; j < bt.edges; ++j) {
      e = off_at + bt.first + i64(j) * bt.half;
      const int lv = (j & 1) ? 0 : 1;
      if (lv != level_) change(e, lv);
      if (sound) sound->advance(e);
    }
    t_ = e + 43;
    while (next_frame_ <= t_) next_frame_ += 69888;
    step(4, kOn);
    step(10);
    step(10);
  }
};

// ---------------------------------------------------------------------------
// PLP: Plip Plop.

bool plip_is_drum(int d) { return d >= 129 && d <= 132; }

void plip_layout(const Song& s, Memory& m) {
  for (int i = 0; i < 49; ++i) { m.m[0x8038 + 2 * i] = u8(kPlipFreq[i]); m.m[0x8039 + 2 * i] = u8(kPlipFreq[i] >> 8); }
  bool hasdrums = false;
  for (int i = 0; i < s.length; ++i) {
    const Pattern& p = s.pat[s.order[i]];
    for (int r = 0; r < p.rows; ++r)
      if (p.drum[r] != 0) hasdrums = true;
  }
  m.set(0x8700, 175);
  const int CL = 0x8701;
  m.wp = CL + s.length * 3 + 3;
  int A[256] = {};
  std::vector<int> empty_slots;
  for (int pn : s.used()) {
    const Pattern& p = s.pat[pn];
    const int row = 22 - p.tempo;
    A[pn] = m.wp;
    int first = -1;
    for (int r = 0; r < p.rows && first < 0; ++r)
      if (plip_is_drum(p.drum[r])) first = r;
    int slot = 0;
    if (first >= 0) {
      m.put(59);
      m.put(first == 0 ? 1 : (first * row) & 255);
      slot = m.wp;
      m.put_word(0);
    } else if (hasdrums) {
      m.put(59);
      m.put(255);
      empty_slots.push_back(m.wp);
      m.put_word(0);
    }
    auto put = [&](int note, int frames, int drums, int bend) {
      int n = note == 130 ? 0 : note >= 104 ? note - 103 : note + 4;
      if (n > 48) n = 0;
      if (bend > 0 && bend < 200) frames -= drums;
      auto piece = [&](int len) {
        if (bend > 0 && bend <= 100) { m.put(61); m.put(n); m.put(bend); m.put(len); }
        else if (bend > 100 && bend <= 200) { bend = 355 - bend; m.put(61); m.put(n); m.put(bend); m.put(len); }
        else { m.put(n); m.put(len); }
      };
      while (frames > 255) { piece(255); frames -= 255; }
      if (frames > 0) piece(frames);
    };
    int note = 130, frames = 0, bend = 0, drums = 0;
    for (int r = 0; r < p.rows; ++r) {
      if (bend > 0 && bend < 200 && plip_is_drum(p.drum[r])) drums++;
      if (p.col[0][r] == 255) { frames += row; continue; }
      if (frames > 0) { put(note, frames, drums, bend); drums = 0; }
      note = p.col[0][r];
      bend = p.sus[0][r];
      frames = row;
      if (bend > 0 && bend < 200 && plip_is_drum(p.drum[r])) drums++;
    }
    if (frames > 0) put(note, frames, drums, bend);
    m.put(255);
    if (first >= 0) {
      m.set_word(slot, m.wp);
      int d = 129;
      frames = 0;
      auto drumput = [&](int dv, int f) {
        const int kind = dv == 129 ? 32 : dv == 130 ? 64 : dv == 131 ? 96 : 0;
        while (f > 31) { m.put(kind + 31); f -= 31; }
        if (f > 0) m.put(kind + f);
      };
      for (int r = first; r < p.rows; ++r) {
        if (p.drum[r] < 129) frames += row;
        else {
          if (frames > 0) drumput(d, frames);
          d = p.drum[r];
          frames = row;
        }
      }
      if (frames > 0) drumput(d, frames);
      m.put(138);
    }
  }
  if (!empty_slots.empty()) {
    const int at = m.wp;
    m.put(138);
    for (int sl : empty_slots) m.set_word(sl, at);
  }
  const int end = m.wp;
  m.wp = CL;
  int L = 0;
  for (int i = 0; i < s.length; ++i) {
    if (i == s.loop) L = m.wp;
    m.put(49);
    m.put_word(A[s.order[i]]);
  }
  m.put(51);
  m.put_word(L);
  m.wp = end;
}

struct NoteOver {};
struct DrumOver {};

class PlipPlop : public Machine {
 public:
  using Machine::Machine;

 private:
  enum Target { kNormal, kDrumEnd };
  u16 ptr = 0x8701, DP = 0, CP = 670, TG = 0;
  u8 L = 0, S = 0, don = 0, dw = 1;
  Target target = kNormal;

  void run() override {
    t_ = 5898;
    try {
      interpret(0);
    } catch (const NoteOver&) {
      throw Unplayable{};
    } catch (const DrumOver&) {
      throw Unplayable{};
    }
  }

  void interpret(int depth) {
    if (depth > 64) throw Unplayable{};
    for (;;) {
      const u8 x = rd(ptr); step(7);
      ptr++; step(6);
      step(7);
      if (x == 255) { step(11); return; }
      steps({5, 7, 4, 10});
      if (x >= 49) {
        step(7);
        command(x, depth);
        continue;
      }
      step(12); step(4);
      if (x != 0) {
        step(7);
        u8 z = x; step(7);
        z = u8(z + z); step(4);
        steps({10, 7, 4, 11});
        step(7); step(6);
        const u16 b = rdw(u16(0x8038 + z)); step(7);
        CP = b; step(20, kR2);
        step(4);
        u8 f = rd(ptr); step(7);
        ptr++; step(6);
        step(4);
        f--; step(4);
        L = f; step(4);
        step(4, kOn);
        try {
          step(17);
          buzz(b);
        } catch (const NoteOver&) {
        }
        L++; step(6);
        step(12);
      } else {
        step(12); step(4);
        const u8 f = rd(ptr); step(7);
        ptr++; step(6);
        step(4);
        L = f; step(4);
      }
      try {
        step(17);
        wait_note();
      } catch (const NoteOver&) {
      }
      step(4, kOff);
      step(4); step(12);
    }
  }

  void command(u8 x, int depth) {
    steps({7, 7, 4, 11, 7, 6, 7, 4, 17, 4});
    switch (x) {
      case 49: {
        step(4, kMark);
        const u8 lo = rd(ptr); step(7);
        ptr++; step(6);
        step(11);
        const u8 hi = rd(ptr); step(7);
        const u16 kept = ptr;
        ptr = u16(hi << 8 | lo); step(4);
        step(17);
        interpret(depth + 1);
        ptr = kept; step(10);
        ptr++; step(6);
        step(10);
        break;
      }
      case 51: {
        step(4);
        const u8 lo = rd(ptr); step(7);
        ptr++; step(6);
        const u8 hi = rd(ptr); step(7);
        step(4);
        ptr = u16(hi << 8 | lo); step(10);
        break;
      }
      case 59:
        step(4);
        dw = rd(ptr); step(7);
        ptr++; step(6);
        step(13);
        step(7); step(6); step(7);
        DP = rdw(ptr); ptr += 2; step(6);
        step(20, kR2);
        step(7);
        don = 255; step(13);
        step(10);
        break;
      case 61:
        bend();
        break;
      default:
        throw Unplayable{};
    }
    step(12);
  }

  void bend() {
    step(4);
    const u8 n = rd(ptr); step(7);
    ptr++; step(6);
    const u8 rate = rd(ptr); step(7);
    ptr++; step(6);
    u8 k = rd(ptr); step(7);
    ptr++; step(6);
    steps({11, 4, 10});
    const u8 z = u8(n + n); step(4);
    steps({4, 7, 10, 11});
    step(7); step(6);
    TG = rdw(u16(0x8038 + z)); step(7);
    step(20, kR2);
    for (;;) {
      step(11);
      u16 w = CP; step(16);
      step(4); step(4);
      int c, u;
      if (rate >= 128) {
        step(10);
        steps({4, 4, 4, 4, 7});
        w = u16(w + rate - 256); step(4);
        c = 0; step(4);
        u = 0; step(13);
      } else {
        step(10);
        steps({4, 4, 4, 7});
        const int sum = w + rate;
        c = sum > 0xFFFF;
        w = u16(sum); step(4);
        step(7);
        u = 1; step(13);
        step(12);
      }
      steps({4, 4, 16});
      c = (int(TG) - int(w) - c) < 0; step(15, kR2);
      if (u) c = 1 - c;
      step(4);
      if (c == 1) step(12);
      else { step(7); w = TG; step(20, kR2); }
      CP = w; step(20, kR2);
      L = 1; step(7);
      try {
        step(17);
        buzz(w);
      } catch (const NoteOver&) {
      }
      step(4);
      out(11, 0);
      step(4, kOff);
      step(10);
      k--;
      if (k != 0) { step(13); continue; }
      step(8);
      step(4); step(10);
      return;
    }
  }

  void buzz(u16 b) {
    step(4, kOn);
    step(4);
    S = 0; step(4);
    step(4); step(4);
    u16 e = u16(b - 1); step(6);
    const u16 Rb = b; step(20, kR2);
    const u16 Re = e; step(20, kR2);
    step(8, kR2); e = u16(e >> 1); step(8, kR2);
    u16 bc = b;
    for (;;) {
      bc--; step(6);
      step(4); step(4);
      if (bc != 0) step(10);
      else {
        step(10);
        bc = Rb; step(10);
        step(4);
        S = u8(S + 16); step(7);
        out(11, S);
        step(4); step(10);
      }
      e--; step(6);
      step(4); step(4);
      if (e != 0) { step(10); continue; }
      step(10);
      e = Re; step(10);
      step(4);
      S = u8(S + 16); step(7);
      out(11, S);
      step(4); step(10);
    }
  }

  void wait_note() {
    for (;;) {
      step(4, kOn);
      steps({4, 11, 4, 4, 7, 12});
    }
  }

  void tick() override {
    step(12); step(10);
    if (target == kDrumEnd) {
      step(10);
      target = kNormal; step(16);
      step(14, kR2); steps({10, 10, 10, 10});
      end_of_tick();
      throw DrumOver{};
    }
    steps({11, 4, 11, 4, 4, 7, 10});
    const u8 z = don; step(13);
    step(4);
    if (z != 0) {
      step(7); step(13);
      dw--; step(4);
      step(13);
      if (dw == 0) {
        step(7); step(10); step(14, kR2); step(15, kR2);
        step(4, kOn);
        try {
          step(14);
          drum();
        } catch (const DrumOver&) {
        }
        return;
      }
      step(12);
    } else step(12);
    end_of_tick();
  }

  // END: either back to where the tick came, or the running BUZZ or WAIT ends.
  void end_of_tick() {
    L--; step(4);
    if (L != 0) {
      step(12); step(10);
      step(4, kOn);
      step(14);
      return;
    }
    step(7); step(10); step(10);
    step(4, kOn);
    step(14);
    throw NoteOver{};
  }

  void drum() {
    steps({11, 11, 11});
    L--; step(4);
    if (L != 0) step(12);
    else { step(7); L++; step(4); }
    step(11); step(10);
    target = kDrumEnd; step(16);
    step(4, kOn);
    u16 g = DP; step(16);
    for (;;) {
      const u8 x = rd(g); step(7);
      g++; step(6);
      step(4);
      if (x < 128) {
        step(10); step(4);
        u8 z = x & 31; step(7);
        z--; step(4);
        dw = z; step(13);
        steps({4, 4, 4, 4});
        const int kind = (x >> 5) & 3; step(7);
        step(4);
        DP = g; step(16);
        steps({7, 10, 11, 4, 7, 11, 7, 6, 7, 4, 10, 4});
        drum_sound(kind);
      }
      steps({10, 7, 4, 7, 17, 10, 11, 4, 7, 11, 7, 6, 7, 4, 10, 4});
      step(4);
      don = 0; step(13);
      steps({4, 10, 12});
    }
  }

  [[noreturn]] void drum_sound(int kind) {
    if (kind == 1) {
      u16 g = 0x0F18; step(10);
      for (;;) {
        u8 b = rd(g); step(7);
        hold(b);
        step(7);
        out(11, 16);
        g++; step(6);
        b = rd(g); step(7);
        hold(b);
        step(4);
        out(11, 0);
        g++; step(6);
        step(12);
      }
    }
    u8 c = kind == 2 ? 63 : 0; step(7);
    for (;;) {
      u8 b = c; step(4);
      hold(b);
      step(7);
      out(11, 16);
      if (kind == 0) { c++; step(4); c++; step(4); }
      else if (kind == 2) { step(4); c = u8(c >> 1 | c << 7); step(4); step(4); }
      else { step(4); c = u8(c + 4); step(7); step(4); }
      b = c; step(4);
      hold(b);
      step(4);
      out(11, 0);
      step(12);
    }
  }
};

// ---------------------------------------------------------------------------
// The engine.

struct EngineKind {
  const char* name;
  const char* display;
};
const EngineKind kKinds[] = {
    {"SFX", "Special FX"}, {"TMB", "The Music Box"}, {"MSD", "The Music Studio"},
    {"P1D", "Phaser1, digital drums"}, {"P1S", "Phaser1, synth drums"},
    {"SVG", "Savage"}, {"RMB", "ROM Beep"}, {"PLP", "Plip Plop"}};

class Beepola : public Engine {
 public:
  ~Beepola() override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

  void open(const std::string&, const std::vector<uint8_t>& data, int, int rate, Info& info) override {
    read_song(data, song_);
    kind_ = -1;
    for (int i = 0; i < 8; ++i)
      if (song_.engine == kKinds[i].name) kind_ = i;
    if (kind_ < 0) throw Refused("the song is for an engine that isn't known: " + song_.engine);
    const std::string name = kKinds[kind_].name;
    if (name != "SFX" && name != "PLP" && song_.length > 127)
      throw Refused("the song has more positions than the engine can count");
    if (name == "SFX") sfx_check(song_);
    else if (name == "SVG") savage_check(song_);
    else if (name == "RMB") beep_check(song_);

    mem_.reset(new Memory);
    Memory& m = *mem_;
    if (name == "SFX") sfx_layout(song_, m);
    else if (name == "TMB") box_layout(song_, m, false);
    else if (name == "MSD") box_layout(song_, m, true);
    else if (name == "P1D" || name == "P1S") phaser_layout(song_, m, name == "P1D");
    else if (name == "SVG") savage_layout(song_, m);
    else if (name == "RMB") beep_layout(song_, m);
    else plip_layout(song_, m);
    if (m.wp > 0xFE00) throw Refused(kNoFit);

    const char* dump = std::getenv("BBMP_BEEPOLA_DUMP");
    if (dump && *dump) write_layout(std::string(dump) + ".layout", m);

    // The data that is not shipped, where it is needed.
    std::vector<u8> bytes;
    have_rom_ = load_data(kRomFile, bytes);
    if (have_rom_) std::memcpy(m.m, bytes.data(), 16384);
    if (name == "P1D") {
      have_samples_ = load_data(kSampleFile, bytes);
      if (have_samples_) std::memcpy(m.m + 0x82CC, bytes.data(), 1024);
    }
    if (name == "SVG") {
      have_tail_ = load_data(kTailFile, bytes);
      if (have_tail_) std::memcpy(m.m + 0x8128, bytes.data(), 136);
    }

    // Pass 1: how long the song is, and whether it plays at all.
    Needs needs;
    Log log;
    std::unique_ptr<Machine> first = make_machine(needs);
    if (dump && *dump) first->log = &log;
    bool unplayable = false;
    try {
      first->play();
    } catch (const Unplayable&) {
      unplayable = true;
    }
    std::string missing;
    auto lack = [&](bool need, bool have, const char* file) {
      if (need && !have) missing += std::string(missing.empty() ? "" : ", ") + file;
    };
    lack(needs.rom, have_rom_, kRomFile.name);
    lack(needs.samples, have_samples_, kSampleFile.name);
    lack(needs.tail, have_tail_, kTailFile.name);
    if (!missing.empty()) throw Refused("This song needs Beepola's data (" + missing + "), which is not here");
    if (unplayable) throw Refused(kCantPlay);
    end_ = first->end_time;
    if (dump && *dump) write_log(dump, log, *first);
    if (end_ < 350000 || first->changes <= 100) throw Refused("the engine plays nothing");

    info.tracks = 1;
    info.title = song_.title;
    info.author = song_.author;
    info.type = std::string("Beepola (") + kKinds[kind_].display + ")";
    info.duration_ms = long(end_ * 1000 / 3500000);
    for (int p : song_.used())
      if (!song_.pat[p].name.empty()) info.names.push_back(song_.pat[p].name);

    rate_ = rate;
    worker_ = std::thread([this] { second_pass(); });
  }

  int render(int16_t* out, int frames) override {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [&] { return queue_.size() >= size_t(frames) * 2 || done_; });
    const size_t n = std::min(queue_.size() / 2, size_t(frames));
    std::copy(queue_.begin(), queue_.begin() + n * 2, out);
    queue_.erase(queue_.begin(), queue_.begin() + n * 2);
    lock.unlock();
    cv_.notify_all();
    return int(n);
  }

 private:
  class Output : public Sound {
   public:
    Output(Beepola& owner, int rate) : Sound(rate), owner_(owner) {}
    void flush() {
      if (buf_.empty()) return;
      std::unique_lock<std::mutex> lock(owner_.mu_);
      owner_.cv_.wait(lock, [&] { return owner_.queue_.size() < (1u << 17) || owner_.stop_; });
      if (owner_.stop_) throw Stopped{};
      owner_.queue_.insert(owner_.queue_.end(), buf_.begin(), buf_.end());
      buf_.clear();
      lock.unlock();
      owner_.cv_.notify_all();
    }

   protected:
    void frame(int16_t s) override {
      buf_.push_back(s);
      buf_.push_back(s);
      if (buf_.size() >= 4096) flush();
    }

   private:
    Beepola& owner_;
    std::vector<int16_t> buf_;
  };

  std::unique_ptr<Machine> make_machine(Needs& needs) {
    const Memory& m = *mem_;
    const int len = song_.length;
    switch (kind_) {
      case 0: return std::unique_ptr<Machine>(new SpecialFx(m, len, needs));
      case 1: return std::unique_ptr<Machine>(new MusicBox(m, len, needs, u16(m.wp - 2 * len), song_.loop));
      case 2: return std::unique_ptr<Machine>(new MusicStudio(m, len, needs, u16(m.wp - 2 * len), song_.loop));
      case 3: return std::unique_ptr<Machine>(new Phaser(m, len, needs, true));
      case 4: return std::unique_ptr<Machine>(new Phaser(m, len, needs, false));
      case 5: return std::unique_ptr<Machine>(new Savage(m, len, needs));
      case 6: return std::unique_ptr<Machine>(new RomBeep(m, len, needs));
      default: return std::unique_ptr<Machine>(new PlipPlop(m, len, needs));
    }
  }

  void second_pass() {
    try {
      Needs needs;
      std::unique_ptr<Machine> machine = make_machine(needs);
      Output output(*this, rate_);
      machine->sound = &output;
      try {
        machine->play();
      } catch (const Unplayable&) {
      }
      output.flush();
    } catch (const Stopped&) {
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      done_ = true;
    }
    cv_.notify_all();
  }

  static void write_layout(const std::string& path, const Memory& m) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    for (int a = 0; a < 65536; ++a)
      if (m.written[a]) std::fprintf(f, "%04X %02X\n", a, m.m[a]);
    std::fclose(f);
  }

  static void write_log(const std::string& prefix, const Log& log, const Machine& mc) {
    auto deltas = [](const std::string& path, const std::vector<i64>& v) {
      FILE* f = std::fopen(path.c_str(), "w");
      if (!f) return;
      i64 prev = 0;
      for (i64 x : v) { std::fprintf(f, "%lld\n", (long long)(x - prev)); prev = x; }
      std::fclose(f);
    };
    deltas(prefix + ".edges", log.edges);
    deltas(prefix + ".ticks", log.ticks);
    if (!log.inside.empty()) {
      if (FILE* f = std::fopen((prefix + ".inside").c_str(), "w")) {
        for (size_t i : log.inside) std::fprintf(f, "%zu\n", i);
        std::fclose(f);
      }
    }
    FILE* f = std::fopen((prefix + ".marks").c_str(), "w");
    if (!f) return;
    for (i64 x : log.marks) std::fprintf(f, "mark %lld\n", (long long)x);
    std::fprintf(f, "end %lld %s\n", (long long)mc.end_time, mc.end_by_marks ? "marks" : "limit");
    std::fclose(f);
  }

  Song song_;
  int kind_ = -1;
  std::unique_ptr<Memory> mem_;
  bool have_rom_ = false, have_samples_ = false, have_tail_ = false;
  i64 end_ = 0;
  int rate_ = 48000;

  std::thread worker_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<int16_t> queue_;
  bool done_ = false, stop_ = false;
};

}  // namespace

std::unique_ptr<Engine> make_beepola() { return std::unique_ptr<Engine>(new Beepola); }

}  // namespace ay
