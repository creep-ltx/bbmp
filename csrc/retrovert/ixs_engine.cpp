// SPDX-License-Identifier: MIT
// ixs_engine: a player for iXalance songs (.ixs), written for bbmp.
//
// A song is a small tracker module whose samples are not stored but made when it is
// loaded, by two sound generators, from a few hundred bytes of settings:
//
//   unpack (zlib)  ->  generator one (six operators, filter, reverb)
//                  ->  generator two (wave table, partials, detuned voices)
//                  ->  the tracker part (orders, instruments, samples, patterns)
//                  ->  play: ticks, a few commands, a fixed-point mixer at 44100 Hz
//
// Written from a plain description of what the format's player does (a clean-room
// rewrite, README.txt). Exactness matters: the output is compared with reference
// recordings sample for sample, so every step keeps the precision (single or double)
// and the order the description gives. Build with -ffp-contract=off.
//
// Three sample names stand for sounds that came with the original player and are not
// part of this program; they are read from a folder given at run time (Options).

#include "ixs_engine.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace ixs {
namespace {

// ---------------------------------------------------------------- numbers

struct Refusal {
  std::string why;
};

[[noreturn]] void refuse(const std::string& why) { throw Refusal{why}; }

// Float to integer the way x86 converts: out of range or NaN gives the lowest value.
int32_t to_i32(double x) {
  if (!(x > -2147483649.0 && x < 2147483648.0)) return INT32_MIN;
  return static_cast<int32_t>(x);
}

int64_t to_i64(double x) {
  if (!(x >= -9223372036854775808.0 && x < 9223372036854775808.0)) return INT64_MIN;
  return static_cast<int64_t>(x);
}

uint32_t to_u32(double x) { return static_cast<uint32_t>(static_cast<uint64_t>(to_i64(x))); }

int32_t wrap16(int32_t v) { return static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(v))); }

int32_t sat(int32_t v) { return v > 32767 ? 32767 : (v < -32767 ? -32767 : v); }

int32_t mul(int32_t a, int32_t b) {
  return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}
int32_t add(int32_t a, int32_t b) {
  return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}
