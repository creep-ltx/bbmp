// SPDX-License-Identifier: GPL-3.0-or-later
// The effects of a Skale Tracker song.
//
// Skale's mixer sends each instrument, before its fader and BAL, to up to 16
// effect buses, each with a VST plug-in. What a bus returns goes to the master
// (after the master volume, as the instruments do) and, through a matrix in the
// global section, on to later buses. How the sends are stored and mixed was found
// by playing edited copies of songs through Skale's own player (SkalePlayer.dll
// 0.81 under Wine) with a plug-in that returns its input, and comparing:
//
//   send into a bus        instrument before fader and BAL x send level
//   return of a bus        plug-in output x bus fader / 2, then balance:
//                          left x 2 (1 - pan), right x 2 pan
//   bus to later bus       level x the plug-in's LEFT output with the source
//                          bus's fader and balance: (xL gl, xL gr); Skale's own
//                          quirk, it takes the left channel for both sides
//
// Each of these matched to under -110 dB. Across all 28 songs with effects, the
// level of what the buses return, next to the dry mix, came out as in Skale's
// player to within +-0.5 dB for most songs and 2.5 dB at most, where the loader's
// level of an instrument differs from Skale's. A bypassed bus is taken as silent
// (no song on Modland has one). The plug-ins are rewritten here from
// their sources: mda Delay, Stereo, Image, ThruZero and RoundPan from Paul
// Kellett's mda-vst (MIT licence, copyright (c) 2008 Paul Kellett; the notice is in
// csrc/synth/LICENSE.mda), Freeverb2 from Jezar's Freeverb (public
// domain). mda Delay was checked against the same code built as a VST for Skale's
// player. Freeverb2 is a closed plug-in of which only the successor's documentation
// is at hand; its parameters are taken as room size, damping, predelay, wet, dry,
// width, freeze on Jezar's model, which is a guess (see read_mixer). SChorus and
// sc Multi Tap Delay are closed and left out: those buses stay silent, as they are
// in Skale when the plug-in is missing.

#include "skale_fx.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace skale {
namespace {

constexpr float kRate = 44100.0f;

// ---- mda Delay ----
class MdaDelay : public Effect {
 public:
  MdaDelay() : buffer_(kSize + 2, 0.0f) {
    p_ = {0.50f, 0.27f, 0.70f, 0.50f, 0.33f, 0.50f};
    update();
  }
  void set_parameter(int i, float v) override {
    if (i >= 0 && i < 6) p_[i] = v;
    update();
  }
  void process(const float* a_, const float* b_, float* out_l, float* out_r, int n) override {
    float w = wet_, y = dry_, fb = fbk_, lx = lmix_, hx = hmix_, f = fil_, f0 = fil0_;
    int i = ipos_, s = kSize;
    int l = (i + ldel_) % (s + 1), r = (i + rdel_) % (s + 1);
    for (int k = 0; k < n; ++k) {
      const float a = a_[k], b = b_[k];
      const float ol = buffer_[l], or_ = buffer_[r];
      const float tmp = w * (a + b) + fb * (ol + or_);
      f0 = f * (f0 - tmp) + tmp;
      buffer_[i] = lx * f0 + hx * tmp;
      if (--i < 0) i = s;
      if (--l < 0) l = s;
      if (--r < 0) r = s;
      out_l[k] = y * a + ol;
      out_r[k] = y * b + or_;
    }
    ipos_ = i;
    fil0_ = std::fabs(f0) < 1.0e-10f ? 0.0f : f0;
  }