int32_t sub(int32_t a, int32_t b) {
  return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

const double kTwoPi = 6.283185307179586;
const double kInvPi = 0.3183098861837907;
const double kPi = 3.141592653589793;
const double kFreqScale = 0.00014247585714285716;
const double kGrowthScale = 0.0000144404;
const double kTwoPiSingle = static_cast<double>(static_cast<float>(6.283185307179586));
const double kTwoPiVibrato = 6.283185308;
const double kHalfPiShape = 1.5707964;
const double kTwelfthSingle = static_cast<double>(1.0f / 12.0f);
const float kStep256 = 0.00390625f;

// Limits of the new engine (the original reads or writes outside its memory past them).
const uint64_t kStoreLimit = 31457260;      // bytes, 260 per entry plus its data
const uint64_t kMaxValues = 16777216;       // values in one array of generator one
const size_t kMaxUnpacked = 64u << 20;      // the unpacked data of a song
const uint64_t kWorkLimit = 4000000000ull;  // generator steps for a whole song (a few seconds)

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Reads the unpacked data; anything past its end refuses the song.
struct Reader {
  const std::vector<uint8_t>& data;
  uint64_t at;
  const char* part;
  void need(uint64_t n) const {
    if (at > data.size() || data.size() - at < n) refuse(std::string(part) + " reaches past the song's data");
  }
  uint8_t u8() { need(1); return data[at++]; }
  uint16_t u16() { need(2); const uint16_t v = rd16(&data[at]); at += 2; return v; }
  uint32_t u32() { need(4); const uint32_t v = rd32(&data[at]); at += 4; return v; }
  const uint8_t* take(uint64_t n) { need(n); const uint8_t* p = data.data() + at; at += n; return p; }
};

// ---------------------------------------------------------------- SHA-256 (for the library files)

std::string sha256(const std::vector<uint8_t>& msg) {
  static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::vector<uint8_t> m(msg);
  const uint64_t bits = static_cast<uint64_t>(msg.size()) * 8;
  m.push_back(0x80);
  while (m.size() % 64 != 56) m.push_back(0);
  for (int i = 7; i >= 0; --i) m.push_back(static_cast<uint8_t>(bits >> (8 * i)));
  auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
  for (size_t off = 0; off < m.size(); off += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (static_cast<uint32_t>(m[off + 4 * i]) << 24) | (m[off + 4 * i + 1] << 16) |
             (m[off + 4 * i + 2] << 8) | m[off + 4 * i + 3];
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
  std::string out;
  char buf[9];
  for (uint32_t v : h) { std::snprintf(buf, sizeof(buf), "%08x", v); out += buf; }
  return out;
}

// ---------------------------------------------------------------- the sample store

struct Entry {
  std::string name;
  std::vector<uint8_t> data;
};

std::string fold(const std::string& name) {
  std::string key = name;
  for (char& c : key)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  return key;
}

class Store {
 public:
  void put(const std::string& name, const std::vector<uint8_t>& data) {
    const auto found = index_.find(fold(name));
    if (found == index_.end()) {
      used_ += 260 + data.size();
      if (used_ > kStoreLimit) refuse("the song's samples need more room than the original player has");
      index_.emplace(fold(name), list_.size());
      list_.push_back({name, data});
      return;
    }
    Entry& e = list_[found->second];
    if (data.size() > e.data.size()) refuse("a sample is made twice, the second time longer");
    std::copy(data.begin(), data.end(), e.data.begin());
  }
  const Entry* find(const std::string& name) const {
    const auto found = index_.find(fold(name));
    return found == index_.end() ? nullptr : &list_[found->second];
  }
  const std::vector<Entry>& list() const { return list_; }

 private:
  std::vector<Entry> list_;
  std::unordered_map<std::string, size_t> index_;
  uint64_t used_ = 0;
};

std::vector<uint8_t> pcm16(const std::vector<int16_t>& v) {
  std::vector<uint8_t> out(v.size() * 2);
  for (size_t i = 0; i < v.size(); ++i) {
    out[2 * i] = static_cast<uint8_t>(v[i] & 0xFF);
    out[2 * i + 1] = static_cast<uint8_t>((static_cast<uint16_t>(v[i]) >> 8) & 0xFF);
  }
  return out;
}

struct Work {
  uint64_t steps = 0;
  void add(uint64_t n) {
    steps += n;
    if (steps > kWorkLimit) refuse("the song's samples would take too long to make");
  }
};

// ---------------------------------------------------------------- generator one

// A resonant two-pole filter: speed and position.
struct Resonator {
  double speed = 0, place = 0;
  double run(double x, double cutoff, double q, bool add_input) {
    const double c = cutoff < 1.0 ? 1.0 : cutoff;
    speed = (speed - (x + place) / c) * q;
    place = speed + place;
    return add_input ? place + x : place;
  }
};

double op1_wave(double x, float width, float choice) {
  const double sel = choice;
  if (sel >= -0.3 && sel <= 0.3) return std::sin(x);
  double m = std::fmod(x, kTwoPi);
  if (m < 0) m += kTwoPi;
  if (sel > 0.3) {
    double w = width;
    if (w < -0.99) w = -0.99;
    return m <= (w + 1.0) * 0.5 * kPi ? -1.0 : 1.0;
  }
  const double t = m * kInvPi;
  return t >= 1.0 ? t - 2.0 : t;
}

class FmGenerator {
 public:
  FmGenerator(const std::vector<uint8_t>& u, Store& store, Work& work) : u_(u), store_(store), work_(work) {}

  void run(uint32_t at_offset) {
    Reader r{u_, at_offset, "generator one's data"};
    const uint32_t count = r.u32();
    uint8_t running[268] = {};
    for (uint32_t index = 0; index < count; ++index) {
      const uint8_t* raw = r.take(268);
      uint8_t rec[268];
      std::memcpy(rec, raw, 9);
      for (int i = 9; i < 268; ++i) {
        running[i] = static_cast<uint8_t>(running[i] + raw[i]);
        rec[i] = running[i];
      }
      int name_len = -1;
      for (int i = 0; i < 9; ++i)
        if (rec[i] == 0) { name_len = i; break; }
      if (name_len < 0) refuse("a sample record's name has no end");
      if (rec[266] == 0) {
        if (index + 1 < count) refuse("a sample record that makes nothing is not the last");
        continue;
      }
      record(std::string(reinterpret_cast<const char*>(rec), name_len), rec);
    }
  }

 private:
  void fade(int64_t n) {
    const int64_t len = static_cast<int32_t>(len_);
    if (n > len) n = len;
    if (n == 0) return;
    const float q = static_cast<float>(1.0 / static_cast<double>(n));
    for (int64_t i = len - n; i < len; ++i) {
      a_[i] = static_cast<float>(static_cast<double>(len - i) * q * a_[i]);
      b_[i] = static_cast<float>(static_cast<double>(len - i) * q * b_[i]);
    }
  }

  void record(const std::string& name, const uint8_t* rec) {
    float p[100];
    for (int k = 0; k < 100; ++k) p[k] = static_cast<float>((rec[9 + k] - 127.0) * (1.0 / 127.0));
    int32_t note[16];
    float amp[16];
    for (int s = 0; s < 16; ++s) {
      note[s] = static_cast<int32_t>(rd32(rec + 109 + 4 * s));
      const uint32_t bits = rd32(rec + 189 + 4 * s);
      std::memcpy(&amp[s], &bits, 4);
    }
    const uint8_t* legato = rec + 173;
    const int32_t seg_len = static_cast<int32_t>(rd32(rec + 253));
    const uint32_t segs = rd32(rec + 261);
    const bool stereo = rec[267] != 0;
    if (segs > 16) refuse("a sample record has more than 16 segments");
    len_ = static_cast<uint32_t>(seg_len) * segs;
    if (len_ > kMaxValues || (seg_len > 0 && static_cast<uint64_t>(seg_len) * segs != len_))
      refuse("a generated sample is too long");
    work_.add(static_cast<uint64_t>(len_) * 32);   // an operator sample costs about 32 other steps
    a_.assign(len_, 0.0f);
    b_.assign(len_, 0.0f);

    double phase[6] = {};
    filt1_ = Resonator();
    filt2_ = Resonator();
    const double e = std::pow(23.4, static_cast<double>(p[88])) *
                     (static_cast<double>(static_cast<int32_t>(segs)) * kGrowthScale);
    double grow[32], rate[32];
    for (int j = 0; j < 32; ++j) {
      grow[j] = p[50 + j] * e + 1.0;
      rate[j] = grow[j];
    }
    const bool filter_adds = p[36] < 0, reverb_adds = p[85] < 0;

    for (uint32_t s = 0; s < segs; ++s) {
      const double glide = static_cast<double>((p[32] + 1.0f) * kStep256);
      double z1 = 0, z2 = 0, z3 = 0;
      const bool tied = legato[s] != 0;
      if (!tied)
        for (int k = 0; k < 6; ++k) start_level_[k] = static_cast<double>((p[3 * k] - 1.0f) * 0.5f);
      double ratio0[6], decay[6];
      for (int k = 0; k < 6; ++k) {
        ratio0[k] = static_cast<double>((p[3 * k + 1] - 1.0f) * 0.5f);
        decay[k] = std::pow(static_cast<double>((p[3 * k + 2] + 1.0f) * 0.5f), 1.0 / 512);
      }
      double feed[3];
      for (int k = 0; k < 3; ++k) feed[k] = static_cast<double>((p[18 + k] - 1.0f) * 0.5f);
      const double cut_decay = std::pow(static_cast<double>((p[26] + 1.0f) * 0.5f), 1.0 / 512);
      if (!tied) {
        const float f = (p[27] + 1.0f) * 0.5f;
        start_cut_ = static_cast<double>(f * f * 512.0f + 1.0f);
      }
      const double reso = static_cast<double>((p[28] + 1.0f) * 0.5f);
      const double drive = static_cast<double>((p[25] + 1.0f) * 0.5f);
      double scale = static_cast<double>(p[21] + 1.0f) * 256.0;
      if (p[22] != 0) scale = std::pow(2.0, static_cast<double>(-(note[s] / 15))) * scale;
      const double target = std::pow(2.0, static_cast<double>(note[s]) * kTwelfthSingle) * 110.0;
      double freq = end_freq_;
      double lt[6];
      for (int k = 0; k < 6; ++k) lt[k] = start_level_[k];
      double ct = start_cut_;
      double level[6] = {}, ratio[6], cut = 0;
      const float drive_s = static_cast<float>(drive * 8.0 + 1.0);

      for (int32_t i = 0; i < seg_len; ++i) {
        for (int j = 0; j < 32; ++j) rate[j] = grow[j] * rate[j];
        for (int k = 0; k < 6; ++k) {
          level[k] = rate[3 * k] * lt[k];
          ratio[k] = rate[3 * k + 1] * ratio0[k];
        }
        cut = rate[27] * ct;
        feed[0] = rate[18] * feed[0];
        feed[1] = rate[19] * feed[1];
        feed[2] = rate[20] * feed[2];
        if (!tied) {
          freq = target;
        } else {
          if (end_freq_ < target && freq < target) {
            freq = (glide + 1.0) * freq;
            if (target <= freq) freq = target;
          }
          if (!(end_freq_ <= target) && !(freq <= target)) {
            freq = (1.0 - glide) * freq;
            if (freq < target || freq == target) freq = target;
          }
          if (end_freq_ < target && !(freq <= target)) freq = target;
        }
        const double inc6 = freq * ratio[5] * kFreqScale;
        phase[5] = std::fmod(inc6 + phase[5], kTwoPi);
        const double y6 = std::sin(inc6 + z3 + phase[5]) * scale * level[5];
        const double inc5 = freq * ratio[4] * kFreqScale;
        phase[4] = std::fmod(inc5 + phase[4], kTwoPi);
        const double y5 = std::sin(inc5 * y6 + phase[4]) * scale * level[4];
        const double inc4 = ratio[3] * freq * kFreqScale;
        phase[3] = std::fmod(inc4 + phase[3], kTwoPi);
        const double y4 = std::sin((z2 + y5) * inc4 + phase[3]) * level[3] * scale;
        const double inc3 = freq * ratio[2] * kFreqScale;
        phase[2] = std::fmod(inc3 + phase[2], kTwoPi);
        const double y3 = std::sin(inc3 * y4 + phase[2]) * level[2];
        const double inc2 = freq * ratio[1] * kFreqScale;
        phase[1] = std::fmod(inc2 + phase[1], kTwoPi);
        const double y2 = std::sin(z1 * inc2 + phase[1]) * scale * level[1];
        const double inc1 = freq * ratio[0] * kFreqScale;
        phase[0] = std::fmod(inc1 + phase[0], kTwoPi);
        const double x1 = inc1 * y2 + phase[0];
        const double y1 = level[0] * op1_wave(x1, p[90], p[91]);
        z1 = y1 * feed[0] * scale;
        z2 = y3 * scale * feed[1];
        z3 = y3 * scale * feed[2];
        const double x = static_cast<double>(amp[s]) * (y1 + y3);
        float v = static_cast<float>(filt1_.run(x, cut, reso, filter_adds) * 0.5);
        v = drive_s * v;
        if (v > 1) v = 1;
        else if (v < -1) v = -1;
        a_[static_cast<size_t>(seg_len) * s + i] = v;
        for (int k = 0; k < 6; ++k) lt[k] = lt[k] * decay[k];
        ct = ((1.0 - cut_decay) + 1.0) * ct;
      }
      if (seg_len > 0) {
        for (int k = 0; k < 6; ++k) start_level_[k] = level[k];
        start_cut_ = cut;
      }
      end_freq_ = p[86] >= 0 ? target : freq;
    }

    // after the segments
    if (p[30] > 0) {
      fade(1000);
      std::reverse(a_.begin(), a_.end());
    }
    fade(1000);
    const double dry = (p[92] - 1.0) * -0.5;
    const double wet = (p[93] - 1.0) * -0.5;
    uint32_t mult = to_u32((p[94] - 1.0) * 0.5 * -16.0);
    if (p[87] < 0) mult = mult * to_u32(p[87] * -8.0);
    const double fb = (p[95] - 1.0) * -0.5;
    const double gin = (p[96] - 1.0) * -0.5;
    const double rc = -((p[97] - 1.0) * 0.5 * 512.0 + 1.0);
    const double rq = (p[98] - 1.0) * -0.5;
    const int32_t taps = to_i32(-((p[99] - 1.0) * 0.5 * 100.0));
    const bool folded = p[31] > 0;
    const double xfade = p[33] >= 0 ? -1.0 : static_cast<double>(0.5f - p[33] * 0.5f);

    if (p[84] < 0) {
      const int32_t n = to_i32(static_cast<double>(-p[84]) * static_cast<double>(static_cast<int32_t>(len_)));
      for (int32_t j = 0; j < n && static_cast<uint32_t>(j) < len_; ++j) {
        double t = static_cast<double>(j) / static_cast<double>(static_cast<float>(n));
        t = t * t;
        a_[j] = a_[j] * static_cast<float>(t);
      }
    }
    if (stereo) {
      reverb(mult, dry, wet, fb, gin, rc, rq, taps, folded, xfade, reverb_adds, p[89]);
      if (p[31] <= 0 && xfade < 0) fade(1000);
      if (p[23] < 0) fade(static_cast<int32_t>(len_) / 4);
      if (p[23] > 0) fade(len_);
    } else {
      if (p[23] < 0) fade(static_cast<int32_t>(len_) / 4);
      if (p[23] > 0) fade(len_);
      b_ = a_;
    }
    if (p[34] < 0) {
      const float k = p[34] * -100.0f;
      auto clamp = [](float v) { return !(v <= 1) ? 1.0f : (v < -1 ? -1.0f : v); };
      for (uint32_t i = 0; i < len_; ++i) {
        a_[i] = clamp(k * a_[i]);
        b_[i] = clamp(k * b_[i]);
      }
    }
    if (p[83] < 0) {
      const uint32_t old = len_;
      const int32_t m = to_i32(static_cast<double>(-p[83]) * static_cast<double>(len_));
      const int64_t total = static_cast<int64_t>(old) + m;
      if (total < 0 || static_cast<uint64_t>(total) > kMaxValues) refuse("a generated sample is too long");
      if (total > 0 && static_cast<int64_t>(old) - m - 1 < 0)
        refuse("a generated sample copies from before its start");
      std::vector<float> na(total), nb(total);
      for (int64_t k = 0; k < total; ++k) {
        if (k <= m) {
          double t = static_cast<double>(k) / static_cast<double>(static_cast<float>(m));
          t = t * t;
          const int64_t from = k - 1 + old - m;
          na[k] = static_cast<float>(t * a_[from]);
          nb[k] = static_cast<float>(t * b_[from]);
        } else {
          na[k] = a_[k - m];
          nb[k] = b_[k - m];
        }
      }
      a_.swap(na);
      b_.swap(nb);
      len_ = static_cast<uint32_t>(total);
    }

    const bool half = static_cast<double>(p[35]) < -0.5;
    const uint32_t n = half ? len_ >> 1 : len_;
    std::vector<int16_t> left(n), right(n);
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t j = half ? 2 * i : i;
      left[i] = static_cast<int16_t>(wrap16(to_i32(static_cast<double>(a_[j]) * 32767.5 - 0.5)));
      right[i] = static_cast<int16_t>(wrap16(to_i32(static_cast<double>(b_[j]) * 32767.5 - 0.5)));
    }
    if (stereo) {
      store_.put(name + "l.wav", pcm16(left));
      store_.put(name + "r.wav", pcm16(right));
    } else {
      store_.put(name + ".wav", pcm16(left));
    }
  }

  void reverb(uint32_t mult, double dry, double wet, double fb, double gin, double rc, double rq,
              int32_t taps, bool folded, double xfade, bool adds, float seed_p) {
    const uint32_t old = len_;
    const uint64_t longer = static_cast<uint64_t>(old) * mult;
    if (longer > kMaxValues) refuse("a generated sample is too long");
    len_ = static_cast<uint32_t>(longer);
    std::vector<float> orig;
    orig.swap(a_);
    a_.assign(len_, 0.0f);
    b_.assign(len_, 0.0f);
    std::vector<float> delay(98305, 0.0f);

    uint32_t seed = 0xDEADu - static_cast<uint32_t>(to_i32(seed_p * -100.0f));
    auto next = [&seed]() {
      seed = seed * 0x343FDu + 0x269EC3u;
      return static_cast<int>((seed >> 16) & 0x7FFF);
    };
    const int n = taps > 0 ? taps : 0;
    std::vector<int32_t> wpos(2 * n), rpos(2 * n), span(2 * n);
    std::vector<double> sign(2 * n);
    const float h = 1.0f / 32767.0f, big = 98304.0f;
    for (int set = 0; set < 2; ++set) {
      for (int t = 0; t < n; ++t) {
        const int k = set + 2 * t;
        wpos[k] = to_i32(h * big * static_cast<float>(next()));
        rpos[k] = to_i32(h * big * static_cast<float>(next()));
        const int sp = 98304 / (2 * n);
        const float f3 = static_cast<float>(98304 - t * sp);
        span[k] = to_i32(f3 - h * static_cast<float>(next()) * static_cast<float>(sp >> 1));
        sign[k] = static_cast<double>(h * static_cast<float>(next())) > 0.5 ? -1.0 : 1.0;
      }
    }
    work_.add(static_cast<uint64_t>(len_) * (1 + 4 * static_cast<uint64_t>(n)));
    auto slot = [](int32_t i) { return static_cast<uint32_t>(i) <= 98304u ? static_cast<uint32_t>(i) : 0u; };
    const double dv = 1.0 / static_cast<double>(taps);
    for (uint32_t l = 0; l < len_; ++l) {
      const double x = l < old ? static_cast<double>(orig[l]) : 0.0;
      for (int set = 0; set < 2; ++set) {
        for (int t = 0; t < n; ++t) {
          const int k = set + 2 * t;
          if (++wpos[k] > span[k]) wpos[k] = 0;
          if (++rpos[k] > span[k]) rpos[k] = 0;
          float& cell = delay[slot(wpos[k])];
          cell = static_cast<float>((sign[k] * gin) * x + static_cast<double>(cell) * fb);
        }
      }
      double s0 = 0.0, s1 = 0.0;
      for (int t = 0; t < n; ++t) s0 = s0 + static_cast<double>(delay[slot(rpos[2 * t])]) * wet;
      for (int t = 0; t < n; ++t) s1 = s1 + static_cast<double>(delay[slot(rpos[1 + 2 * t])]) * wet;
      a_[l] = static_cast<float>(dv * s0);
      b_[l] = static_cast<float>(dv * s1);
    }
    if (folded) {
      std::vector<float> na(old, 0.0f), nb(old, 0.0f);
      for (uint32_t i = 0; i < old; ++i) {
        for (uint32_t j = 0; j < mult; ++j) {
          na[i] = a_[i + static_cast<size_t>(j) * old] + na[i];
          nb[i] = b_[i + static_cast<size_t>(j) * old] + nb[i];
        }
      }
      a_.swap(na);
      b_.swap(nb);
      len_ = old;
    }
    filt1_.place = 0;
    filt2_ = Resonator();
    for (uint32_t i = 0; i < len_; ++i) {
      a_[i] = static_cast<float>(filt1_.run(a_[i], rc, rq, adds));
      b_[i] = static_cast<float>(filt2_.run(b_[i], rc, rq, adds));
    }
    for (uint32_t i = 0; i < len_; ++i) {
      const double d = (i < old ? static_cast<double>(orig[i]) : 0.0) * dry;
      a_[i] = a_[i] + static_cast<float>(d);
      b_[i] = b_[i] + static_cast<float>(d);
    }
    if (xfade > 0) {
      const int32_t l = to_i32(xfade * static_cast<double>(len_));
      const int64_t n2 = static_cast<int64_t>(len_) - l;
      for (int64_t m = 0; m < n2; ++m) {
        const double av = static_cast<double>(m) / static_cast<double>(static_cast<float>(n2));
        const double bv = 1.0 - av;
        a_[m] = static_cast<float>(bv * a_[m + l] + av * a_[m]);
        b_[m] = static_cast<float>(bv * b_[m + l] + av * b_[m]);
      }
      len_ = static_cast<uint32_t>(l);
      a_.resize(len_);
      b_.resize(len_);
    }
    filt1_ = Resonator();
    filt2_ = Resonator();
    double peak = 0.0;
    for (uint32_t i = 0; i < len_; ++i) {
      if (peak < std::fabs(static_cast<double>(a_[i]))) peak = std::fabs(static_cast<double>(a_[i]));
      if (peak < std::fabs(static_cast<double>(b_[i]))) peak = std::fabs(static_cast<double>(b_[i]));
    }
    const float sc = static_cast<float>(1.0 / peak);
    for (uint32_t i = 0; i < len_; ++i) {
      a_[i] = sc * a_[i];
      b_[i] = sc * b_[i];
    }
  }

  const std::vector<uint8_t>& u_;
  Store& store_;
  Work& work_;
  // carried from record to record
  double end_freq_ = 0, start_level_[6] = {}, start_cut_ = 0;
  // the record being made
  uint32_t len_ = 0;
  std::vector<float> a_, b_;
  Resonator filt1_, filt2_;
};

// ---------------------------------------------------------------- generator two

struct Frame {
  int32_t l = 0, r = 0;
};

void normalise(std::vector<Frame>& buf, size_t n, int flags) {
  if (flags == 0) return;
  int32_t lmin = 32767, lmax = -32767, rmin = 32767, rmax = -32767, rmin2 = 32767, rmax2 = -32767;
  for (size_t i = 0; i < n; ++i) {
    const int32_t l = buf[i].l, r = buf[i].r;
    if (l < lmin) lmin = l;
    if (l > lmax) lmax = l;
    if (r < rmin) rmin = rmin2 = r;
    if (r > rmax) rmax = rmax2 = r;
  }
  if (flags & 4) {
    if (lmin >= rmin) lmin = rmin;
    else rmin2 = lmin;
    if (lmax <= rmax) lmax = rmax;
    else rmax2 = lmax;
  }
  const int32_t cl = (lmax + lmin) / 2, cr = (rmin2 + rmax2) / 2;
  int32_t xl = 0, xr = 0;
  if (flags & 1) {
    lmin -= cl; lmax -= cl; rmin2 -= cr; rmax2 -= cr;
    xl = -cl;
    xr = -cr;
  }
  if (flags & 2) {
    const int32_t pl = std::max(std::abs(lmin), std::abs(lmax));
    const int32_t pr = std::max(std::abs(rmax2), std::abs(rmin2));
    const int32_t kl = pl ? 0x1FFFFFFF / pl : 0, kr = pr ? 0x1FFFFFFF / pr : 0;
    for (size_t i = 0; i < n; ++i) {
      buf[i].l = wrap16(mul(kl, add(xl, buf[i].l)) >> 14);
      buf[i].r = wrap16(mul(kr, add(xr, buf[i].r)) >> 14);
    }
  } else {
    for (size_t i = 0; i < n; ++i) {
      buf[i].l = wrap16(add(buf[i].l, xl));
      buf[i].r = wrap16(add(buf[i].r, xr));
    }
  }
}

class WaveGenerator {
 public:
  WaveGenerator(const std::vector<uint8_t>& u, Store& store, Work& work) : u_(u), store_(store), work_(work) {}

  void run(uint32_t at_offset) {
    Reader r{u_, at_offset, "generator two's data"};
    const int count = r.u8();
    for (int e = 0; e < count; ++e) {
      std::string name;
      for (;;) {
        const uint8_t c = r.u8();
        if (c == 0) break;
        name += static_cast<char>(c);
        if (name.size() >= 200) refuse("a wave sample's name is too long");
      }
      const uint32_t skip = number(r);
      if (skip != 0) continue;
      const uint32_t body = number(r);
      entry(name, r.at, body);
      r.at += body;
    }
  }

 private:
  static uint32_t number(Reader& r) {
    const uint8_t b0 = r.u8();
    if ((b0 & 0xC0) != 0xC0) return b0;
    const uint8_t b1 = r.u8();
    uint32_t v = (b0 & 0x3Fu) + ((b1 & 0x7Fu) << 6);
    if (b1 & 0x80) v |= static_cast<uint32_t>(r.u16()) << 13;
    return v;
  }

  static int32_t shape(float x, const float q[4]) {
    float m = static_cast<float>(std::fmod(static_cast<double>(x), 1.0));
    double a = 32767.0, b;
    if (static_cast<double>(m) >= static_cast<double>(q[1])) {
      a = -32767.0;
      b = static_cast<double>(1.0f - q[1]);
      m = 1.0f - m;
    } else {
      b = q[1];
    }
    const double c = static_cast<double>(m) >= b ? 1.0 : static_cast<double>(m) / b;
    double d;
    if (c >= q[3]) {
      const float e = 1.0f - q[3];
      const double u = 1.0 - c;
      d = u >= e ? 1.0 : u / e;
    } else {
      d = c / q[3];
    }
    const double qq = static_cast<double>(1.0f - q[2]);
    const double r = d >= qq ? 1.0 : d / qq;
    const double v = a * (std::sin(r * kHalfPiShape) * q[0] + static_cast<double>(1.0f - q[0]) * r);
    return static_cast<int32_t>(std::round(v));
  }

  void entry(const std::string& name, uint64_t start, uint32_t length) {
    Reader r{u_, start, "generator two's data"};
    r.need(length);
    bool have1 = false, have3 = false, have4 = false;
    int nf1 = 7, nf2 = 7;
    uint32_t tl = 0;
    std::vector<Frame> table;
    uint32_t rep = 0;
    int32_t np = 0;
    int64_t wl = 0;
    std::vector<float> rt, ph, lv, pn, vd, vr;
    uint32_t out = 0, xl = 0;
    int32_t nv = 0;
    std::vector<float> dt;
    uint64_t off = 0;
    if (length > 0) {
      do {
        r.at = start + off;
        const uint32_t type = number(r);
        const uint32_t len = number(r);
        const uint64_t data = r.at;
        if (type == 1) {
          const uint8_t* b = r.take(5);
          for (int k = 0; k < 4; ++k)
            q_[k] = static_cast<float>(0.005 * static_cast<double>(static_cast<float>(b[k])));
          tl = 4u * b[4];
          table.assign(tl + 1, Frame());
          for (uint32_t i = 0; i < tl; ++i) {
            const float x = static_cast<float>(static_cast<double>(i) / static_cast<double>(static_cast<float>(tl)));
            const int32_t v = wrap16(shape(x, q_));
            table[i] = {v, v};
          }
          have1 = true;
        } else if (type == 3) {
          rep = r.u16();
          np = static_cast<int32_t>(len - 2u) / 8;
          const int32_t cnt = np > 0 ? np : 0;
          const uint8_t* b = r.take(8 * static_cast<uint64_t>(cnt));
          rt.assign(cnt, 0); ph.assign(cnt, 0); lv.assign(cnt, 0); pn.assign(cnt, 0);
          vd.assign(cnt, 0); vr.assign(cnt, 0);
          for (int32_t j = 0; j < cnt; ++j) {
            rt[j] = static_cast<float>(static_cast<double>(rd16(b + 2 * j)) * 0.0005);
            ph[j] = static_cast<float>(static_cast<int32_t>(tl * b[2 * cnt + j] + 8u) >> 8);
            lv[j] = static_cast<float>(2 * static_cast<int8_t>(b[3 * cnt + j]));
            pn[j] = static_cast<float>(b[4 * cnt + j]);
            const uint8_t* v = b + 5 * cnt + 3 * j;
            const uint32_t w = v[0] | (v[1] << 8) | (v[2] << 16);
            int32_t depth = w & 0xFFF, speed = (w >> 12) & 0xFFF;
            if (depth > 3072) depth -= 4096;
            if (speed > 3072) speed -= 4096;
            vd[j] = static_cast<float>(depth);
            vr[j] = static_cast<float>(speed);
          }
          wl = static_cast<int64_t>(tl) * rep;
          have3 = true;
        } else if (type == 4) {
          const uint8_t* b = r.take(2);
          out = (static_cast<uint32_t>(b[0]) + b[1]) << 10;
          xl = static_cast<uint32_t>(b[0]) << 10;
          if (xl >= out) xl = (7 * out) >> 3;
          nv = static_cast<int32_t>(len - 2u);
          const int32_t cnt = nv > 0 ? nv : 0;
          const uint8_t* d = r.take(cnt);
          dt.assign(cnt, 0);
          for (int32_t j = 0; j < cnt; ++j) dt[j] = static_cast<float>(static_cast<int8_t>(d[j]));
          have4 = true;
        } else if (type == 5) {
          const uint8_t b = r.u8();
          nf1 = b & 7;
          nf2 = (b >> 4) & 7;
        }
        off = (data - start) + len;
      } while (off < length);
    }
    if (!have1 || !have3 || !have4) refuse("a wave sample lacks part of its settings");
    if (tl == 0 || np < 1 || np > 1000 || nv <= 0 || wl <= 0) refuse("a wave sample's settings are out of range");

    // the name: after the last '\', before the last '.'
    const size_t slash = name.rfind('\\');
    const size_t from = slash == std::string::npos ? 0 : slash + 1;
    size_t dot = name.rfind('.');
    if (dot != std::string::npos && slash != std::string::npos && dot < slash) refuse("a wave sample's name is empty");
    if (dot == std::string::npos) dot = name.size();
    const std::string part = name.substr(from, dot - from);
    if (part.empty() || part.size() > 58) refuse("a wave sample's name is empty or too long");

    work_.add(static_cast<uint64_t>(wl) * np + static_cast<uint64_t>(out) * nv);
    if (wl > static_cast<int64_t>(kMaxValues)) refuse("a wave sample's wave is too long");

    // stage one: partials into a wave
    std::vector<Frame> wave(wl + 1);
    table[tl] = table[0];
    double s1 = 0.0, s2 = 0.0;
    for (int32_t j = 0; j < np; ++j) {
      const double t1 = std::fabs(static_cast<double>(lv[j]));
      const double t2 = 100.0 - pn[j];
      s2 = s2 + t2 * t1;
      s1 = s1 + t1 * pn[j];
    }
    const float in = static_cast<float>(s1 >= s2 ? s1 : s2);
    const float norm = static_cast<double>(in) > 0.0001 ? 1.0f / in : 10000.0f;
    const uint32_t tlf = tl * 1024;
    for (int32_t j = 0; j < np; ++j) {
      uint32_t q = to_u32(ph[j] * 1024.0);
      const int32_t sb = to_i32(rt[j] * 1024.0);
      const int32_t gr = to_i32(static_cast<double>(pn[j]) * 1024.0 * lv[j] * norm);
      const int32_t gl = to_i32((100.0 - pn[j]) * 1024.0 * lv[j] * norm);
      const double w = vr[j] * kTwoPiSingle / static_cast<double>(wl);
      const double dep = vd[j] * w * 1024.0;
      int32_t step = 0;
      for (int64_t i = 0; i < wl; ++i) {
        if (i % 64 == 0) step = to_i32(std::sin(static_cast<double>(i) * w) * dep + static_cast<double>(sb));
        if (q >= tlf) q %= tlf;
        const int32_t fr = static_cast<int32_t>(q & 1023);
        const uint32_t k = q >> 10;
        const Frame& t0 = table[k];
        const Frame& t1 = table[k + 1];
        const int32_t il = add(t0.l, mul(fr, sub(t1.l, t0.l)) >> 10);
        const int32_t ir = add(t0.r, mul(fr, sub(t1.r, t0.r)) >> 10);
        wave[i].l = wrap16(add(wave[i].l, mul(gl, il) >> 10));
        wave[i].r = wrap16(add(wave[i].r, mul(gr, ir) >> 10));
        q = q + static_cast<uint32_t>(step);
      }
    }
    normalise(wave, wl, nf1);

    // stage two: detuned voices
    std::vector<Frame> ob(out);
    wave[wl] = wave[0];
    const int32_t vs = 65536 / nv;
    int64_t c = 0;
    auto at = [&](int64_t idx) -> const Frame& {
      static const Frame zero;
      return (idx >= 0 && idx <= wl) ? wave[idx] : zero;
    };
    for (int32_t j = 0; j < nv; ++j) {
      const double ratio = std::pow(2.0, dt[j] * (1.0 / 12.0));
      const int64_t a = to_i64(ratio * 4294967296.0);
      const uint64_t as = static_cast<uint64_t>(a) * out;
      int64_t cn = c / nv;
      const int64_t p0 = to_i64(static_cast<double>(cn) * ratio * 4294967296.0);
      uint32_t h = static_cast<uint32_t>(p0);
      int32_t g = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(p0) >> 32));
      g = static_cast<int32_t>(g % wl);
      for (uint32_t i = 0; i < out; ++i) {
        const int64_t o = (cn + 1 == static_cast<int64_t>(out)) ? 0 : cn + 1;
        const uint64_t m = static_cast<uint64_t>(a) +
                           ((static_cast<uint64_t>(static_cast<int64_t>(g)) << 32) + h);
        g = static_cast<int32_t>(static_cast<uint32_t>(m >> 32));
        h = static_cast<uint32_t>(m);
        const int64_t idx = g % wl;
        const int32_t fr16 = static_cast<int32_t>((m >> 16) & 0xFFFF);
        const Frame& w0 = at(idx);
        const Frame& w1 = at(idx + 1);
        int32_t c1 = add(w0.l, mul(fr16, sub(w1.l, w0.l)) >> 16);
        int32_t c2 = add(w0.r, mul(fr16, sub(w1.r, w0.r)) >> 16);
        if (i < xl) {
          const uint64_t mm = m + as;
          const int32_t ma = static_cast<int32_t>(static_cast<uint32_t>(mm) >> 16);
          const int64_t mb = static_cast<int32_t>(static_cast<uint32_t>(mm >> 32)) % wl;
          const uint32_t z = to_u32(static_cast<double>(i) * 16384.0 / static_cast<double>(xl));
          const Frame& m0 = at(mb);
          const Frame& m1 = at(mb + 1);
          const int32_t ol = add(m0.l, mul(ma, sub(m1.l, m0.l)) >> 16);
          const int32_t orr = add(m0.r, mul(ma, sub(m1.r, m0.r)) >> 16);
          c2 = static_cast<int32_t>(static_cast<uint32_t>(c2) * z + (16384u - z) * static_cast<uint32_t>(orr)) >> 14;
          c1 = static_cast<int32_t>(static_cast<uint32_t>(c1) * z + (16384u - z) * static_cast<uint32_t>(ol)) >> 14;
        }
        if (o >= 0 && o < static_cast<int64_t>(out)) {
          ob[o].l = wrap16(add(ob[o].l, mul(vs, c1) >> 16));
          ob[o].r = wrap16(add(ob[o].r, mul(vs, c2) >> 16));
        }
        cn = o;
      }
      c = c + out;
    }
    normalise(ob, out, nf2);

    std::vector<int16_t> left(out), right(out);
    for (uint32_t i = 0; i < out; ++i) {
      left[i] = static_cast<int16_t>(ob[i].l);
      right[i] = static_cast<int16_t>(ob[i].r);
    }
    store_.put(part + "L.wav", pcm16(left));
    store_.put(part + "R.wav", pcm16(right));
  }

  const std::vector<uint8_t>& u_;
  Store& store_;
  Work& work_;
  float q_[4] = {};
};