 private:
  static constexpr int kSize = 32766;
  void update() {
    float tmp;
    ldel_ = static_cast<int>(kSize * p_[0] * p_[0]);
    if (ldel_ < 4) ldel_ = 4;
    switch (static_cast<int>(p_[1] * 17.9f)) {
      case 17: tmp = 0.5000f; break;
      case 16: tmp = 0.6667f; break;
      case 15: tmp = 0.7500f; break;
      case 14: tmp = 0.8333f; break;
      case 13: tmp = 1.0000f; break;
      case 12: tmp = 1.2000f; break;
      case 11: tmp = 1.3333f; break;
      case 10: tmp = 1.5000f; break;
      case 9: tmp = 2.0000f; break;
      default: tmp = 4.0f * p_[1]; break;
    }
    rdel_ = static_cast<int>(kSize * p_[0] * p_[0] * tmp);
    if (rdel_ > kSize) rdel_ = kSize;
    if (rdel_ < 4) rdel_ = 4;
    fil_ = p_[3];
    if (p_[3] > 0.5f) {
      fil_ = 0.5f * fil_ - 0.25f;
      lmix_ = -2.0f * fil_;
      hmix_ = 1.0f;
    } else {
      hmix_ = 2.0f * fil_;
      lmix_ = 1.0f - hmix_;
    }
    fil_ = static_cast<float>(std::exp(-6.2831853f * std::pow(10.0f, 2.2f + 4.5f * fil_) / kRate));
    fbk_ = 0.495f * p_[2];
    wet_ = 1.0f - p_[4];
    wet_ = p_[5] * (1.0f - wet_ * wet_);
    dry_ = p_[5] * 2.0f * (1.0f - p_[4] * p_[4]);
  }
  std::array<float, 6> p_;
  std::vector<float> buffer_;
  int ipos_ = 0, ldel_ = 4, rdel_ = 4;
  float wet_ = 0, dry_ = 0, fbk_ = 0, lmix_ = 0, hmix_ = 0, fil_ = 0, fil0_ = 0;
};

// ---- mda Stereo ----
class MdaStereo : public Effect {
 public:
  MdaStereo() : buffer_(4800, 0.0f) {
    p_ = {0.78f, 0.43f, 0.50f, 0.00f, 0.50f};
    update();
  }
  void set_parameter(int i, float v) override {
    if (i >= 0 && i < 5) p_[i] = v;
    update();
  }
  void process(const float* in_l, const float* in_r, float* out_l, float* out_r, int n) override {
    float ph = phi_;
    int bp = bufpos_;
    for (int k = 0; k < n; ++k) {
      const float a = in_l[k] + in_r[k];
      buffer_[bp] = a;
      const int tmp = mod_ > 0.f ? (bp + static_cast<int>(del_ + std::fabs(mod_ * std::sin(ph)))) % 4410
                                 : (bp + static_cast<int>(del_)) % 4410;
      const float b = buffer_[tmp];
      out_l[k] = a * li_ - b * ld_;
      out_r[k] = a * ri_ - b * rd_;
      if (--bp < 0) bp = 4410;
      if (mod_ > 0.f) ph += dphi_;
    }
    bufpos_ = bp;
    phi_ = static_cast<float>(std::fmod(ph, 6.2831853f));
  }

 private:
  void update() {
    dphi_ = static_cast<float>(3.141 * std::pow(10.0, -2.0 + 3.0 * p_[4]) / kRate);
    mod_ = static_cast<float>(2100.0 * std::pow(p_[3], 2));
    if (p_[0] < 0.5f) {
      li_ = 0.25f + 1.5f * p_[0];
      ld_ = 0.0f;
      ri_ = 2.0f * p_[0];
      rd_ = 1.0f - ri_;
    } else {
      li_ = 1.5f - p_[0];
      ld_ = p_[0] - 0.5f;
      ri_ = li_;
      rd_ = -ld_;
    }
    del_ = static_cast<float>(20.0 + 2080.0 * std::pow(p_[1], 2));
    if (p_[2] > 0.5f) {
      li_ *= (1.0f - p_[2]) * 2.0f;
      ld_ *= (1.0f - p_[2]) * 2.0f;
    } else {
      ri_ *= 2 * p_[2];
      rd_ *= 2 * p_[2];
    }
    const float w = 0.5f + std::fabs(p_[0] - 0.5f);
    ri_ *= w; rd_ *= w; li_ *= w; ld_ *= w;
  }
  std::array<float, 5> p_;
  std::vector<float> buffer_;
  int bufpos_ = 0;
  float phi_ = 0, dphi_ = 0, mod_ = 0, li_ = 0, ld_ = 0, ri_ = 0, rd_ = 0, del_ = 0;
};

// ---- mda Image ----
class MdaImage : public Effect {
 public:
  MdaImage() { p_ = {0.6f, 0.75f, 0.5f, 0.75f, 0.5f, 0.5f}; update(); }
  void set_parameter(int i, float v) override {
    if (i >= 0 && i < 6) p_[i] = v;
    update();
  }
  void process(const float* in_l, const float* in_r, float* out_l, float* out_r, int n) override {
    for (int k = 0; k < n; ++k) {
      const float a = in_l[k], b = in_r[k];
      out_l[k] = l2l_ * a + r2l_ * b;
      out_r[k] = r2r_ * b + l2r_ * a;
    }
  }