// ---------------------------------------------------------------- the library samples

struct LibraryFile {
  const char* name;  // as a song names it
  const char* alt;   // the same file under the name the reference folder uses
  size_t size;
  const char* sha256;
};

const LibraryFile kLibrary[3] = {
    {"909OH.WAV", "909oh.bin", 3424, "e93adc9872f7b255a4088b11d48d9f3db988e379e620bbf86993491f1057ba23"},
    {"909SD.WAV", "909sd.bin", 3413, "54cb1bca8f5a05ae6630ae76d5dec7222224509845f9afbcb899f57a3e7866b6"},
    {"CRASH.WAV", "crash.bin", 16098, "2c0d056aa8d8e95995505615afecaaae446855fb523d075e7edfaad76e6a5746"},
};

bool read_file(const std::string& path, std::vector<uint8_t>& out, size_t limit) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out.clear();
  uint8_t buf[4096];
  size_t got;
  while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    out.insert(out.end(), buf, buf + got);
    if (out.size() > limit) break;
  }
  std::fclose(f);
  return true;
}

// ---------------------------------------------------------------- the tracker part

struct Envelope {
  uint8_t flags = 0, count = 0, loop_start = 0, loop_end = 0, sus_start = 0, sus_end = 0;
  int8_t value[25] = {};
  uint16_t tick[25] = {};
};

struct Instrument {
  uint16_t fadeout = 0;
  uint8_t volume = 0;
  uint8_t key_note[120] = {}, key_sample[120] = {};
  Envelope env[3];
};

struct Sample {
  uint8_t gvol = 0, flags = 0, dvol = 0, dpan = 0;
  uint32_t length = 0, loop_start = 0, loop_end = 0, sus_start = 0, sus_end = 0, c5 = 0;
  const uint8_t* data = nullptr;
  uint64_t bytes = 0;
  int8_t byte(int64_t i) const {
    return (i >= 0 && static_cast<uint64_t>(i) < bytes) ? static_cast<int8_t>(data[i]) : 0;
  }
  int32_t frame16(int64_t i) const {
    if (i < 0 || static_cast<uint64_t>(i) * 2 + 1 >= bytes) return 0;
    return static_cast<int16_t>(rd16(data + 2 * i));
  }
};

struct Cell {
  uint8_t note = 0, inst = 0, vol = 0, cmd = 0, arg = 0;
};

struct Pattern {
  bool present = false;
  uint16_t rows = 0, length = 0;
  uint64_t at = 0;
  std::vector<Cell> grid;  // 200 x 64, once unpacked
};

const int kRows = 200, kChannels = 64;

// ---------------------------------------------------------------- the player