 private:
  void update() {
    const float w = 4.f * p_[1] - 2.f, k = 2.f * p_[2], c = 4.f * p_[3] - 2.f, b = 2.f * p_[4];
    float g = static_cast<float>(std::pow(10.0, 2.0 * p_[5] - 1.0));
    switch (static_cast<int>(p_[0] * 3.9)) {
      case 0: r2l_ = g * c * (2.f - b); l2l_ = g * w * (2.f - k); r2r_ = g * c * b; l2r_ = -g * w * k; break;
      case 1: l2l_ = g * c * (2.f - b); r2l_ = g * w * (2.f - k); l2r_ = g * c * b; r2r_ = -g * w * k; break;
      case 2:
        g *= 0.5f;
        l2l_ = g * (c * (2.f - b) + w * (2.f - k));
        r2l_ = g * (c * (2.f - b) - w * (2.f - k));
        l2r_ = g * (c * b - w * k);
        r2r_ = g * (c * b + w * k);
        break;
      case 3:
        g *= 0.5f;
        l2l_ = g * (2.f - b) * (2.f - k);
        r2l_ = g * (2.f - b) * k;
        l2r_ = -g * b * (2.f - k);
        r2r_ = g * b * k;
        break;
    }
  }
  std::array<float, 6> p_;
  float l2l_ = 0, l2r_ = 0, r2l_ = 0, r2r_ = 0;
};

// ---- mda ThruZero ----
// Skale's copy has four parameters; the fifth of later versions (the least delay)
// is at 1.0, where it does nothing.
class MdaThruZero : public Effect {
 public:
  MdaThruZero() : buffer_(kBuf, 0.0f), buffer2_(kBuf, 0.0f) {
    p_ = {0.30f, 0.43f, 0.47f, 0.30f, 1.00f};
    update();
  }
  void set_parameter(int i, float v) override {
    if (i == 3) phi_ = 0.0f;
    if (i >= 0 && i < 5) p_[i] = v;
    update();
  }
  void process(const float* in_l, const float* in_r, float* out_l, float* out_r, int n) override {
    float f = fb_, f1 = fb1_, f2 = fb2_, ph = phi_;
    int bp = bufpos_;
    for (int k = 0; k < n; ++k) {
      float a = in_l[k], b = in_r[k];
      ph += rat_;
      if (ph > 1.0f) ph -= 2.0f;
      bp = (bp - 1) & (kBuf - 1);
      buffer_[bp] = a + f * f1;
      buffer2_[bp] = b + f * f2;
      float tmpf = dem_ + dep_ * (1.0f - ph * ph);
      int tmp = static_cast<int>(tmpf);
      tmpf -= tmp;
      tmp = (tmp + bp) & (kBuf - 1);
      const int tmpi = (tmp + 1) & (kBuf - 1);
      f1 = buffer_[tmp];
      f2 = buffer2_[tmp];
      f1 = tmpf * (buffer_[tmpi] - f1) + f1;
      f2 = tmpf * (buffer2_[tmpi] - f2) + f2;
      out_l[k] = a * dry_ - f1 * wet_;
      out_r[k] = b * dry_ - f2 * wet_;
    }
    if (std::fabs(f1) > 1.0e-10f) { fb1_ = f1; fb2_ = f2; } else { fb1_ = fb2_ = 0.0f; }
    phi_ = ph;
    bufpos_ = bp;
  }