struct EnvState {
  uint8_t index = 0, loop_start = 0, loop_end = 0, mode = 0;
  int32_t ticks = 0, value = 0, slope = 0;
  bool on = false;
};

struct Channel {
  Cell cell;
  bool active = false;
  uint8_t key = 0;
  int instrument = 0, sample = 0;
  int32_t pitch = 0, centre = 0;
  uint32_t offset = 0;
  int trig = 0;
  bool release = false, cut = false, fading = false;
  uint8_t chvol = 0, chpan = 0;
  int32_t vol = 0;
  uint8_t dpan = 0;
  uint8_t env_vol = 0, env_pan = 0, env_pitch = 0;
  EnvState env[3];
  int32_t fade = 0;
  // mixer
  uint32_t pos = 0;
  uint8_t dir = 0, mode = 0;
  int32_t loop_start = 0, loop_end = 0;
  uint32_t freq = 0, mixvol = 0, mixpan = 0;
  bool bits16 = false;
  // command memories
  uint8_t mem_d = 0, mem_ef = 0, mem_g = 0, mem_h = 0, mem_s = 0, delay = 1;
  float vib_phase = 0;
  int32_t target = 0, tc = 0;
  bool vib_on = false;
};

}  // namespace

class Song {
 public:
  Song(const std::vector<uint8_t>& file, const Options& opt) : opt_(opt) { load(file); }

  void start();
  bool next(std::vector<int16_t>& block);
  void write_samples(FILE* to) const;

 private:
  void load(const std::vector<uint8_t>& file);
  void tracker(uint32_t at);
  void unpack(Pattern& p);
  const uint8_t* library(int which, uint64_t& size);

  void tick();
  void row_tick();
  void effect_tick();
  bool is_delay(const Channel& ch) const {
    return ch.cell.cmd == 19 && ((ch.cell.arg >> 4) == 0xD || (ch.cell.arg == 0 && (ch.mem_s >> 4) == 0xD));
  }
  void note_event(Channel& ch);
  void instrument_event(Channel& ch);
  void sample_start(Channel& ch);
  void volume_column(Channel& ch);
  void row_command(Channel& ch);
  void tick_command(Channel& ch);
  void envelopes(Channel& ch);
  bool env_step(EnvState& st, const Envelope& e);
  void finish(Channel& ch);
  void loop_setup(Channel& ch);
  uint32_t volume(const Channel& ch) const;
  uint32_t pan(const Channel& ch) const;
  uint32_t frequency(const Channel& ch) const;
  void mix(Channel& ch, int32_t* left, int32_t* right, uint32_t frames);
  void dump_tick(bool preroll);
  uint8_t order_at(int i) const { return i < static_cast<int>(orders_.size()) ? orders_[i] : 255; }
  const Instrument* inst(const Channel& ch) const {
    return ch.instrument >= 1 && ch.instrument <= static_cast<int>(instruments_.size())
               ? &instruments_[ch.instrument - 1] : nullptr;
  }
  const Sample* smp(const Channel& ch) const {
    return ch.sample >= 1 && ch.sample <= static_cast<int>(samples_.size()) ? &samples_[ch.sample - 1] : nullptr;
  }

  Options opt_;
  std::vector<uint8_t> u_;
  float outvol_ = 0;
  Store store_;
  std::vector<uint8_t> lib_data_[3];
  bool lib_tried_[3] = {}, lib_ok_[3] = {};
  bool lib_missing_[3] = {};

  uint8_t gvol_ = 0, speed0_ = 0, tempo0_ = 0;
  uint8_t cv_[64] = {}, cp_[64] = {};
  std::vector<uint8_t> orders_;
  std::vector<Instrument> instruments_;
  std::vector<Sample> samples_;
  std::vector<Pattern> patterns_;
  float pt_[7680];

  // song state
  uint8_t speed_ = 0, countdown_ = 0, tempo_ = 0, order_ = 0, row_ = 0;
  int pattern_ = 0, brk_ = -1;
  uint32_t ends_ = 0;
  uint64_t tick_no_ = 0;
  Channel ch_[kChannels];
};

namespace {
const Cell kNoCell = {};
}

void Song::load(const std::vector<uint8_t>& file) {
  if (file.size() <= 192) refuse("not an iXalance song (too short)");
  if (std::memcmp(file.data(), "IMPM", 4) == 0) refuse("an Impulse Tracker module, not an iXalance song");
  if (std::memcmp(file.data(), "IXS!", 4) != 0) refuse("not an iXalance song");
  const uint32_t t = rd32(&file[4]), g1 = rd32(&file[8]), g2 = rd32(&file[12]), packed = rd32(&file[16]);
  std::memcpy(&outvol_, &file[20], 4);
  if (56ull + packed > file.size()) refuse("the song's packed data is cut short");

  z_stream zs{};
  if (inflateInit(&zs) != Z_OK) refuse("can't unpack the song");
  zs.next_in = const_cast<Bytef*>(file.data() + 56);
  zs.avail_in = packed;
  std::vector<uint8_t> chunk(1 << 16);
  for (;;) {
    zs.next_out = chunk.data();
    zs.avail_out = static_cast<uInt>(chunk.size());
    const int ret = inflate(&zs, Z_NO_FLUSH);
    const size_t made = chunk.size() - zs.avail_out;
    u_.insert(u_.end(), chunk.begin(), chunk.begin() + made);
    if (u_.size() > kMaxUnpacked) { inflateEnd(&zs); refuse("the song's data unpacks to too much"); }
    if (ret == Z_STREAM_END || (ret != Z_OK)) break;
    if (made == 0 && zs.avail_in == 0) break;
  }
  inflateEnd(&zs);
  const uint64_t n = u_.size();
  if (n == 0 || t >= n || g1 >= n || g2 >= n || g2 < g1 || g1 + 4ull > n || g2 + 1ull > n)
    refuse("the song's data is damaged");

  Work work;
  FmGenerator(u_, store_, work).run(g1);
  WaveGenerator(u_, store_, work).run(g2);

  for (int j = 0; j < 7680; ++j) {
    if (j < 3840)
      pt_[j] = 1.0f / static_cast<float>(std::pow(2.0, static_cast<double>(j - 3840) * (-1.0 / 768.0)));
    else
      pt_[j] = static_cast<float>(std::pow(2.0, static_cast<double>(j - 3840) * (1.0 / 768.0)));
  }
  tracker(t);
}

const uint8_t* Song::library(int which, uint64_t& size) {
  if (!lib_tried_[which]) {
    lib_tried_[which] = true;
    if (!opt_.library_dir.empty()) {
      const LibraryFile& lf = kLibrary[which];
      for (const char* name : {lf.name, lf.alt}) {
        std::vector<uint8_t> data;
        if (read_file(opt_.library_dir + "/" + name, data, lf.size) && data.size() == lf.size &&
            sha256(data) == lf.sha256) {
          lib_data_[which] = std::move(data);
          lib_ok_[which] = true;
          break;
        }
      }
    }
  }
  if (!lib_ok_[which]) {
    lib_missing_[which] = true;
    size = 0;
    return nullptr;
  }
  size = lib_data_[which].size();
  return lib_data_[which].data();
}

void Song::tracker(uint32_t at) {
  Reader r{u_, at, "the tracker part"};
  if (r.u8() != 33) refuse("the song's tracker part is damaged");
  const uint16_t on = r.u16(), in = r.u16(), sn = r.u16(), pn = r.u16();
  gvol_ = r.u8();
  r.u8();
  speed0_ = r.u8();
  tempo0_ = r.u8();
  r.take(26);
  std::memcpy(cv_, r.take(64), 64);
  std::memcpy(cp_, r.take(64), 64);
  const uint8_t* ord = r.take(on);
  orders_.assign(ord, ord + on);

  instruments_.resize(in);
  for (Instrument& ins : instruments_) {
    if (r.u8() == 'i') continue;
    ins.fadeout = r.u16();
    ins.volume = r.u8();
    r.u8();
    const uint8_t k0 = r.u8(), k1 = r.u8();
    if (k1 < 128) {
      ins.key_note[0] = k0;
      ins.key_sample[0] = k1;
      const uint8_t* rest = r.take(238);
      for (int n = 1; n < 120; ++n) {
        ins.key_note[n] = rest[2 * n - 2];
        ins.key_sample[n] = rest[2 * n - 1];
      }
    } else {
      ins.key_note[0] = k0;
      ins.key_sample[0] = k1 & 0x7F;
      for (int n = 1; n < 120; ++n) {
        ins.key_note[n] = static_cast<uint8_t>(n);
        ins.key_sample[n] = k1 & 0x7F;
      }
    }
    for (Envelope& e : ins.env) {
      e.flags = r.u8();
      if (e.flags == 0) continue;
      e.count = r.u8();
      e.loop_start = r.u8();
      e.loop_end = r.u8();
      e.sus_start = r.u8();
      e.sus_end = r.u8();
      if (e.count > 25) refuse("an envelope has too many points");
      for (int i = 0; i < e.count; ++i) {
        e.value[i] = static_cast<int8_t>(r.u8());
        e.tick[i] = r.u16();
      }
    }
  }

  samples_.resize(sn);
  for (Sample& s : samples_) {
    if (r.u8() == 's') continue;
    s.gvol = r.u8();
    s.flags = r.u8();
    s.dvol = r.u8();
    s.length = r.u32();
    s.loop_start = r.u32();
    s.loop_end = r.u32();
    s.sus_start = r.u32();
    s.sus_end = r.u32();
    s.c5 = r.u32();
    r.take(4);
    s.dpan = r.u8();
    const uint8_t* nm = r.take(13);
    const void* zero = std::memchr(nm, 0, 13);
    if (!zero) refuse("a sample's name has no end");
    const std::string name(reinterpret_cast<const char*>(nm), static_cast<const uint8_t*>(zero) - nm);
    int lib = -1;
    for (int k = 0; k < 3; ++k)
      if (name == kLibrary[k].name) lib = k;
    if (lib >= 0) {
      if (s.flags & 2) refuse("a library sample is marked 16-bit");
      uint64_t size = 0;
      s.data = library(lib, size);
      s.bytes = size;
      s.length = static_cast<uint32_t>(size);
    } else {
      const Entry* e = store_.find(name);
      const uint64_t need = (s.flags & 2) ? static_cast<uint64_t>(s.length) * 2 : s.length;
      if (!e || e->data.size() < need) refuse("a sample's sound was not made");
      s.data = e->data.data();
      s.bytes = need;
    }
  }

  patterns_.resize(pn);
  for (Pattern& p : patterns_) {
    if (r.u8() == 'p') continue;
    p.present = true;
    p.length = r.u16();
    p.rows = r.u16();
    p.at = r.at;
    r.take(p.length);
  }

  // the order list
  int end = -1;
  for (size_t i = 0; i < orders_.size(); ++i)
    if (orders_[i] == 255) { end = static_cast<int>(i); break; }
  if (end < 0) refuse("the order list has no end");
  bool any = false;
  for (int i = 0; i < end; ++i) {
    if (orders_[i] == 254) continue;
    any = true;
    if (orders_[i] >= pn) refuse("the order list names a pattern that is not there");
    Pattern& p = patterns_[orders_[i]];
    if (p.grid.empty()) unpack(p);
  }
  if (!any) refuse("the order list has nothing to play");
  // the order index is a byte: a list whose patterns all lie past entry 255 never reaches one
  bool reachable = false;
  for (int i = 0; i < end && i < 256; ++i) reachable = reachable || orders_[i] != 254;
  if (!reachable) refuse("the order list has nothing to play");
  if (tempo0_ < 15) refuse("the song's tempo is too low");

  bool missing = false;
  std::string names;
  for (int k = 0; k < 3; ++k)
    if (lib_missing_[k]) {
      names += std::string(missing ? ", " : "") + kLibrary[k].name;
      missing = true;
    }
  if (missing) refuse("This song needs iXalance's library samples (" + names + "), which are not here");
}

void Song::unpack(Pattern& p) {
  if (!p.present || p.rows > kRows) refuse("the song plays a pattern that is missing or damaged");
  p.grid.assign(kRows * kChannels, Cell());
  for (int row = 0; row < p.rows; ++row)
    for (int c = 0; c < kChannels; ++c) p.grid[row * kChannels + c].vol = 255;
  uint8_t mask[kChannels] = {};
  Cell last[kChannels];
  for (Cell& c : last) c.vol = 255;
  uint64_t at = p.at;
  const uint64_t end = p.at + p.length;
  auto byte = [&]() -> uint8_t {
    if (at >= end) refuse("a pattern of the song is damaged");
    return u_[at++];
  };
  int row = 0;
  do {
    const uint8_t c = byte();
    if (c == 0) {
      ++row;
      continue;
    }
    const int chn = (c - 1) & 63;
    if (c & 0x80) mask[chn] = byte();
    const uint8_t m = mask[chn];
    if (row >= kRows) refuse("a pattern of the song is damaged");
    Cell& cell = p.grid[row * kChannels + chn];
    if (m & 1) last[chn].note = cell.note = byte();
    if (m & 2) last[chn].inst = cell.inst = byte();
    if (m & 4) last[chn].vol = cell.vol = byte();
    if (m & 8) {
      last[chn].cmd = cell.cmd = byte();
      last[chn].arg = cell.arg = byte();
    }
    if (m & 16) cell.note = last[chn].note;
    if (m & 32) cell.inst = last[chn].inst;
    if (m & 64) cell.vol = last[chn].vol;
    if (m & 128) {
      cell.cmd = last[chn].cmd;
      cell.arg = last[chn].arg;
    }
  } while (row < p.rows);
}

void Song::start() {
  speed_ = countdown_ = speed0_;
  tempo_ = tempo0_;
  ends_ = 0;
  brk_ = -1;
  row_ = 0;
  order_ = 0;
  while (orders_[order_] == 254) ++order_;
  pattern_ = orders_[order_];
  for (int c = 0; c < kChannels; ++c) {
    ch_[c] = Channel();
    ch_[c].chvol = cv_[c];
    ch_[c].chpan = cp_[c];
  }
  std::vector<int16_t> scratch;
  const uint32_t blk = to_u32((2.5 / static_cast<double>(tempo_)) * 44100.0);
  std::vector<int32_t> l(blk), r(blk);
  for (int i = 0; i < 35; ++i) {
    tick();
    dump_tick(true);
    std::fill(l.begin(), l.end(), 0);
    std::fill(r.begin(), r.end(), 0);
    for (Channel& ch : ch_)
      if (ch.active) mix(ch, l.data(), r.data(), blk);
  }
  row_ = 0;
  order_ = 0;
  countdown_ = speed_;
  brk_ = -1;
}

bool Song::next(std::vector<int16_t>& block) {
  if (ends_ != 0) return false;
  tick();
  dump_tick(false);
  const uint32_t blk = to_u32((2.5 / static_cast<double>(tempo_)) * 44100.0);
  std::vector<int32_t> l(blk, 0), r(blk, 0);
  for (Channel& ch : ch_)
    if (ch.active) mix(ch, l.data(), r.data(), blk);
  block.resize(2 * static_cast<size_t>(blk));
  for (uint32_t i = 0; i < blk; ++i) {
    block[2 * i] = static_cast<int16_t>(l[i]);
    block[2 * i + 1] = static_cast<int16_t>(r[i]);
  }
  return true;
}

void Song::dump_tick(bool preroll) {
  if (opt_.ticks) {
    std::fprintf(opt_.ticks, "T %llu%s ord=%u pat=%d row=%u spd=%u tempo=%u end=%u\n",
                 static_cast<unsigned long long>(tick_no_), preroll ? "p" : "", order_, pattern_, row_,
                 countdown_, tempo_, ends_);
    for (int c = 0; c < kChannels; ++c) {
      const Channel& ch = ch_[c];
      if (!ch.active) continue;
      std::fprintf(opt_.ticks,
                   " c%d s=%d pos=%u dir=%u lf=%u ls=%d le=%d f=%u v=%u p=%u bits=%d ft=%d pe=%u ve=%u fade=%d\n",
                   c, ch.sample, ch.pos, ch.dir, ch.mode, ch.loop_start, ch.loop_end, ch.freq, ch.mixvol,
                   ch.mixpan, ch.bits16 ? 16 : 8, ch.pitch, ch.env_pitch, ch.env_vol, ch.fade);
    }
  }
  ++tick_no_;
}

void Song::tick() {
  if (countdown_ != 0) --countdown_;
  if (countdown_ == 0) row_tick();
  else effect_tick();
}