 private:
  static constexpr int kBuf = 2048;
  void update() {
    rat_ = static_cast<float>(std::pow(10.0f, 3.f * p_[0] - 2.f) * 2.f / kRate);
    dep_ = 2000.0f * p_[1] * p_[1];
    dem_ = dep_ - dep_ * p_[4];
    dep_ -= dem_;
    wet_ = p_[2];
    dry_ = 1.f - wet_;
    if (p_[0] < 0.01f) { rat_ = 0.0f; phi_ = 0.0f; }
    fb_ = 1.9f * p_[3] - 0.95f;
  }
  std::array<float, 5> p_;
  std::vector<float> buffer_, buffer2_;
  int bufpos_ = 0;
  float phi_ = 0, fb_ = 0, fb1_ = 0, fb2_ = 0, rat_ = 0, dep_ = 0, dem_ = 0, wet_ = 0, dry_ = 0;
};

// ---- mda RoundPan ----
class MdaRoundPan : public Effect {
 public:
  MdaRoundPan() { p_ = {0.5f, 0.8f}; dphi_ = 5.0f / kRate; }
  void set_parameter(int i, float v) override {
    if (i == 0) { p_[0] = v; phi_ = static_cast<float>(6.2831853 * (p_[0] - 0.5)); }
    if (i == 1) p_[1] = v;
    if (p_[1] > 0.55f) dphi_ = static_cast<float>(20.0 * (p_[1] - 0.55) / kRate);
    else if (p_[1] < 0.45f) dphi_ = static_cast<float>(-20.0 * (0.45 - p_[1]) / kRate);
    else dphi_ = 0.0f;
  }
  void process(const float* in_l, const float* in_r, float* out_l, float* out_r, int n) override {
    const float x = 0.5f, y = 0.7854f, fourpi = 12.566371f;
    float ph = phi_;
    for (int k = 0; k < n; ++k) {
      const float a = x * (in_l[k] + in_r[k]);
      out_l[k] = static_cast<float>(a * -std::sin((x * ph) - y));
      out_r[k] = static_cast<float>(a * std::sin((x * ph) + y));
      ph += dphi_;
    }
    if (ph < 0.0f) ph += fourpi; else if (ph > fourpi) ph -= fourpi;
    phi_ = ph;
  }