void Song::row_tick() {
  countdown_ = speed_;
  const Pattern& cur = patterns_[pattern_];
  if (row_ >= cur.rows || brk_ >= 0) {
    row_ = static_cast<uint8_t>(brk_ >= 0 ? brk_ : 0);
    brk_ = -1;
    do {
      order_ = static_cast<uint8_t>(order_ + 1);
      if (order_at(order_) == 255) {
        order_ = 0;
        ++ends_;
      }
    } while (order_at(order_) == 254);
    pattern_ = order_at(order_);
  }
  const Pattern& p = patterns_[pattern_];
  for (int c = 0; c < kChannels; ++c) {
    Channel& ch = ch_[c];
    ch.cell = row_ < kRows && !p.grid.empty() ? p.grid[row_ * kChannels + c] : kNoCell;
    ch.trig = 0;
    const bool fresh = !(ch.cell.cmd == 7 || is_delay(ch));
    if (ch.cell.cmd == 8) {
      ch.vib_on = true;
    } else if (ch.vib_on) {
      ch.pitch = ch.centre;
      ch.vib_on = false;
    }
    if (fresh) {
      note_event(ch);
      instrument_event(ch);
    }
    if (ch.cell.cmd == 15) ch.offset = ch.cell.arg * 256u;
    if (ch.trig) sample_start(ch);
    if (fresh) volume_column(ch);
    row_command(ch);
    tick_command(ch);
    envelopes(ch);
    finish(ch);
    loop_setup(ch);
  }
  row_ = static_cast<uint8_t>(row_ + 1);
}

void Song::effect_tick() {
  for (Channel& ch : ch_) {
    ch.trig = 0;
    if (!is_delay(ch)) volume_column(ch);
    tick_command(ch);
    envelopes(ch);
    finish(ch);
  }
}

void Song::note_event(Channel& ch) {
  const uint8_t n = ch.cell.note;
  if (n == 0) return;
  if (n == 254) {
    ch.cut = true;
  } else if (n == 255) {
    ch.release = true;
  } else if (n <= 119) {
    ch.key = n;
    ch.env_vol = 64;
    ch.env_pitch = 32;
    ch.env_pan = 32;
    ch.fade = 1024;
    ch.cut = ch.release = ch.fading = false;
    ch.trig = 1;
    ch.offset = 0;
  } else {
    ch.fading = true;
  }
}

void Song::instrument_event(Channel& ch) {
  const uint8_t v = ch.cell.inst;
  if (v == 0) return;
  ch.instrument = v;
  ch.trig |= 2;
  const Instrument* ins = inst(ch);
  for (int k = 0; k < 3; ++k) {
    EnvState& st = ch.env[k];
    st.index = 0;
    st.ticks = 0;
    st.on = ins ? (ins->env[k].flags & 1) != 0 : false;
    st.value = ins ? ins->env[k].value[0] * 256 : 0;
  }
}

void Song::sample_start(Channel& ch) {
  const Instrument* ins = inst(ch);
  if (!ins) {  // no instrument (an undefined case)
    ch.active = false;
    return;
  }
  const uint8_t note = ins->key_note[ch.key], number = ins->key_sample[ch.key];
  ch.sample = number;
  ch.pitch = ch.centre = note * 64;
  const Sample* s = smp(ch);
  if (!s) {  // sample 0 or past the count (an undefined case)
    ch.sample = 0;
    ch.active = false;
    return;
  }
  if (s->flags & 1) {
    ch.active = true;
    ch.pos = ch.offset * 256u;
    ch.dir = 1;
    ch.bits16 = (s->flags & 2) != 0;
    ch.vol = s->dvol;
    ch.dpan = s->dpan;
  } else {
    ch.active = false;
  }
}

void Song::volume_column(Channel& ch) {
  const uint8_t b = ch.cell.vol;
  if (b <= 64) ch.vol = b;
  else if (b >= 128 && b <= 192) ch.dpan = static_cast<uint8_t>(b - 128);
}

void Song::row_command(Channel& ch) {
  ch.tc = -1;
  const uint8_t x = ch.cell.arg;
  switch (ch.cell.cmd) {
    case 3:
      brk_ = x;
      break;
    case 4: {
      uint8_t y = x;
      if (x) ch.mem_d = x;
      else y = ch.mem_d;
      const int hi = y >> 4, lo = y & 15;
      if (hi && lo) {
        if (lo == 15) ch.vol = std::min(ch.vol, 64);
        else if (hi == 15) ch.vol = std::max(0, ch.vol - lo);
      }
      break;
    }
    case 5:
    case 6: {
      uint8_t y = x;
      if (x) ch.mem_ef = x;
      else y = ch.mem_ef;
      const int hi = y >> 4, lo = y & 15;
      int amount;
      if (hi == 15) amount = lo * 4;
      else if (hi == 14) amount = lo;
      else break;
      if (ch.cell.cmd == 5) ch.pitch = std::max(0, ch.pitch - amount);
      else ch.pitch = std::min(7680, ch.pitch + amount);
      break;
    }
    case 7:
      if (x) {
        ch.mem_g = x;
        if (const Sample* s = smp(ch)) ch.vol = s->dvol;
      }
      if (ch.cell.note != 0) ch.target = ch.cell.note * 64;
      break;
    case 8:
      if (x) {
        ch.vib_phase = 0;
        ch.mem_h = x;
      }
      ch.centre = ch.pitch;
      break;
    case 15:
      ch.offset = x * 256u;
      break;
    case 19:
      if (x) ch.mem_s = x;
      if ((ch.mem_s >> 4) == 0xD && (ch.mem_s & 15) != 0) ch.delay = ch.mem_s & 15;
      break;
    default:
      break;
  }
}

void Song::tick_command(Channel& ch) {
  ch.tc = ch.tc + 1;
  const uint8_t x = ch.cell.arg;
  switch (ch.cell.cmd) {
    case 4: {
      uint8_t y = x;
      if (x) ch.mem_d = x;
      else y = ch.mem_d;
      const int hi = y >> 4, lo = y & 15;
      if (lo == 0) ch.vol = std::min(64, ch.vol + hi);
      else if (hi == 0) ch.vol = std::max(0, ch.vol - lo);
      return;
    }
    case 5:
    case 6: {
      uint8_t y = x;
      if (x) ch.mem_ef = x;
      else y = ch.mem_ef;
      const int hi = y >> 4;
      if (hi == 14 || hi == 15) return;
      if (ch.cell.cmd == 5) ch.pitch = std::max(0, ch.pitch - 4 * y);
      else ch.pitch = std::min(7680, ch.pitch + 4 * y);
      return;
    }
    case 7: {
      if (ch.pitch == ch.target) return;
      int32_t p;
      if (ch.pitch < ch.target) p = std::min(ch.target, ch.pitch + 4 * ch.mem_g);
      else p = std::max(ch.target, ch.pitch - 4 * ch.mem_g);
      if (ch.active) ch.pitch = p;
      return;
    }
    case 8: {
      const int hs = ch.mem_h >> 4, hd = ch.mem_h & 15;
      const float st = hs ? static_cast<float>(static_cast<double>(static_cast<float>(hs)) * 4.0) : 0.0f;
      const double dep = hd ? static_cast<double>(hd) * 4.0 : 0.0;
      const double s = std::sin(static_cast<double>(ch.vib_phase) * kTwoPiVibrato);
      ch.vib_phase = static_cast<float>(std::fmod(static_cast<double>(st * kStep256 + ch.vib_phase), 1.0));
      const float sum = static_cast<float>(ch.centre) + static_cast<float>(-(dep * s));
      ch.pitch = to_i32(std::round(static_cast<double>(sum)));
      break;
    }
    case 19:
      break;
    default:
      return;
  }
  // S, and H: a delayed note starts when its tick comes
  if ((ch.mem_s >> 4) == 0xD && ch.delay == ch.tc) {
    note_event(ch);
    instrument_event(ch);
    if (ch.trig) {
      sample_start(ch);
      volume_column(ch);
    }
    loop_setup(ch);
  }
}

bool Song::env_step(EnvState& st, const Envelope& e) {
  auto point = [&e](int i, int& v, int& t) {
    if (i >= 0 && i < 25) { v = e.value[i]; t = e.tick[i]; }
    else { v = 0; t = 0; }
  };
  if (st.mode != 0 || !(e.flags & 4)) {
    if (e.flags & 2) {
      st.mode |= 2;
      st.loop_start = e.loop_start;
      st.loop_end = e.loop_end;
    } else {
      st.loop_start = 0;
      st.loop_end = static_cast<uint8_t>(e.count - 1);
      st.mode &= static_cast<uint8_t>(~2);
    }
  } else {
    st.loop_start = e.sus_start;
    st.loop_end = e.sus_end;
    st.mode = 2;
  }
  int bv, bt;
  point(st.index, bv, bt);
  if (st.index != st.loop_end || (st.mode & 2)) {
    if (st.loop_start == st.loop_end && st.index == st.loop_start) {
      st.value = bv * 256;
      return true;
    }
    if (st.index == st.loop_end) {
      st.index = st.loop_start;
      point(st.index, bv, bt);
      st.value = bv * 256;
      st.ticks = bt;
    }
    int av, at;
    point((st.index + 1) & 255, av, at);
    st.slope = at == bt ? 0 : to_i32(static_cast<double>(av - bv) * 256.0) / (at - bt);
    st.ticks = st.ticks + 1;
    st.value = add(st.value, st.slope);
    if (st.ticks > at) {
      st.index = static_cast<uint8_t>(st.index + 1);
      st.value = av * 256;
    }
    return true;
  }
  st.value = bv * 256;
  return false;
}

void Song::envelopes(Channel& ch) {
  const Instrument* ins = inst(ch);
  if (!ins) return;
  if (ch.env[0].on) {
    if (!env_step(ch.env[0], ins->env[0])) ch.fading = true;
    ch.env_vol = static_cast<uint8_t>(ch.env[0].value >> 8);
  }
  if (ch.env[1].on) {
    env_step(ch.env[1], ins->env[1]);
    ch.env_pan = static_cast<uint8_t>(ch.env[1].value >> 8);
  }
  if (ch.env[2].on) {
    env_step(ch.env[2], ins->env[2]);
    ch.env_pitch = static_cast<uint8_t>(ch.env[2].value >> 8);
  }
}

uint32_t Song::volume(const Channel& ch) const {
  const Sample* s = smp(ch);
  const Instrument* ins = inst(ch);
  const uint32_t sg = s ? s->gvol : 0, iv = ins ? ins->volume : 0;
  uint32_t v = (sg * static_cast<uint32_t>(ch.vol) * ch.chvol) >> 10;
  v = (v * iv * ch.env_vol) >> 10;
  return (v * gvol_ * static_cast<uint32_t>(ch.fade)) >> 21;
}

uint32_t Song::pan(const Channel& ch) const {
  const int base = (ch.dpan & 0x80) ? (ch.dpan & 0x7F) : ch.chpan;
  const int c = static_cast<int8_t>(static_cast<uint8_t>(base - 32));
  return static_cast<uint8_t>((((32 - std::abs(c)) * (ch.env_pan - 32)) >> 5) + c + 32);
}

uint32_t Song::frequency(const Channel& ch) const {
  const Sample* s = smp(ch);
  int64_t j = static_cast<int64_t>(ch.env_pitch) * 32 + ch.pitch - 1024;
  if (j < 0) j = 0;  // outside the table: an undefined case
  if (j > 7679) j = 7679;
  const float v = pt_[j] * static_cast<float>(s ? s->c5 : 0);
  return to_u32(std::round(static_cast<double>(v)));
}

void Song::finish(Channel& ch) {
  if (ch.fading) {
    const Instrument* ins = inst(ch);
    const int32_t fo = ins ? ins->fadeout : 0;
    ch.fade = std::max(0, ch.fade - fo);
    if (ch.fade == 0 || fo == 0) ch.active = false;
  }
  if (ch.cut) {
    ch.cut = false;
    ch.active = false;
  }
  if (ch.active) {
    ch.mixvol = volume(ch);
    ch.mixpan = pan(ch);
    ch.freq = frequency(ch);
  }
}

void Song::loop_setup(Channel& ch) {
  if (!ch.active) return;
  ch.mixvol = volume(ch);
  ch.mixpan = pan(ch);
  ch.freq = frequency(ch);
  const Sample* s = smp(ch);
  if (!s) return;
  if (!(s->flags & 0x20) || ch.release) {
    if (!(s->flags & 0x10)) {
      ch.loop_start = 0;
      ch.loop_end = static_cast<int32_t>(s->length * 256u);
      ch.mode = 0;
    } else {
      ch.loop_start = static_cast<int32_t>(s->loop_start * 256u);
      ch.loop_end = static_cast<int32_t>(s->loop_end * 256u);
      ch.mode = (s->flags & 0x40) ? 2 : 1;
    }
  } else {
    ch.loop_start = static_cast<int32_t>(s->sus_start * 256u);
    ch.loop_end = static_cast<int32_t>(s->sus_end * 256u);
    ch.mode = (s->flags & 0x80) ? 2 : 1;
  }
  if (ch.release) ch.fading = true;
}

void Song::mix(Channel& ch, int32_t* left, int32_t* right, uint32_t frames) {
  if (ch.mode == 255) return;
  const Sample* s = smp(ch);
  if (!s) return;
  const uint32_t v = ch.mixvol, pn = ch.mixpan;
  const double ov = outvol_;
  const int32_t lv = static_cast<int32_t>(static_cast<uint32_t>(
      to_i64(static_cast<double>(static_cast<int32_t>(v * (64u - pn))) * ov)));
  const int32_t rv = static_cast<int32_t>(static_cast<uint32_t>(
      to_i64(static_cast<double>(static_cast<int32_t>(pn * v)) * ov)));
  const uint64_t lw = static_cast<uint64_t>(static_cast<int64_t>(lv));
  const uint64_t rw = static_cast<uint64_t>(static_cast<int64_t>(rv));
  uint64_t w = (lw << 16) | (rw << 32) | (lw << 48) | (rw & 0xFFFFFFFFull);
  if (rv < 0) w |= 0xFFFFFFFF00000000ull;
  int32_t lane[4];
  for (int i = 0; i < 4; ++i) lane[i] = static_cast<int16_t>(static_cast<uint16_t>(w >> (16 * i)));
  const int32_t b = to_i32(256.0 * static_cast<double>(ch.freq) / 44100.0);
  if (b <= 0) return;  // too slow to move (an undefined case): silent

  int64_t remaining = frames;
  uint32_t o = 0;
  while (remaining > 0) {
    if (static_cast<int32_t>(ch.pos) >= ch.loop_end) {
      if (ch.mode == 0) {
        ch.mode = 255;
        return;
      }
      if (ch.mode == 2) {
        ch.dir = 255;
        ch.pos = 2u * static_cast<uint32_t>(ch.loop_end) - ch.pos;
      } else if (ch.mode == 1) {
        ch.pos = ch.pos + static_cast<uint32_t>(ch.loop_start) - static_cast<uint32_t>(ch.loop_end);
      }
    }
    const uint64_t dist = static_cast<uint32_t>(static_cast<uint32_t>(ch.loop_end) - ch.pos);
    const int64_t num = static_cast<int64_t>((static_cast<uint64_t>(b) + dist) << 8) - b;
    int64_t x = static_cast<int32_t>(static_cast<uint32_t>(num / (static_cast<int64_t>(b) * 256)));
    if (x <= 0) x = 1;
    if (x > remaining) x = remaining;
    remaining -= x;
    const uint32_t pos0 = ch.pos;
    ch.pos = ch.pos + static_cast<uint32_t>(b) * static_cast<uint32_t>(x);
    bool edge = false;
    if (ch.pos >= static_cast<uint32_t>(ch.loop_end)) {
      x -= 1;
      edge = true;
    }
    uint32_t h = pos0;
    for (int64_t k = 0; k < x; ++k, ++o, h += static_cast<uint32_t>(b)) {
      const int64_t i = h >> 8;
      if (!ch.bits16) {
        const int32_t sv = s->byte(i) * 256;
        left[o] = sat(left[o] + ((sv * lane[0]) >> 16));
        right[o] = sat(right[o] + ((sv * lane[1]) >> 16));
      } else {
        const int32_t a = s->frame16(i), bn = s->frame16(i + ch.dir);
        const int32_t f = static_cast<int32_t>(h & 255) * 64;
        const int32_t pa = wrap16((a * (16384 - f)) >> 14);
        const int32_t pb = wrap16((bn * f) >> 14);
        const int32_t sl = sat(((pa * lane[0]) >> 16) + ((pb * lane[2]) >> 16));
        const int32_t sr = sat(((pa * lane[1]) >> 16) + ((pb * lane[3]) >> 16));
        left[o] = sat(left[o] + sl);
        right[o] = sat(right[o] + sr);
      }
    }
    if (edge) {
      const int32_t ps = static_cast<int32_t>(ch.pos);
      const int32_t q1 = sub(ps, b);
      int32_t q2 = q1;
      if (ch.mode == 2) q2 = sub(mul(2, ch.loop_end), ps);
      else if (ch.mode == 1) q2 = sub(add(ps, ch.loop_start), ch.loop_end);
      if (!ch.bits16) {
        const int32_t u = (q1 & 255) * s->byte(q1 >> 8) + (256 - (q1 & 255)) * s->byte(q2 >> 8);
        const uint32_t w24 = (static_cast<uint32_t>(u) & 0xFFFF) * 256u;
        const int32_t e0 = static_cast<int16_t>(static_cast<uint16_t>(w24 & 0xFFFF));
        const int32_t e1 = static_cast<int16_t>(static_cast<uint16_t>((w24 >> 16) | (w24 & 0xFFFF)));
        left[o] = sat(left[o] + ((e0 * lane[0]) >> 16));
        right[o] = sat(right[o] + ((e1 * lane[1]) >> 16));
      } else {
        const int32_t sv = s->frame16(q1 >> 8);
        left[o] = sat(left[o] + ((sv * lane[0]) >> 16));
        right[o] = sat(right[o] + ((sv * lane[1]) >> 16));
      }
      ++o;
    }
  }
}

void Song::write_samples(FILE* to) const {
  for (const Entry& e : store_.list()) {
    uint8_t n[4];
    const uint32_t nl = static_cast<uint32_t>(e.name.size()), dl = static_cast<uint32_t>(e.data.size());
    for (int i = 0; i < 4; ++i) n[i] = static_cast<uint8_t>(nl >> (8 * i));
    std::fwrite(n, 1, 4, to);
    std::fwrite(e.name.data(), 1, nl, to);
    for (int i = 0; i < 4; ++i) n[i] = static_cast<uint8_t>(dl >> (8 * i));
    std::fwrite(n, 1, 4, to);
    if (dl) std::fwrite(e.data.data(), 1, dl, to);
  }
}

Player::Player() = default;
Player::~Player() = default;

bool Player::next(std::vector<int16_t>& block) { return song_->next(block); }

void Player::write_samples(FILE* to) const { song_->write_samples(to); }

std::unique_ptr<Player> open(const std::vector<uint8_t>& file, const Options& options, std::string& error) {
  try {
    std::unique_ptr<Player> p(new Player());
    p->song_.reset(new Song(file, options));
    p->song_->start();
    return p;
  } catch (const Refusal& r) {
    error = r.why;
  } catch (const std::bad_alloc&) {
    error = "not enough memory for this song";
  }
  return nullptr;
}

std::string title(const std::vector<uint8_t>& file) {
  if (file.size() < 56 || std::memcmp(file.data(), "IXS!", 4) != 0) return "";
  std::string t(reinterpret_cast<const char*>(&file[24]), 32);
  while (!t.empty() && (t.back() == ' ' || t.back() == '\0')) t.pop_back();
  const size_t zero = t.find('\0');
  if (zero != std::string::npos) t.resize(zero);
  return t;
}

}  // namespace ixs