 private:
  std::array<float, 2> p_;
  float phi_ = 0, dphi_ = 0;
};

// ---- Freeverb2: Jezar's Freeverb model ----
class Freeverb : public Effect {
 public:
  Freeverb() {
    const int comb[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    const int allpass[4] = {556, 441, 341, 225};
    for (int i = 0; i < 8; ++i) {
      comb_l_[i].buffer.assign(comb[i], 0.0f);
      comb_r_[i].buffer.assign(comb[i] + kSpread, 0.0f);
    }
    for (int i = 0; i < 4; ++i) {
      allpass_l_[i].buffer.assign(allpass[i], 0.0f);
      allpass_r_[i].buffer.assign(allpass[i] + kSpread, 0.0f);
    }
    update();
  }
  // 0 room size, 1 damping, 2 predelay (not played), 3 wet, 4 dry, 5 width, 6 freeze
  void set_parameter(int i, float v) override {
    switch (i) {
      case 0: roomsize_ = v * 0.28f + 0.7f; break;
      case 1: damp_ = v * 0.4f; break;
      case 3: wet_ = v * 3.0f; break;
      case 4: dry_ = v * 2.0f; break;
      case 5: width_ = v; break;
      case 6: mode_ = v; break;
      default: break;
    }
    update();
  }
  void process(const float* in_l, const float* in_r, float* out_l, float* out_r, int n) override {
    for (int k = 0; k < n; ++k) {
      float ol = 0, or_ = 0;
      const float input = (in_l[k] + in_r[k]) * gain_;
      for (int i = 0; i < 8; ++i) {
        ol += comb_l_[i].process(input, feedback_, damp1_);
        or_ += comb_r_[i].process(input, feedback_, damp1_);
      }
      for (int i = 0; i < 4; ++i) {
        ol = allpass_l_[i].process(ol);
        or_ = allpass_r_[i].process(or_);
      }
      out_l[k] = ol * wet1_ + or_ * wet2_ + in_l[k] * dry_;
      out_r[k] = or_ * wet1_ + ol * wet2_ + in_r[k] * dry_;
    }
  }

 private:
  static constexpr int kSpread = 23;
  struct Comb {
    std::vector<float> buffer;
    size_t idx = 0;
    float store = 0;
    float process(float input, float feedback, float damp) {
      const float output = buffer[idx];
      store = output * (1.0f - damp) + store * damp;
      if (std::fabs(store) < 1e-30f) store = 0;
      buffer[idx] = input + store * feedback;
      if (++idx >= buffer.size()) idx = 0;
      return output;
    }
  };
  struct Allpass {
    std::vector<float> buffer;
    size_t idx = 0;
    float process(float input) {
      const float bufout = buffer[idx];
      buffer[idx] = input + bufout * 0.5f;
      if (++idx >= buffer.size()) idx = 0;
      return bufout - input;
    }
  };
  void update() {
    wet1_ = wet_ * (width_ / 2 + 0.5f);
    wet2_ = wet_ * ((1 - width_) / 2);
    if (mode_ >= 0.5f) { feedback_ = 1; damp1_ = 0; gain_ = 0; }
    else { feedback_ = roomsize_; damp1_ = damp_; gain_ = 0.015f; }
  }
  Comb comb_l_[8], comb_r_[8];
  Allpass allpass_l_[4], allpass_r_[4];
  float roomsize_ = 0.5f * 0.28f + 0.7f, damp_ = 0.5f * 0.4f, wet_ = 1.0f, dry_ = 0.0f, width_ = 1.0f, mode_ = 0.0f;
  float wet1_ = 0, wet2_ = 0, feedback_ = 0, damp1_ = 0, gain_ = 0;
};

// ---- reading the file ----

uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
float f32(const uint8_t* p) { const uint32_t v = u32(p); float f; std::memcpy(&f, &v, 4); return f; }

struct Chunk { uint16_t id, tag; const uint8_t* data; size_t size; };

// A list of chunks up to its end chunk; false when it doesn't fill `size` exactly
bool chunks(const uint8_t* d, size_t size, std::vector<Chunk>& out) {
  out.clear();
  size_t at = 0;
  while (at + 8 <= size) {
    const uint32_t length = u32(d + at + 4);
    if (length > size - at - 8) return false;
    out.push_back({u16(d + at), u16(d + at + 2), d + at + 8, length});
    at += 8 + length;
    if (out.back().tag == 0xFFFF) break;
  }
  return at == size && !out.empty() && out.back().tag == 0xFFFF;
}

size_t cstring_end(const uint8_t* d, size_t size, size_t from) {
  while (from < size && d[from]) ++from;
  return from < size ? from + 1 : size;
}

// Where an instrument record's list of samples begins (as the format research's
// reference parser finds it): the first place from which the rest of the record
// reads as a list of chunks to its end, with every sample in it of its own size.
long sample_list(const uint8_t* d, size_t size, size_t from) {
  std::vector<Chunk> list;
  for (size_t off = from; off + 8 <= size; ++off) {
    const uint16_t id = u16(d + off), tag = u16(d + off + 2);
    if ((id != 1 && id != 3) || (tag != 1 && tag != 2 && tag != 0xFFFF)) continue;
    if (!chunks(d + off, size - off, list)) continue;
    bool ok = true;
    for (const Chunk& c : list) {
      if (c.id != 1 || c.tag != 1) continue;
      const size_t n2 = cstring_end(c.data, c.size, 1);
      if (n2 + 27 > c.size) continue;
      const uint32_t audio = u32(c.data + n2 + 18);
      if (n2 + 27 + static_cast<uint64_t>(audio) + 3 != c.size) { ok = false; break; }
    }
    if (ok) return static_cast<long>(off);
  }
  return -1;
}

std::string file_name(const std::string& path) {
  const size_t cut = path.find_last_of("\\/");
  std::string name = cut == std::string::npos ? path : path.substr(cut + 1);
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (name.size() > 4 && name.compare(name.size() - 4, 4, ".dll") == 0) name.resize(name.size() - 4);
  return name;
}

}  // namespace

std::unique_ptr<Effect> make_effect(const std::string& dll) {
  const std::string name = file_name(dll);
  if (name == "mda delay") return std::make_unique<MdaDelay>();
  if (name == "mda stereo") return std::make_unique<MdaStereo>();
  if (name == "mda image") return std::make_unique<MdaImage>();
  if (name == "mda thruzero") return std::make_unique<MdaThruZero>();
  if (name == "mda roundpan") return std::make_unique<MdaRoundPan>();
  if (name == "freeverb2") return std::make_unique<Freeverb>();
  return nullptr;
}

bool Mixer::fed(int slot) const {
  const Bus& b = buses[slot];
  if (!b.effect || b.bypassed) return false;
  for (const auto& s : instrument_sends)
    if (s[slot] != 0.0f) return true;
  return false;
}

bool Mixer::any() const {
  for (int s = 0; s < kSlots; ++s)
    if (fed(s)) return true;
  return false;
}

bool read_mixer(const uint8_t* data, size_t size, Mixer& mixer) {
  mixer = Mixer();
  if (size < 17 || std::memcmp(data + 8, "ALIM3", 5) != 0) return false;
  const uint32_t version = u32(data + 13);
  std::vector<Chunk> top;
  // the top level has no end chunk of its own at the very end in every file:
  // walk it by hand
  size_t at = 0;
  while (at + 8 <= size) {
    const uint32_t length = u32(data + at + 4);
    if (length > size - at - 8) break;
    top.push_back({u16(data + at), u16(data + at + 2), data + at + 8, length});
    at += 8 + length;
    if (top.back().tag == 0xFFFF) break;
  }
  std::vector<Chunk> recs;
  for (const Chunk& c : top) {
    if (c.tag == 4 && chunks(c.data, c.size, recs)) {
      for (const Chunk& r : recs) {
        if (r.tag == 0xFFFF || r.size < 3) continue;
        const int slot = r.data[0];
        if (slot >= kSlots) continue;
        const size_t n1 = cstring_end(r.data, r.size, 1);
        const size_t n2 = cstring_end(r.data, r.size, n1);
        if (n2 + 14 > r.size) continue;
        const uint32_t count = u32(r.data + n2 + 10);
        if (count > 256 || n2 + 14 + count * 4 > r.size) continue;
        Bus& bus = mixer.buses[slot];
        bus.dll.assign(reinterpret_cast<const char*>(r.data + 1), n1 - 2);
        bus.name.assign(reinterpret_cast<const char*>(r.data + n1), n2 - n1 - 1);
        bus.volume = f32(r.data + n2 + 1);
        bus.pan = f32(r.data + n2 + 5);
        bus.bypassed = r.data[n2 + 9] != 0;
        if (!std::isfinite(bus.volume) || bus.volume < 0 || bus.volume > 16) bus.volume = 0.8f;
        if (!std::isfinite(bus.pan) || bus.pan < 0 || bus.pan > 1) bus.pan = 0.5f;
        bus.effect = make_effect(bus.dll);
        if (bus.effect) {
          // A VST's parameters are 0 to 1. mda Delay and Stereo make positions in
          // their buffers of them, and read far outside with a larger number.
          for (uint32_t i = 0; i < count; ++i) {
            const float v = f32(r.data + n2 + 14 + i * 4);
            if (std::isfinite(v)) bus.effect->set_parameter(static_cast<int>(i), std::clamp(v, 0.0f, 1.0f));
          }
        } else {
          mixer.missing.push_back(file_name(bus.dll));
        }
      }
    } else if (c.tag == 3 && chunks(c.data, c.size, recs)) {
      for (const Chunk& r : recs) {
        if (r.tag == 0xFFFF || r.size < 2) continue;
        const int index = r.data[0];
        const size_t name_end = cstring_end(r.data, r.size, 1);
        const long start = sample_list(r.data, r.size, name_end);
        if (start < 0) continue;
        std::array<float, kSlots> sends{};
        bool found = false;
        if (version == 7600 && start >= 132) {
          // 0.76: 16 records of flag, level, bus number just before the list
          bool sane = true;
          for (int k = 0; k < kSlots && sane; ++k) {
            const uint8_t* p = r.data + start - 132 + 6 * k;
            sane = p[0] <= 1 && p[5] == k;
          }
          if (sane) {
            found = true;
            for (int k = 0; k < kSlots; ++k) {
              const uint8_t* p = r.data + start - 132 + 6 * k;
              if (p[0]) sends[k] = f32(p + 1);
            }
          }
        } else if (version < 7600 && start >= 262) {
          // before 0.76: 16 records of flag, level, four zero bytes, 1.0
          bool sane = true;
          for (int k = 0; k < kSlots && sane; ++k) {
            const uint8_t* p = r.data + start - 262 + 13 * k;
            sane = p[0] <= 1 && u32(p + 5) == 0 && f32(p + 9) == 1.0f;
          }
          if (sane) {
            found = true;
            for (int k = 0; k < kSlots; ++k) {
              const uint8_t* p = r.data + start - 262 + 13 * k;
              if (p[0]) sends[k] = f32(p + 1);
            }
          }
        }
        if (!found) continue;
        for (float& v : sends)
          if (!std::isfinite(v) || v < 0 || v > 16) v = 0;
        if (static_cast<size_t>(index) >= mixer.instrument_sends.size())
          mixer.instrument_sends.resize(index + 1);
        mixer.instrument_sends[index] = sends;
      }
    } else if (c.tag == 5 && c.size == 424) {
      for (int i = 0; i < 32; ++i) {
        const uint8_t* p = c.data + 136 + 9 * i;
        const uint32_t to = u32(p + 1);
        const float level = f32(p + 5);
        const int from = i / 4;
        if (p[0] && to < static_cast<uint32_t>(kSlots) && static_cast<int>(to) > from && std::isfinite(level))
          mixer.bus_sends.push_back({from, static_cast<int>(to), level});
      }
    }
  }
  return mixer.any();
}

void run_buses(Mixer& mixer, std::array<std::vector<float>, kSlots>& inputs, float* out, int frames) {
  std::vector<float> in_l(frames), in_r(frames), out_l(frames), out_r(frames);
  for (int s = 0; s < kSlots; ++s) {
    Bus& bus = mixer.buses[s];
    if (inputs[s].empty() || !bus.effect || bus.bypassed) continue;
    const std::vector<float>& in = inputs[s];
    for (int k = 0; k < frames; ++k) { in_l[k] = in[2 * k]; in_r[k] = in[2 * k + 1]; }
    bus.effect->process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), frames);
    const float g = 0.5f * bus.volume, gl = g * 2.0f * (1.0f - bus.pan), gr = g * 2.0f * bus.pan;
    for (int k = 0; k < frames; ++k) {
      out[2 * k] += out_l[k] * gl;
      out[2 * k + 1] += out_r[k] * gr;
    }
    for (const Send& send : mixer.bus_sends) {
      if (send.from != s) continue;
      std::vector<float>& to = inputs[send.to];
      if (to.empty()) to.assign(static_cast<size_t>(frames) * 2, 0.0f);
      for (int k = 0; k < frames; ++k) {
        to[2 * k] += send.level * out_l[k] * gl;
        to[2 * k + 1] += send.level * out_l[k] * gr;
      }
    }
  }
}

}  // namespace skale
