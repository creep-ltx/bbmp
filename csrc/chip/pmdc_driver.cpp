// SPDX-License-Identifier: GPL-3.0-or-later
//
// pmdc: the PMD driver, written for this player from a description of PMD's behaviour
// (clean room). See pmdc_driver.h. Section numbers (§) are those of the description.

#include "pmdc_driver.h"

#include <algorithm>
#include <cstring>

namespace pmdc {

namespace {

constexpr I REST = 0xFF;
constexpr I STEP = 1181552;           // one timer step (§3.1)
constexpr long READ_LIMIT = 20000;    // bytes one part may read in one tick (bad songs loop)

// carriers and "modulator TL" mask by algorithm (§2.2)
constexpr int CARRIER[8] = {0x80, 0x80, 0x80, 0x80, 0xA0, 0xE0, 0xE0, 0xF0};
constexpr int MODTL[8] = {0xEE, 0xEE, 0xEE, 0xEE, 0xCC, 0x88, 0x88, 0x00};

constexpr int FNUM[12] = {0x26A, 0x28F, 0x2B6, 0x2DF, 0x30B, 0x339,
                          0x36A, 0x39E, 0x3D5, 0x410, 0x44E, 0x48F};
constexpr int SSGT[12] = {0xEE8, 0xE12, 0xD48, 0xC89, 0xBD5, 0xB2B,
                          0xA8A, 0x9F3, 0x964, 0x8DD, 0x85E, 0x7E6};
constexpr int ADT[12] = {0x6264, 0x6840, 0x6E74, 0x7506, 0x7BFC, 0x835E,
                         0x8B2E, 0x9376, 0x9C3C, 0xA588, 0xAF62, 0xB9D0};
constexpr uint32_t PPZT[12] = {0x8000, 0x87A6, 0x8FB3, 0x9838, 0xA146, 0xAADE,
                               0xB4FF, 0xBFCC, 0xCB34, 0xD747, 0xE418, 0xF1A5};

// semitone 12-15 of a note that is not a rest: no Modland song reaches a pitch table
// with one; take the nearest entry instead of reading past the table
int semi(I n) { int s = static_cast<int>(n & 15); return s > 11 ? 11 : s; }

// SSG effects 0-12 (§11.1): rows of duration, tone lo/hi, noise, mixer, volume,
// envelope lo/hi, shape, tone sweep, noise sweep
struct Effect { int priority; int rows; int row[2][11]; };
constexpr Effect EFFECTS[13] = {
    {1, 2, {{1, 220, 5, 31, 54, 15, 0, 0, 0, 127, 0}, {8, 164, 6, 0, 62, 16, 176, 4, 0, 127, 0}}},
    {1, 1, {{14, 144, 1, 7, 54, 16, 184, 11, 0, 93, -14}}},
    {1, 2, {{2, 188, 2, 0, 54, 15, 0, 0, 0, 100, 0}, {14, 132, 3, 0, 54, 16, 196, 9, 0, 100, 0}}},
    {1, 2, {{2, 244, 1, 5, 54, 15, 0, 0, 0, 60, 0}, {14, 108, 2, 0, 54, 16, 196, 9, 0, 60, 0}}},
    {1, 2, {{2, 44, 1, 0, 54, 15, 0, 0, 0, 50, 0}, {14, 144, 1, 0, 54, 16, 196, 9, 0, 50, 0}}},
    {1, 1, {{2, 55, 0, 0, 62, 16, 44, 1, 0, 100, 0}}},
    {1, 1, {{16, 0, 0, 15, 55, 16, 184, 11, 0, 0, -15}}},
    {1, 1, {{6, 39, 0, 0, 54, 16, 244, 1, 0, 0, 0}}},
    {1, 1, {{32, 39, 0, 0, 54, 16, 136, 19, 0, 0, 0}}},
    {1, 1, {{31, 40, 0, 31, 54, 16, 136, 19, 0, 0, -15}}},
    {1, 1, {{31, 30, 0, 0, 54, 16, 136, 19, 0, 0, 0}}},
    {2, 2, {{3, 221, 1, 15, 55, 16, 232, 3, 0, 0, 113}, {2, 221, 1, 0, 55, 16, 232, 3, 0, 0, 0}}},
    {2, 2, {{1, 44, 1, 0, 62, 16, 44, 1, 13, 0, 0}, {6, 44, 1, 0, 62, 16, 16, 39, 0, 80, 0}}},
};

I tbToT(I tb) {
  if (tb >= 256) return 255;
  const I t = ((8800 / (256 - tb)) + 1) / 2;
  return t > 255 ? 255 : t;
}

I tToTb(I t) {
  if (t < 18) return 0;
  I tb = 256 - 0x112C / t;
  if (0x112C % t >= 128) tb--;
  return tb;
}

std::string num(I v) { return std::to_string(v); }

}  // namespace

// ---------------------------------------------------------------- random numbers (§13)

Random::Random() {
  int64_t r[34];
  r[0] = 1;
  for (int i = 1; i <= 30; i++) {
    r[i] = (16807 * r[i - 1]) % 2147483647;
    if (r[i] < 0) r[i] += 2147483647;
  }
  for (int i = 31; i <= 33; i++) r[i] = r[i - 31];
  for (int i = 0; i < 34; i++) r_[i] = static_cast<uint32_t>(r[i]);
  for (i_ = 34; i_ < 344; i_++) r_[i_ % 34] = r_[(i_ - 31) % 34] + r_[(i_ - 3) % 34];
}

uint32_t Random::next() {
  const uint32_t v = r_[(i_ - 31) % 34] + r_[(i_ - 3) % 34];
  r_[i_ % 34] = v;
  i_++;
  if (i_ >= 344 + 34 * 1000000) i_ -= 34 * 1000000;   // keep the index small; same residues
  drawn++;
  const uint32_t out = v >> 1;
  if (onDraw) onDraw(user, out);
  return out;
}

// ---------------------------------------------------------------- data

Driver::Driver(Random& rng, Sink* sink) : rng_(rng), sink_(sink), area_(0x10000, 0x80) {}

int Driver::rd(I d) const {
  const I i = d + 1;
  return i >= 1 && i < static_cast<I>(area_.size()) ? area_[static_cast<size_t>(i)] : 0x80;
}

void Driver::wd(I d, int v) {
  const I i = d + 1;
  if (i >= 1 && i < static_cast<I>(area_.size())) area_[static_cast<size_t>(i)] = static_cast<uint8_t>(v);
}

// The 26-byte record of voice n (§2.2): searched from the table's start, 26 bytes at a
// time, to the end of the 64 KB area; 25 zero bytes after the number when not found.
void Driver::record(I n, int rec[26]) const {
  for (int i = 0; i < 26; i++) rec[i] = 0;
  rec[0] = static_cast<int>(n & 0xFF);
  if (!hasVoices_) return;
  const I table = rw(24);
  for (I off = table; off + 1 < static_cast<I>(area_.size()); off += 26) {
    if (rd(off) == (n & 0xFF)) {
      for (int i = 1; i < 26; i++) rec[i] = rd(off + i);
      return;
    }
  }
}

// ---------------------------------------------------------------- chip and timers (§3)

void Driver::w(int reg, I value) {
  const int v = static_cast<int>(value & 0xFF);
  reg &= 0x1FF;
  if (reg == 0x07) reg7_ = v;
  if (reg == 0x10 && v != 0 && v < 0x80) rhythmKeys++;
  switch (reg) {
    case 0x24: ta_ = (ta_ & 3) | (v << 2); break;
    case 0x25: ta_ = (ta_ & ~I(3)) | (v & 3); break;
    case 0x26: tbReg_ = v; break;
    case 0x27: {
      const I old = ctrl_;
      if ((old ^ v) & 1) cA_ = (v & 1) ? (1024 - ta_) * STEP : 0;
      if ((old ^ v) & 2) cB_ = (v & 2) ? (256 - tbReg_) * STEP : 0;
      if (v & 0x10) flagA_ = false;
      if (v & 0x20) flagB_ = false;
      ctrl_ = v;
      break;
    }
  }
  if (sink_) sink_->reg(reg, v);
}

uint32_t Driver::nextEvent() const {
  const uint32_t tA = cA_ ? static_cast<uint32_t>(((cA_ + 0xFFFF) >> 16) - 1) : 0xFFFFFFFFu;
  const uint32_t tB = cB_ ? static_cast<uint32_t>(((cB_ + 0xFFF) >> 12) - 1) : 0xFFFFFFFFu;
  return std::min(tA, tB) + 1;   // 0 when both are stopped
}

void Driver::advance(uint32_t us) {
  if (cA_) {
    cA_ -= static_cast<I>(us) * 65536;
    if (cA_ <= 0) {
      const I period = (1024 - ta_) * STEP;
      cA_ += ((-cA_) / period + 1) * period;
      if (ctrl_ & 4) flagA_ = true;
    }
  }
  if (cB_) {
    cB_ -= static_cast<I>(us) * 4096;
    if (cB_ <= 0) {
      const I period = (256 - tbReg_) * STEP;
      cB_ += ((-cB_) / period + 1) * period;
      if (ctrl_ & 8) flagB_ = true;
    }
  }
}

void Driver::event() {
  if (flagA_) timerA();
  if (flagB_) timerB();
  w(0x27, fm3mode_ | 0x30);
}

// §5.4
void Driver::timerA() {
  taCount_++;
  if (taCount_ % 8 == 0 && fadeSpeed_) {
    if (fadeSpeed_ + fadeLevel_ < 256) {
      fadeLevel_ += fadeSpeed_;
    } else {
      fadeLevel_ = 255;
      fadeSpeed_ = 0;
      stopReq_ = true;
    }
  }
  if (effPri_) effectStep();
}

// §5.5, §5.6
void Driver::timerB() {
  if (stopReq_) {
    stopReq_ = false;
    stop();
  }
  if (!playing_) return;
  if (!opm_) {
    kind_ = SSG;
    for (int i = 0; i < 3; i++) {
      ctxCh_ = i + 1;
      ctxIdx_ = 6 + i;
      ssgTick(part_[6 + i]);
    }
  }
  kind_ = FM;
  for (int port = 1; port >= 0; port--) {
    for (int i = 0; i < 3; i++) {
      ctxP_ = port ? 0x100 : 0;
      ctxCh_ = i + 1;
      ctxIdx_ = port * 3 + i;
      fmTick(part_[ctxIdx_]);
    }
  }
  for (int i = 0; i < 3; i++) {
    ctxP_ = 0;
    ctxCh_ = 3;
    ctxIdx_ = EXT + i;
    fmTick(part_[ctxIdx_]);
  }
  if (!opm_) {
    kind_ = RHY;
    ctxIdx_ = 10;
    rhyTick(part_[10]);
    kind_ = ADPCM;
    ctxIdx_ = 9;
    ctxP_ = 0;
    ctxCh_ = 3;
    adTick(part_[9]);
  }
  kind_ = PPZ;
  for (int i = 0; i < 8; i++) {
    ctxP_ = 0;
    ctxCh_ = i;
    ctxIdx_ = PPZP + i;
    adTick(part_[ctxIdx_]);
  }
  loopCheck();
  if (tb_ != lastTB_) {
    w(0x26, tb_);
    lastTB_ = tb_;
  }
  if (barCount_ + 1 == barLen_) {
    bars_++;
    barCount_ = 0;
  } else {
    barCount_++;
  }
  lastTA_ = taCount_;
}

// §12.1
void Driver::loopCheck() {
  I r = 3;
  for (auto& p : part_)
    if (p.active) r &= p.loopflag;
  if (r == 0) return;
  for (auto& p : part_)
    if (p.active && p.loopflag != 3) p.loopflag = 0;
  if (r == 3) {
    loopCounter_ = -1;
  } else {
    loopCounter_++;
    if (loopCounter_ == 255) loopCounter_ = 1;
  }
}

// ---------------------------------------------------------------- set-up (§5)

void Driver::init() {
  w(0x29, 0);
  w(0x24, 0);
  w(0x25, 0);
  w(0x26, 0);
  w(0x27, 0x3F);
  partReset();
  chipReset();
  w(0x07, 0xBF);
  stop();
  timerSetup();
  w(0x29, 0x83);
}

void Driver::stop() {
  playing_ = false;
  fadeSpeed_ = 0;
  loopCounter_ = -1;
  fadeLevel_ = 0xFF;
  silence();
}

void Driver::silence() {
  static const int regs[] = {0x80, 0x81, 0x82, 0x84, 0x85, 0x86, 0x88, 0x89, 0x8A, 0x8C, 0x8D, 0x8E,
                             0x180, 0x181, 0x184, 0x185, 0x188, 0x189, 0x18C, 0x18D,
                             0x182, 0x186, 0x18A, 0x18E};
  for (int r : regs) w(r, 0xFF);
  for (int k : {0, 1, 2, 4, 5, 6}) w(0x28, k);
  ev("PPS stop");
  ev("P86 stop");
  if (effPri_ == 0) w(0x07, 0xBF);
  else w(0x07, (reg7_ & 0x3F) | 0x9B);
  w(0x10, 0xFF);
  w(0x101, 0x02);
  w(0x100, 0x01);
  w(0x110, 0x80);
  w(0x110, 0x18);
  for (int c = 0; c < 8; c++) ev("PPZ stop " + num(c));
}

void Driver::partReset() {
  fadeLevel_ = fadeSpeed_ = 0;
  for (auto& p : part_) {
    const I keep = p.mask & 0x0F;
    p = Part();
    p.mask = keep;
    p.note = p.prev = REST;
  }
  tie_ = 0;
  loopCounter_ = 0;
  bars_ = barCount_ = 0;
  taCount_ = lastTA_ = 0;
  for (auto& k : keyon_) k[0] = k[1] = k[2] = 0;
  fm3fbalg_ = 0;
  adStart_ = adStop_ = 0;
  adRep1_ = adRep2_ = 0;
  adRel_ = 0x8000;
  drumRecord_ = 0;
  for (auto& s : slotdt_) s = 0;
  slotdtOn_ = false;
  fm3special_ = 0;
  fm3mode_ = 0x3F;
  barLen_ = 96;
  vdFM_ = vdSSG_ = vdAD_ = vdRHY_ = vdPPZ_ = 0;
}

void Driver::chipReset() {
  w(0x29, 0x83);
  noiseMem_ = 0;
  w(0x06, 0);
  noiseLast_ = 0;
  for (int r : {0xB4, 0xB5, 0xB6, 0x1B4, 0x1B5, 0x1B6}) w(r, 0xC0);
  hwlfo_ = 0;
  w(0x22, 0);
  for (auto& r : rhy_) r = 0xCF;
  w(0x10, 0xFF);
  rhyTotal_ = 48 * 4 * (256 - vdRHY_) / 1024;
  w(0x11, rhyTotal_);
  w(0x10C, 0xFF);
  w(0x10D, 0xFF);
  for (int c = 0; c < 8; c++) ev("PPZ pan " + num(c) + " 5");
}

void Driver::timerSetup() {
  tb_ = tbP_ = 200;
  t_ = tP_ = tbToT(tb_);
  if (tb_ != lastTB_) {
    w(0x26, tb_);
    lastTB_ = tb_;
  }
  w(0x25, 0);
  w(0x24, 0);
  w(0x27, 0x3F);
  bars_ = barCount_ = 0;
  barLen_ = 96;
}

std::string Driver::load(const uint8_t* file, size_t size) {
  // §2.1: what the old player refused
  if (size < 3) return "not a PMD song (too short)";
  if (size > 0x10000) return "not a PMD song (larger than 64 KB)";
  if (file[0] > 0x0F || (file[1] != 0x18 && file[1] != 0x1A) || file[2] != 0)
    return "not a PMD song";
  stop();
  std::fill(area_.begin(), area_.end(), 0x80);
  std::memcpy(area_.data(), file, size);
  hasVoices_ = rw(0) != 0x18;
  stop();
  // the search for the song's PCM files: none of them is there, but the first missing
  // one makes a call
  if (!memo(0).empty()) ev("P86 stop");
  else if (!memo(-1).empty()) ev("PPS stop");
  return "";
}

void Driver::start() {
  tb_ = 0;
  if (tb_ != lastTB_) {
    w(0x26, 0);
    lastTB_ = 0;
  }
  w(0x27, 0);
  stop();
  partReset();
  partSetup();
  chipReset();
  timerSetup();
  playing_ = true;
}

void Driver::partSetup() {
  opm_ = area_[0];
  for (int i = 0; i < 11; i++) {
    Part& p = part_[i];
    p.pos = rw(2 * i);
    p.active = rd(p.pos) != 0x80;
    p.len = 1;
    p.keyoff = -1;
    p.lfo[0].dcCount = p.lfo[1].dcCount = p.lfo[0].dcCountR = p.lfo[1].dcCountR = -1;
    p.note = p.prev = REST;
    if (i < 6) {
      p.vol = 108;
      p.pan = 0xC0;
      p.slotmask = 0xF0;
      p.vwmask = 0xFF;
    } else if (i < 9) {
      p.vol = 8;
      p.mixer = 7;
      p.estate = 3;
    } else if (i == 9) {
      p.vol = 128;
      p.pan = 0xC0;
    } else {
      p.vol = 15;
      p.patPos = -1;
    }
  }
  rtable_ = rw(22);
}

// ---------------------------------------------------------------- memos (§2.3)

std::string Driver::memo(int n) const {
  if (rw(0) != 0x1A) return "";
  const I vo = rw(24);
  const I m = rw(vo - 4);
  const int v = rd(vo - 2), marker = rd(vo - 1);
  if (!(v == 0x40 || (marker == 0xFE && v >= 0x41))) return "";
  const int index = n + (v >= 0x42) + (v >= 0x48);
  if (index < 0) return "";
  I at = 0;
  for (int j = 0; j <= index; j++) {
    at = rw(m + 2 * j);
    if (at == 0 || rd(at) == '/') return "";
  }
  std::string s;
  for (I d = at; d + 1 < static_cast<I>(area_.size()) && area_[static_cast<size_t>(d + 1)]; d++)
    s += static_cast<char>(area_[static_cast<size_t>(d + 1)]);
  return s;
}

std::string memoText(const std::string& raw) {
  static const uint16_t KANA[] = {
      0xa9fe, 0xa1a3, 0xa1d6, 0xa1d7, 0xa1a2, 0xa1a6, 0xa5f2, 0xa5a1, 0xa5a3, 0xa5a5,  // 9E-A7
      0xa5a7, 0xa5a9, 0xa5e3, 0xa5e5, 0xa5e7, 0xa5c3, 0xa1bc, 0xa5a2, 0xa5a4, 0xa5a6,  // A8-B1
      0xa5a8, 0xa5aa, 0xa5ab, 0xa5ad, 0xa5af, 0xa5b1, 0xa5b3, 0xa5b5, 0xa5b7, 0xa5b9,  // B2-BB
      0xa5bb, 0xa5bd, 0xa5bf, 0xa5c1, 0xa5c4, 0xa5c6, 0xa5c8, 0xa5ca, 0xa5cb, 0xa5cc,  // BC-C5
      0xa5cd, 0xa5ce, 0xa5cf, 0xa5d2, 0xa5d5, 0xa5d8, 0xa5db, 0xa5de, 0xa5df, 0xa5e0,  // C6-CF
      0xa5e1, 0xa5e2, 0xa5e4, 0xa5e6, 0xa5e8, 0xa5e9, 0xa5ea, 0xa5eb, 0xa5ec, 0xa5ed,  // D0-D9
      0xa5ef, 0xa5f3, 0xa1ab, 0xa1ac, 0xaae0, 0xaae1, 0xa5ef, 0xa5ab, 0xa5b1, 0xa5f4,  // DA-E3
      0xa5ac, 0xa5ae, 0xa5b0, 0xa5b2, 0xa5b4, 0xa5b6, 0xa5b8, 0xa5ba, 0xa5bc, 0xa5be,  // E4-ED
      0xa5c0, 0xa5c2, 0xa5c5, 0xa5c7, 0xa5c9, 0xa5d0, 0xa5d1, 0xa5d3, 0xa5d4, 0xa5d6,  // EE-F7
      0xa5d7, 0xa5d9, 0xa5da, 0xa5dc, 0xa5dd};                                          // F8-FC
  std::string a;
  for (size_t i = 0; i < raw.size(); i++) {
    const auto c = static_cast<unsigned char>(raw[i]);
    const bool two = (c >= 0x80 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
    if (!two || i + 1 >= raw.size()) {
      a += static_cast<char>(c);
      continue;
    }
    const auto x = static_cast<unsigned char>(raw[i + 1]);
    i++;
    if (c == 0x85 && x >= 0x40 && x <= 0xFC) {
      if (x <= 0x7E) a += static_cast<char>(x - 0x1F);
      else if (x == 0x7F) continue;
      else if (x <= 0x9D) a += static_cast<char>(x - 0x20);
      else {
        const uint16_t k = KANA[x - 0x9E];
        a += static_cast<char>(k >> 8);
        a += static_cast<char>(k & 0xFF);
      }
    } else {
      a += static_cast<char>(c);
      a += static_cast<char>(x);
    }
  }
  std::string b;
  for (size_t i = 0; i < a.size(); i++) {
    if (a[i] != 0x1B) {
      b += a[i];
      continue;
    }
    if (i + 1 >= a.size()) break;
    const char k = a[i + 1];
    if (k == '[') {
      size_t j = i + 2;
      while (j < a.size()) {
        const auto c = static_cast<unsigned char>(a[j]);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) break;
        j += ((c >= 0x80 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) ? 2 : 1;
      }
      i = j;
    } else if (k == '=') {
      i += 3;
    } else if (k == ')' || k == '!') {
      i += 2;
    } else {
      i += 1;
    }
  }
  return b;
}

// ---------------------------------------------------------------- shared note rules

// §7.1
I Driver::transpose(const Part& p, I n) const {
  if (n == 0x0F) return n;
  if (n == 0x0C) return p.note == REST ? 0x0F : p.note;
  const I d = p.transpose + p.mtranspose;
  if (d == 0) return n;
  I o = n >> 4, s = n & 0x0F;
  if (d < 0) {
    s += d;
    if (s < 0) {
      do {
        o--;
        s += 12;
      } while (s < 0);
    }
    if (o < 0) o = 0;
  } else {
    s += d;
    while (s >= 12) {
      o++;
      s -= 12;
    }
    if (o > 7) o = 7;
  }
  return (o << 4) | s;
}

// §7.9: pos is just after the length byte
void Driver::gate(Part& p, I& pos) {
  if (rd(pos) == 0xC1) {
    pos++;
    p.qcount = 0;
    return;
  }
  I g = p.qbase;
  if (p.q2) g += (p.len * p.q2) >> 8;
  if (p.randq) {
    const I x = static_cast<I>(rng_.next()) % ((p.randq & 0x7F) + 1);
    if (p.randq < 0x80) g += x;
    else g = std::max<I>(g - x, 0);
  }
  if (p.qmin) p.qcount = p.len - p.qmin < 0 ? 0 : std::min(g, p.len - p.qmin);
  else p.qcount = g;
}

// §7.11 step 3
void Driver::oneShotRule(Part& p) {
  if (p.oneshot && p.note != REST) {
    oneShotFlag_--;
    if (oneShotFlag_) {
      p.oneshot = 0;
      oneShotFlag_ = 0;
    }
  }
}

// ---------------------------------------------------------------- LFOs (§7.10)

void Driver::lfoInit(Part& p, int set) {
  Lfo& l = p.lfo[set];
  l.value = 0;
  l.delay = l.delayR;
  l.speed = l.speedR;
  l.step = l.stepR;
  l.time = l.timeR;
  l.dcCount = l.dcCountR;
  if (l.wave == 2 || l.wave == 3) l.speed = 1;
  else l.speed++;
}

bool Driver::lfoStep(Part& p, int set) {
  Lfo& l = p.lfo[set];
  const I before = l.value;
  if (l.delay) {
    l.delay--;
    return false;
  }
  if (p.extend & (set ? 0x20 : 0x02)) {
    const I n = taCount_ - lastTA_;
    if (n == 0) return false;
    for (I k = 0; k < n; k++) waveStep(l);
  } else {
    waveStep(l);
  }
  return l.value != before;
}

void Driver::waveStep(Lfo& l) {
  if (l.speed != 1) {
    if (l.speed != 255) l.speed--;
    return;
  }
  l.speed = l.speedR;
  switch (l.wave) {
    case 0:
    case 4:
    case 5: {
      if (l.wave == 5) l.value += (l.step < 0 ? -l.step : l.step) * l.step;
      else l.value += l.step;
      if (l.value == 0) depthChange(l);
      I k = l.time;
      if (k != 255) {
        k--;
        if (k == 0) {
          l.time = l.timeR;
          if (l.wave != 4) l.time *= 2;
          l.step = -l.step;
          return;
        }
      }
      l.time = k;
      return;
    }
    case 1: {
      l.value += l.step;
      I k = l.time;
      if (k != -1) {
        k--;
        if (k == 0) {
          l.value = -l.value;
          depthChange(l);
          k = l.timeR * 2;
        }
      }
      l.time = k;
      return;
    }
    case 2:
      l.value = l.step * l.time;
      depthChange(l);
      l.step = -l.step;
      return;
    case 6:
      if (l.time != 0) {
        if (l.time != 255) l.time--;
        l.value += l.step;
      }
      return;
    default: {  // 3 and anything else: random
      const I a = (l.step < 0 ? -l.step : l.step) * l.time;
      const I r = static_cast<I>(rng_.next());
      l.value = a - (a ? r % (2 * a) : 0);
      depthChange(l);
      return;
    }
  }
}

void Driver::depthChange(Lfo& l) {
  if (--l.dcSpeed != 0) return;
  l.dcSpeed = l.dcSpeedR;
  if (l.dcCount == 0) return;
  if (l.dcCount <= 127) l.dcCount--;
  if (l.step < 0) {
    const I x = l.dcDepth - l.step;
    l.step = x >= 0 ? -x : (l.dcDepth < 0 ? 0 : -127);
  } else {
    const I x = l.step + l.dcDepth;
    l.step = x >= 0 ? x : (l.dcDepth < 0 ? 0 : 127);
  }
}

void Driver::lfoStart(Part& p, bool restOrTied, bool fm) {
  if (!restOrTied) {
    if (fm) {
      p.hlfoCount = p.hlfoDelay;
      if (p.hlfoDelay) w(ctxP_ + 0xB4 + ctxCh_ - 1, p.pan & 0xC0);
      p.sdelayCount = p.sdelay;
    }
    if (p.lfosw & 3) {
      if (!(p.lfosw & 4)) lfoInit(p, 0);
      lfoStep(p, 0);
    }
    if (p.lfosw & 0x30) {
      if (!(p.lfosw & 0x40)) lfoInit(p, 1);
      lfoStep(p, 1);
    }
  } else {
    if (p.lfosw & 3) lfoStep(p, 0);
    if (p.lfosw & 0x30) lfoStep(p, 1);
  }
}

// §7.8
void Driver::portaStep(Part& p) {
  p.portaAcc += p.portaStep;
  if (p.portaRem > 0) {
    p.portaRem--;
    p.portaAcc++;
  } else if (p.portaRem < 0) {
    p.portaRem++;
    p.portaAcc--;
  }
}

// ---------------------------------------------------------------- reading

// §6.6: the end byte. Returns true when reading goes on (at the loop point).
bool Driver::endOfPart(Part& p, ReadResult& r) {
  p.loopflag = 3;
  p.note = REST;
  if (p.loopPos) {
    p.pos = p.loopPos;
    p.loopflag = 1;
    return true;
  }
  if (p.mask) {
    tie_ = oneShotFlag_ = 0;
    r = STOP;
  } else {
    r = CONT;
  }
  return false;
}

void Driver::fmTick(Part& p) {
  if (!p.active) return;
  p.len--;
  if (p.mask) {
    p.keyoff = -1;
  } else if (p.keyoff == 0 && p.len <= p.qcount) {
    fmKeyOff(p);
    p.keyoff = -1;
  }
  if (p.len == 0) {
    if (!p.mask) p.lfosw &= ~I(8);
    const ReadResult r = fmRead(p);
    if (r != CONT) return;
  }
  if (!p.mask) fmIdle(p);
}

Driver::ReadResult Driver::fmRead(Part& p) {
  for (long guard = 0;; guard++) {
    if (guard > READ_LIMIT) {   // a song that only loops without notes
      p.loopflag = 3;
      p.note = REST;
      return CONT;
    }
    const int b = rd(p.pos);
    if (b == 0x80) {
      ReadResult r;
      if (endOfPart(p, r)) continue;
      return r;
    }
    if (b > 0x80 && b != 0xDA) {
      command(p, p.pos);
      continue;
    }
    if (p.mask) {   // a masked part takes a rest
      p.pitch = 0;
      p.note = p.prev = REST;
      p.pos += b == 0xDA ? 3 : 1;
      p.len = rd(p.pos++);
      if (--oneShotFlag_) p.oneshot = 0;
      tie_ = oneShotFlag_ = 0;
      return NOTE;
    }
    const bool porta = b == 0xDA;
    if (porta) p.pos++;
    const I n = rd(p.pos);
    const I nn = (n & 0x0F) == 0x0C ? p.prev : n;
    p.prev = nn;
    const bool rest = (nn & 0x0F) == 0x0F;
    lfoStart(p, rest || tie_, true);
    if (!rest) p.portaAcc = 0;
    fmPitchValue(p, transpose(p, n));
    p.pos++;
    if (porta) {
      const I p1 = p.pitch, cn = p.note;
      fmPitchValue(p, transpose(p, rd(p.pos++)));
      const I p2 = p.pitch;
      p.note = cn;
      p.pitch = p1;
      const I oct = (((p2 >> 8) & 0x38) - ((p1 >> 8) & 0x38)) / 8;
      const I delta = oct * 0x26A + ((p2 & 0x7FF) - (p1 & 0x7FF));
      p.len = rd(p.pos++);
      gate(p, p.pos);
      p.portaStep = p.len ? delta / p.len : 0;
      p.portaRem = p.len ? delta % p.len : 0;
      p.lfosw |= 8;
    } else {
      p.len = rd(p.pos++);
      gate(p, p.pos);
    }
    oneShotRule(p);
    fmVolOut(p);
    fmPitchOut(p);
    fmKeyOn(p);
    tie_ = oneShotFlag_ = 0;
    p.keyoff = rd(p.pos) == 0xFB ? 2 : 0;
    return NOTE;
  }
}

// §7.12
void Driver::fmIdle(Part& p) {
  if (p.hlfoCount && --p.hlfoCount == 0) w(ctxP_ + 0xB4 + ctxCh_ - 1, p.pan);
  if (p.sdelayCount && --p.sdelayCount == 0 && p.keyoff != -1) fmKeyOn(p);
  if (p.lfosw) {
    I ch = p.lfosw & 8;
    if ((p.lfosw & 3) && lfoStep(p, 0)) ch |= p.lfosw & 3;
    if ((p.lfosw & 0x30) && lfoStep(p, 1)) ch |= p.lfosw & 0x30;
    if (ch & 0x19) {
      if (p.lfosw & 8) portaStep(p);
      fmPitchOut(p);
    }
    if (ch & 0x22) {
      fmVolOut(p);
      return;
    }
  }
  if (fadeSpeed_) fmVolOut(p);
}

void Driver::ssgTick(Part& p) {
  if (!p.active) return;
  p.len--;
  if (p.mask) {
    p.keyoff = -1;
  } else if (p.keyoff == 0 && p.len <= p.qcount) {
    envKeyOff(p);
    p.keyoff = -1;
  }
  if (p.len == 0) {
    p.lfosw &= ~I(8);
    const ReadResult r = ssgRead(p);
    if (r != CONT) return;
  }
  pcmIdle(p);
}

Driver::ReadResult Driver::ssgRead(Part& p) {
  for (long guard = 0;; guard++) {
    if (guard > READ_LIMIT) {
      p.loopflag = 3;
      p.note = REST;
      return CONT;
    }
    const int b = rd(p.pos);
    if (b == 0xDA && drumCheck(p, b) == 1) {   // the old player's quirk (§8.1)
      p.pos++;
      continue;
    }
    if (b == 0x80) {
      ReadResult r;
      if (endOfPart(p, r)) continue;
      return r;
    }
    if (b > 0x80 && b != 0xDA) {
      command(p, p.pos);
      continue;
    }
    if (p.mask && (b == 0xDA || drumCheck(p, b) == 0)) {
      p.pitch = 0;
      p.note = REST;
      p.pos += b == 0xDA ? 3 : 1;
      p.len = rd(p.pos++);
      if (--oneShotFlag_) p.oneshot = 0;
      tie_ = oneShotFlag_ = 0;
      return CONT;   // a masked rest: the part still steps its LFOs and envelope
    }
    const bool porta = b == 0xDA;
    if (porta) p.pos++;
    const I n = rd(p.pos);
    const I nn = (n & 0x0F) == 0x0C ? p.prev : n;
    p.prev = nn;
    const bool rest = (nn & 0x0F) == 0x0F;
    if (!(rest || tie_)) envStart(p);
    lfoStart(p, rest || tie_, false);
    if (!rest) p.portaAcc = 0;
    ssgPitchValue(p, transpose(p, n));
    p.pos++;
    if (porta) {
      const I p1 = p.pitch, cn = p.note;
      ssgPitchValue(p, transpose(p, rd(p.pos++)));
      const I delta = p.pitch - p1;
      p.note = cn;
      p.pitch = p1;
      p.len = rd(p.pos++);
      gate(p, p.pos);
      p.portaStep = p.len ? delta / p.len : 0;
      p.portaRem = p.len ? delta % p.len : 0;
      p.lfosw |= 8;
    } else {
      p.len = rd(p.pos++);
      gate(p, p.pos);
    }
    oneShotRule(p);
    ssgVolOut(p);
    ssgPitchOut(p);
    ssgKeyOn(p);
    tie_ = oneShotFlag_ = 0;
    p.keyoff = rd(p.pos) == 0xFB ? 2 : 0;
    return NOTE;
  }
}

// ADPCM part J and the PPZ8 parts (§9, §10)
void Driver::adTick(Part& p) {
  if (!p.active) return;
  p.len--;
  if (p.mask) {
    p.keyoff = -1;
  } else if (p.keyoff == 0 && p.len <= p.qcount) {
    pcmKeyOff(p);
    p.keyoff = -1;
  }
  if (p.len == 0) {
    p.lfosw &= ~I(8);
    const ReadResult r = pcmRead(p);
    if (r != CONT) return;
  }
  pcmIdle(p);
}

Driver::ReadResult Driver::pcmRead(Part& p) {
  for (long guard = 0;; guard++) {
    if (guard > READ_LIMIT) {
      p.loopflag = 3;
      p.note = REST;
      return CONT;
    }
    const int b = rd(p.pos);
    if (b == 0x80) {
      ReadResult r;
      if (endOfPart(p, r)) continue;
      return r;
    }
    if (b > 0x80 && b != 0xDA) {
      command(p, p.pos);
      continue;
    }
    if (p.mask) {
      p.pitch = 0;
      p.note = REST;
      p.pos += b == 0xDA ? 3 : 1;
      p.len = rd(p.pos++);
      if (--oneShotFlag_) p.oneshot = 0;
      tie_ = oneShotFlag_ = 0;
      return CONT;   // as SSG: the part still steps its LFOs and envelope
    }
    const bool porta = b == 0xDA;
    if (porta) p.pos++;
    const I n = rd(p.pos);
    const I nn = (n & 0x0F) == 0x0C ? p.prev : n;
    p.prev = nn;
    const bool rest = (nn & 0x0F) == 0x0F;
    if (!(rest || tie_)) envStart(p);
    lfoStart(p, rest || tie_, false);
    if (!rest) p.portaAcc = 0;
    adPitchValue(p, transpose(p, n));
    p.pos++;
    if (porta) {
      const I p1 = p.pitch, cn = p.note;
      adPitchValue(p, transpose(p, rd(p.pos++)));
      I delta = p.pitch - p1;
      if (kind_ == PPZ) delta /= 16;
      p.note = cn;
      p.pitch = p1;
      p.len = rd(p.pos++);
      gate(p, p.pos);
      p.portaStep = p.len ? delta / p.len : 0;
      p.portaRem = p.len ? delta % p.len : 0;
      p.lfosw |= 8;
    } else {
      p.len = rd(p.pos++);
      gate(p, p.pos);
    }
    oneShotRule(p);
    pcmVolOut(p);
    pcmPitchOut(p);
    if (p.keyoff == -1) pcmKeyOn(p);
    tie_ = oneShotFlag_ = 0;
    p.keyoff = rd(p.pos) == 0xFB ? 2 : 0;
    return NOTE;
  }
}

// §8.3: SSG, ADPCM and PPZ parts without a new note (masked or not)
void Driver::pcmIdle(Part& p) {
  I ch = p.lfosw & 8;
  if ((p.lfosw & 3) && lfoStep(p, 0)) ch |= p.lfosw & 3;
  if ((p.lfosw & 0x30) && lfoStep(p, 1)) ch |= p.lfosw & 0x30;
  if (ch & 0x19) {
    if (p.lfosw & 8) portaStep(p);
    if (kind_ == SSG) ssgPitchOut(p);
    else pcmPitchOut(p);
  }
  const bool env = envStep(p);
  if (env || (ch & 0x22) || fadeSpeed_) {
    if (kind_ == SSG) ssgVolOut(p);
    else pcmVolOut(p);
  }
}

// §8.2
void Driver::rhyTick(Part& p) {
  if (!p.active) return;
  if (--p.len != 0) return;
  for (long guard = 0;; guard++) {
    if (guard > READ_LIMIT) {
      p.loopflag = 3;
      p.patPos = -1;
      return;
    }
    const int b = p.patPos < 0 ? 0xFF : rd(p.patPos);
    if (b == 0xFF) {   // the pattern ends: the rhythm part's own sequence
      bool jumped = false;
      for (long g2 = 0; g2 <= READ_LIMIT && !jumped; g2++) {
        const int c = rd(p.pos);
        if (c == 0x80) {
          p.loopflag = 3;
          if (p.loopPos) {
            p.pos = p.loopPos;
            p.loopflag = 1;
            continue;
          }
          p.patPos = -1;
          tie_ = oneShotFlag_ = 0;
          return;
        }
        if (c > 0x80) {
          command(p, p.pos);
          continue;
        }
        p.patPos = rw(rtable_ + 2 * c);
        p.pos++;
        jumped = true;
      }
      if (!jumped) {
        p.loopflag = 3;
        p.patPos = -1;
        return;
      }
      continue;
    }
    if (b >= 0xC0) {
      command(p, p.patPos);
      continue;
    }
    if (b >= 0x80) {
      const I mask = ((b << 8) | rd(p.patPos + 1)) & 0x3FFF;
      p.len = rd(p.patPos + 2);
      p.patPos += 3;
      if (p.mask) {
        drumRecord_ = 0;
      } else {
        drumRecord_ = mask;
        if (mask && fadeLevel_ == 0) {
          int e = 0;
          while (!(mask & (I(1) << e))) e++;
          effectStart(e);
        }
      }
    } else {
      p.len = rd(p.patPos + 1);
      p.patPos += 2;
      drumRecord_ = 0;
    }
    tie_ = oneShotFlag_ = 0;
    return;
  }
}

// ---------------------------------------------------------------- commands (§6)

void Driver::command(Part& p, I& pos) {
  const int k = kind_;
  const bool fm = k == FM, ssg = k == SSG, pcm = k == ADPCM || k == PPZ, rhy = k == RHY;
  const int cmd = rd(pos);
  auto b1 = [&] { return rd(pos + 1); };
  auto s1 = [&] { return rs(pos + 1); };
  auto ppzch = [&] { return num(ctxCh_); };
  switch (cmd) {
    case 0xFF: {
      const int v = b1();
      pos += 2;
      if (fm) {
        fmVoice(p, v);
      } else if (k == ADPCM) {
        p.voice = v;
        adStart_ = adStop_ = 0;
        adRep1_ = adRep2_ = 0;
        adRel_ = 0x8000;
      } else if (k == PPZ) {
        p.voice = v;
        ev("PPZ loop " + ppzch() + " 0 0");
        ev("PPZ srate " + ppzch() + " 0");
      }
      return;
    }
    case 0xFE:
      if (!rhy) {
        p.qbase = b1();
        if (fm || ssg) p.randq = 0;
      }
      pos += 2;
      return;
    case 0xFD:
      p.vol = b1();
      pos += 2;
      return;
    case 0xFC:
      tempo(pos);
      return;
    case 0xFB:
      tie_ = 1;
      pos += 1;
      return;
    case 0xFA:
      p.detune = rsw(pos + 1);
      pos += 3;
      return;
    case 0xF9:
      wd(rw(pos + 1) + 1, 0);
      pos += 3;
      return;
    case 0xF8: {
      const int c = rd(pos + 1);
      const I target = rw(pos + 3);
      if (c != 0) {
        const int n = (rd(pos + 2) + 1) & 0xFF;
        wd(pos + 2, n);
        if (c == n) {
          pos += 5;
          return;
        }
      } else {
        p.loopflag = 1;
      }
      pos = target + 2;
      return;
    }
    case 0xF7: {
      const I target = rw(pos + 1);
      const int c = rd(target), n = rd(target + 1);
      if (c - 1 == n) pos = target + 4;
      else pos += 3;
      return;
    }
    case 0xF6:
      pos += 1;
      p.loopPos = pos;
      return;
    case 0xF5:
      if (!rhy) p.transpose = s1();
      pos += 2;
      return;
    case 0xF4:
      if (fm) p.vol = std::min<I>(p.vol + 4, 127);
      else if (pcm) p.vol += 16;
      else if (p.vol < 15) p.vol++;
      pos += 1;
      return;
    case 0xF3:
      if (fm) p.vol = std::max<I>(p.vol - 4, 0);
      else if (pcm) p.vol = std::max<I>(p.vol - 16, 0);
      else if (p.vol > 0) p.vol--;
      pos += 1;
      return;
    case 0xF2:
    case 0xBF:
      if (!rhy) {
        Lfo& l = p.lfo[cmd == 0xF2 ? 0 : 1];
        l.delay = l.delayR = rd(pos + 1);
        l.speed = l.speedR = rd(pos + 2);
        l.step = l.stepR = rs(pos + 3);
        l.time = l.timeR = rd(pos + 4);
        lfoInit(p, cmd == 0xF2 ? 0 : 1);
      }
      pos += 5;
      return;
    case 0xF1: {
      if (rhy) {
        pos += 2;
        return;
      }
      I v = b1();
      pos += 2;
      if (v & 0xF8) v = 1;
      p.lfosw = (p.lfosw & ~I(7)) | (v & 7);
      lfoInit(p, 0);
      if (fm && fm3ch()) fm3Check(p);
      return;
    }
    case 0xBE: {
      if (rhy) {
        pos += 2;
        return;
      }
      const I v = b1();
      pos += 2;
      p.lfosw = (p.lfosw & ~I(0x70)) | ((v & 7) << 4);
      lfoInit(p, 1);
      if (fm && fm3ch()) fm3Check(p);
      return;
    }
    case 0xF0:
      if (ssg || pcm) {
        p.ep[1] = p.ep[0] = rd(pos + 1);
        p.ep[2] = rs(pos + 2);
        p.ep[4] = p.ep[3] = rd(pos + 3);
        p.ep[6] = p.ep[5] = rd(pos + 4);
        if (p.eext) {
          p.eext = false;
          p.estate = 2;
          p.elevel = -15;
        }
      }
      pos += 5;
      return;
    case 0xEF: {
      const int r = b1(), v = rd(pos + 2);
      pos += 3;
      if (fm) w(ctxP_ + r, v);
      else if (k == ADPCM) w(0x100 + r, v);
      else w(r, v);
      return;
    }
    case 0xEE:
      if (ssg) noiseMem_ = b1();
      pos += 2;
      return;
    case 0xED:
      if (ssg) p.mixer = b1();
      pos += 2;
      return;
    case 0xEC: {
      const int v = b1();
      pos += 2;
      if (fm) {
        p.pan = (p.pan & 0x3F) | ((v << 6) & 0xC0);
        fmPanCopy(p);
        if (!p.mask) fmPanOut(p);
      } else if (k == ADPCM) {
        p.pan = (v << 6) & 0xC0;
      } else if (k == PPZ) {
        static const int PAN[4] = {0, 9, 1, 5};
        ev("PPZ pan " + ppzch() + " " + num(PAN[v & 3]));
      }
      return;
    }
    case 0xC3: {
      const int s = s1();
      pos += 3;
      if (fm) {
        const int v = s > 0 ? 2 : s == 0 ? 3 : 1;
        p.pan = (p.pan & 0x3F) | ((v << 6) & 0xC0);
        fmPanCopy(p);
        if (!p.mask) fmPanOut(p);
      } else if (k == ADPCM) {
        const int u = s & 0xFF;
        p.pan = u == 0 ? 0xC0 : u < 128 ? 0x80 : 0x40;
      } else if (k == PPZ) {
        ev("PPZ pan " + ppzch() + " " + num(std::clamp(s, -4, 4) + 5));
      }
      return;
    }
    case 0xEB:
    case 0xEA:
    case 0xE9:
    case 0xE8:
    case 0xE6:
    case 0xE5:
      rhythmCommand(cmd, pos);
      return;
    case 0xE7:
      if (!rhy) p.transpose += s1();
      pos += 2;
      return;
    case 0xE4:
      if (fm) p.hlfoDelay = b1();
      pos += 2;
      return;
    case 0xE3: {
      const I v = b1();
      pos += 2;
      if (fm) p.vol = std::min<I>(p.vol + v, 127);
      else if (pcm) p.vol += v;
      else if (p.vol + v < 16) p.vol += v;
      return;
    }
    case 0xE2: {
      const I v = b1();
      pos += 2;
      if (fm || pcm) p.vol = std::max<I>(p.vol - v, 0);
      else if (p.vol - v >= 0) p.vol -= v;
      return;
    }
    case 0xE1: {
      const int v = b1();
      pos += 2;
      if (fm) {
        p.pan = (p.pan & 0xC0) | v;
        fmPanCopy(p);
        if (!p.mask) fmPanOut(p);
      }
      return;
    }
    case 0xE0:
      if (fm) {
        hwlfo_ = b1();
        w(0x22, hwlfo_);
      }
      pos += 2;
      return;
    case 0xDF:
      barLen_ = b1();
      pos += 2;
      return;
    case 0xDE: {
      const I v = b1();
      pos += 2;
      if (fm) p.oneshot = std::min<I>(p.vol + 1 + v, 128);
      else if (pcm) p.oneshot = std::min<I>(v + p.vol, 254) + 1;
      else p.oneshot = std::min<I>(p.vol + v, 15) + 1;
      oneShotFlag_ = 1;
      return;
    }
    case 0xDD: {
      const I v = b1();
      pos += 2;
      p.oneshot = std::clamp<I>(p.vol - v, 0, 254) + 1;
      oneShotFlag_ = 1;
      return;
    }
    case 0xDC:
    case 0xDB:
    case 0xD9:
    case 0xD8:
    case 0xD7:
    case 0xD3:
    case 0xD1:
      pos += 2;
      return;
    case 0xDA:   // only reached in the rhythm part
      pos += 2;
      return;
    case 0xD6:
    case 0xBD:
      if (!rhy) {
        Lfo& l = p.lfo[cmd == 0xD6 ? 0 : 1];
        l.dcSpeed = l.dcSpeedR = rd(pos + 1);
        l.dcDepth = rs(pos + 2);
      }
      pos += 3;
      return;
    case 0xD5:
      p.detune += rsw(pos + 1);
      pos += 3;
      return;
    case 0xD4: {
      const int v = b1();
      pos += 2;
      if (!p.mask) {
        if (v) effectStart(v);
        else effectEnd();
      }
      return;
    }
    case 0xD2:
      fadeSpeed_ = b1();
      pos += 2;
      return;
    case 0xD0:
      if (ssg) noiseMem_ = std::clamp<I>(noiseMem_ + s1(), 0, 31);
      pos += 2;
      return;
    case 0xCF: {
      const I v = b1();
      pos += 2;
      if (!fm) return;
      if (v & 0x0F) {
        p.carrier = (v & 0x0F) << 4;
      } else if (fm3ch()) {
        p.carrier = CARRIER[fm3fbalg_ & 7];
      } else {
        int rec[26];
        record(p.voice, rec);
        p.carrier = CARRIER[rec[25] & 7];
      }
      if ((v & 0xF0) != p.slotmask) {
        p.slotmask = v & 0xF0;
        if (p.slotmask == 0) p.mask |= 0x20;
        else p.mask &= ~I(0x20);
        if (fm3ch()) {
          fm3Check(p);
          if (ctxIdx_ != PART_C) {
            for (int idx : {PART_C, EXT, EXT + 1}) {
              if (idx == ctxIdx_) break;
              Part& o = part_[idx];
              if (!o.mask && o.keyoff != -1) fmKeyOn(o);
            }
          }
        }
        p.vwmask = ((p.slotmask & 0x80) ? 0x11 : 0) | ((p.slotmask & 0x40) ? 0x44 : 0) |
                   ((p.slotmask & 0x20) ? 0x22 : 0) | ((p.slotmask & 0x10) ? 0x88 : 0);
      }
      return;
    }
    case 0xCE: {
      const I a = rsw(pos + 1), b = rsw(pos + 3), c = rw(pos + 5);
      pos += 7;
      if (k == ADPCM) {
        adRep1_ = a >= 0 ? a + adStart_ : a + adStop_;
        adRep2_ = b > 0 ? b + adStart_ : b + adStop_;
        adRel_ = c < 0x8000 ? c + adStart_ : c > 0x8000 ? c + adStop_ : 0x8000;
      } else if (k == PPZ) {
        const I a2 = a >= 0 ? a : -a;
        const I b2 = b >= 0 ? b : (p.voice < 0x80 ? -a2 : -b);
        ev("PPZ loop " + ppzch() + " " + num(static_cast<uint32_t>(a2)) + " " +
           num(static_cast<uint32_t>(b2)));
      }
      return;
    }
    case 0xCD:
      if (ssg || pcm) {
        p.ep[0] = rd(pos + 1) & 0x1F;
        p.ep[2] = rd(pos + 2) & 0x1F;
        p.ep[3] = rd(pos + 3) & 0x1F;
        p.ep[5] = rd(pos + 4) & 0x0F;
        p.esl = ((rd(pos + 4) >> 4) & 0x0F) ^ 0x0F;
        p.eal = rd(pos + 5) & 0x0F;
        if (!p.eext) {
          p.eext = true;
          p.estate = 4;
          p.elevel = 0;
        }
      }
      pos += 6;
      return;
    case 0xCC:
      if (ssg) p.extend = (p.extend & ~I(1)) | (b1() & 1);
      pos += 2;
      return;
    case 0xCB:
    case 0xBC:
      if (!rhy) p.lfo[cmd == 0xCB ? 0 : 1].wave = b1();
      pos += 2;
      return;
    case 0xCA:
      if (!rhy) p.extend = (p.extend & ~I(2)) | ((b1() & 1) << 1);
      pos += 2;
      return;
    case 0xBB:
      if (!rhy) p.extend = (p.extend & ~I(0x20)) | ((b1() & 1) << 5);
      pos += 2;
      return;
    case 0xC9:
      if (ssg || pcm) p.extend = (p.extend & ~I(4)) | ((b1() & 1) << 2);
      pos += 2;
      return;
    case 0xC8:
    case 0xC7: {
      const int m = b1();
      const I v = rsw(pos + 2);
      pos += 4;
      if (!fm || !fm3ch()) return;
      for (int i = 0; i < 4; i++)
        if (m & (1 << i)) slotdt_[i] = cmd == 0xC8 ? v : slotdt_[i] + v;
      slotdtOn_ = slotdt_[0] || slotdt_[1] || slotdt_[2] || slotdt_[3];
      if (!slotdtOn_) slotdt_[0] = 0;
      fm3Check(p);
      return;
    }
    case 0xC6: {
      const I ws[3] = {rw(pos + 1), rw(pos + 3), rw(pos + 5)};
      pos += 7;
      if (!fm) return;
      for (int i = 0; i < 3; i++) {
        if (!ws[i]) continue;
        Part& e = part_[EXT + i];
        e.active = true;
        e.pos = ws[i];
        e.len = 1;
        e.keyoff = -1;
        e.lfo[0].dcCount = e.lfo[1].dcCount = e.lfo[0].dcCountR = e.lfo[1].dcCountR = -1;
        e.note = e.prev = REST;
        e.vol = 108;
        e.pan = part_[PART_C].pan;
        e.mask |= 0x20;
      }
      return;
    }
    case 0xC5:
    case 0xBA: {
      const I v = b1();
      pos += 2;
      if (rhy || ssg) return;
      if (cmd == 0xC5 && !fm) return;
      const I m = (v & 0x0F) ? (((v & 0x0F) << 4) | 0x0F) : p.carrier;
      if (cmd == 0xC5) p.vlfoA = m;
      else p.vlfoB = m;
      // the ADPCM part and the PPZ parts run with the FM context left at ch 3 of port 0
      if (fm ? fm3ch() : (k == ADPCM || ctxCh_ == 3)) fm3Check(p);
      return;
    }
    case 0xC4:
      if (!rhy) p.q2 = b1();
      pos += 2;
      return;
    case 0xC2:
    case 0xB9: {
      if (rhy) {
        pos += 2;
        return;
      }
      const I v = b1();
      pos += 2;
      if (cmd == 0xC2 || fm) {
        Lfo& l = p.lfo[cmd == 0xC2 ? 0 : 1];
        l.delay = l.delayR = v;
        lfoInit(p, cmd == 0xC2 ? 0 : 1);
      } else {   // the old player's quirk: the two sets stay exchanged (§7.10)
        std::swap(p.lfo[0], p.lfo[1]);
        p.lfosw = (p.lfosw & ~I(0x77)) | ((p.lfosw & 7) << 4) | ((p.lfosw >> 4) & 7);
        p.extend = (p.extend & ~I(0xFF)) | ((p.extend & 0x0F) << 4) | ((p.extend >> 4) & 0x0F);
        p.lfo[0].delay = p.lfo[0].delayR = v;
        lfoInit(p, 0);
      }
      return;
    }
    case 0xC1:
      pos += 1;
      return;
    case 0xC0:
      commandC0(p, pos);
      return;
    case 0xB8: {
      const int m = s1(), v = rd(pos + 2);
      pos += 3;
      if (!fm) return;
      static const int SLOTS[4][2] = {{1, 0x40}, {2, 0x48}, {3, 0x44}, {4, 0x4C}};
      for (int i = 0; i < 4; i++) {
        const int slot = SLOTS[i][0];
        if (!(m & (1 << (slot - 1))) || !(p.slotmask & (0x10 << (slot - 1)))) continue;
        const I x = m >= 0 ? (v & 0x7F) : p.tl[slot] + v;
        p.tl[slot] = x;
        if (!p.mask) w(ctxP_ + SLOTS[i][1] + ctxCh_ - 1, x);
      }
      return;
    }
    case 0xB7: {
      const I v = b1();
      pos += 2;
      if (rhy) return;
      Lfo& l = p.lfo[v >= 0x80 ? 1 : 0];
      I c = v >= 0x80 ? (v & 0x7F) : v;
      if (c == 0) c = 255;
      l.dcCount = l.dcCountR = c;
      return;
    }
    case 0xB6: {
      const I s = s1();
      pos += 2;
      if (!fm) return;
      const bool f3 = fm3ch();
      I fb;
      if (s >= 0) {
        fb = ((s << 3) & 0xFF) | (s >> 5);
      } else {
        I s2 = (s & 0x40) ? s : (s & 7);
        const I old = ((f3 ? fm3fbalg_ : p.fbalg) >> 3) & 7;
        const I x = s2 + old;
        fb = x >= 8 ? 0x38 : x >= 0 ? (x << 3) : 0;
      }
      I v;
      if (f3) {
        if (!(p.slotmask & 0x10)) return;
        v = (fm3fbalg_ & 7) | fb;
        fm3fbalg_ = v;
      } else {
        v = (p.fbalg & 7) | fb;
      }
      w(ctxP_ + 0xB0 + ctxCh_ - 1, v);
      p.fbalg = v;
      return;
    }
    case 0xB5: {
      const I m = b1(), d = rd(pos + 2);
      pos += 3;
      if (!fm) return;
      p.sdelayMask = ((~m & 0x0F) << 4);   // the slots not in m's low nibble
      p.sdelay = p.sdelayCount = d;
      return;
    }
    case 0xB4: {
      I ws[8];
      for (int i = 0; i < 8; i++) ws[i] = rw(pos + 1 + 2 * i);
      pos += 17;
      if (k != ADPCM) return;
      for (int i = 0; i < 8; i++) {
        if (!ws[i]) continue;
        Part& z = part_[PPZP + i];
        z.active = true;
        z.pos = ws[i];
        z.len = 1;
        z.keyoff = -1;
        z.lfo[0].dcCount = z.lfo[1].dcCount = z.lfo[0].dcCountR = z.lfo[1].dcCountR = -1;
        z.note = z.prev = REST;
        z.vol = 128;
        z.pan = 5;
      }
      return;
    }
    case 0xB3:
      if (!rhy) p.qmin = b1();
      pos += 2;
      return;
    case 0xB2:
      if (!rhy) p.mtranspose = s1();
      pos += 2;
      return;
    case 0xB1:
      if (!rhy) p.randq = b1();
      pos += 2;
      return;
    default:
      unknown(pos);
      return;
  }
}

// §6.5
void Driver::commandC0(Part& p, I& pos) {
  const int b = rd(pos + 1);
  if (b == 1) {
    pos += 2;
    p.mask |= 0x40;
    if (p.mask != 0x40) return;
    switch (kind_) {
      case FM: fmSilencePart(p); break;
      case SSG: {
        const int t = 1 << (ctxCh_ - 1), u = 1 << (ctxCh_ + 2);
        w(0x07, reg7_ | t | u);
        break;
      }
      case ADPCM:
        w(0x101, 0x02);
        w(0x100, 0x01);
        break;
      case PPZ: ev("PPZ stop " + num(ctxCh_)); break;
      default: break;
    }
    return;
  }
  if (b == 0) {
    pos += 2;
    p.mask &= ~I(0x40);
    if (kind_ == FM && p.mask == 0) fmResend(p);
    return;
  }
  const I v = rd(pos + 2), sv = rs(pos + 2);
  auto rel = [&](I& vd) { vd = sv ? std::clamp<I>(vd + sv, 0, 255) : 0; };
  switch (b) {
    case 0xFF: vdFM_ = v; break;
    case 0xFE: rel(vdFM_); break;
    case 0xFD: vdSSG_ = v; break;
    case 0xFC: rel(vdSSG_); break;
    case 0xFB: vdAD_ = v; break;
    case 0xFA: rel(vdAD_); break;
    case 0xF9: vdRHY_ = v; break;
    case 0xF8: rel(vdRHY_); break;
    case 0xF7: break;   // the 86 board's volume mode: nothing here
    case 0xF6: vdPPZ_ = v; break;
    case 0xF5: rel(vdPPZ_); break;
    default:
      wd(pos + 1, 0x80);   // the part ends at the sub-command
      pos += 1;
      return;
  }
  pos += 3;
}

// §6.3
void Driver::tempo(I& pos) {
  const int b = rd(pos + 1);
  if (b <= 250) {
    tb_ = tbP_ = b;
    t_ = tP_ = tbToT(tb_);
    pos += 2;
    return;
  }
  if (b == 0xFF) {
    t_ = tP_ = std::max(rd(pos + 2), 18);
    tb_ = tbP_ = tToTb(t_);
    pos += 3;
    return;
  }
  const I s = rs(pos + 2);
  pos += 3;
  if (b == 0xFE) {
    I x = tbP_ + s;
    if (s < 0 && x < 0) x = 0;
    if (x > 250) x = 250;
    tb_ = tbP_ = x;
    t_ = tbToT(tb_);
  } else {
    I x = tP_ + s;
    if (s >= 0 && x > 255) x = 255;
    if (s < 0 && x < 0) x = 18;
    t_ = tP_ = x;
    tb_ = tToTb(t_);
  }
}

// §8.5
void Driver::rhythmCommand(int cmd, I& pos) {
  const int b = rd(pos + 1);
  auto fade = [&](I v) { return fadeLevel_ ? ((256 - fadeLevel_) * v) >> 8 : v; };
  switch (cmd) {
    case 0xEB:
      pos += 2;
      if (b == 0) return;
      if (fadeLevel_) w(0x11, ((256 - fadeLevel_) * rhyTotal_) >> 8);
      if (b < 0x80)
        for (int i = 0; i < 6; i++)
          if (b & (1 << i)) w(0x18 + i, rhy_[i]);
      w(0x10, b);
      return;
    case 0xEA: {
      pos += 2;
      const int i = b >> 5;
      if (i < 1 || i > 6) return;
      rhy_[i - 1] = (rhy_[i - 1] & 0xC0) | (b & 0x1F);
      w(0x18 + i - 1, rhy_[i - 1]);
      return;
    }
    case 0xE9: {
      pos += 2;
      const int i = (b >> 5) & 7;
      if (i < 1 || i > 6) return;
      rhy_[i - 1] = ((b & 3) << 6) | (rhy_[i - 1] & 0x1F);
      w(0x18 + i - 1, rhy_[i - 1]);
      return;
    }
    case 0xE8: {
      pos += 2;
      I v = b;
      if (vdRHY_) v = ((256 - vdRHY_) * v) >> 8;
      rhyTotal_ = v;
      w(0x11, fade(v));
      return;
    }
    case 0xE6: {
      pos += 2;
      I v = rhyTotal_ + rs(pos - 1);
      if (v >= 64) v = (v & 0x80) ? 0 : 63;
      rhyTotal_ = v;
      w(0x11, fade(v));
      return;
    }
    case 0xE5: {
      const int i = b;
      const I s = rs(pos + 2);
      pos += 3;
      if (i < 1 || i > 6) return;
      const I v = std::clamp<I>((rhy_[i - 1] & 0x1F) + s, 0, 31);
      rhy_[i - 1] = (rhy_[i - 1] & 0xE0) | v;
      w(0x18 + i - 1, rhy_[i - 1]);
      return;
    }
  }
}

// ---------------------------------------------------------------- FM (§7)

int Driver::fmBit(const Part& p) const {
  const int idx = static_cast<int>(&p - part_);
  return idx == PART_C ? 1 : idx == EXT ? 2 : idx == EXT + 1 ? 4 : 8;
}

void Driver::fmPitchValue(Part& p, I n) {
  if ((n & 0x0F) == 0x0F) {
    p.note = REST;
    if (!(p.lfosw & 0x11)) p.pitch = 0;
    return;
  }
  p.note = n;
  p.pitch = ((n >> 4) << 11) | FNUM[semi(n)];
}

static void blockCorrect(I& f, I& block) {
  while (f >= 0x26A) {
    if (f < 0x4D4) return;
    block += 0x800;
    if (block == 0x4000) {
      block = 0x3800;
      if (f >= 0x800) f = 0x7FF;
      return;
    }
    f -= 0x26A;
  }
  while (f < 0x26A) {
    block -= 0x800;
    if (block < 0) {
      block = 0;
      if (f < 8) f = 8;
      return;
    }
    f += 0x26A;
  }
}

void Driver::fmPitchOut(Part& p) {
  if (p.pitch == 0 || p.slotmask == 0) return;
  if (fm3ch() && fm3mode_ != 0x3F) {
    fm3PitchOut(p);
    return;
  }
  I f = (p.pitch & 0x7FF) + p.portaAcc + p.detune, block = p.pitch & 0x3800;
  if (p.lfosw & 1) f += p.lfo[0].value;
  if (p.lfosw & 0x10) f += p.lfo[1].value;
  blockCorrect(f, block);
  const I v = block | f;
  w(ctxP_ + 0xA4 + ctxCh_ - 1, v >> 8);
  w(ctxP_ + 0xA0 + ctxCh_ - 1, v);
}

// §7.6: FM3 special mode, a pitch per slot
void Driver::fm3PitchOut(Part& p) {
  const I base = (p.pitch & 0x7FF) + p.portaAcc + p.detune;
  const I mA = (p.vlfoA & 0x0F) ? p.vlfoA : 0xF0, mB = (p.vlfoB & 0x0F) ? p.vlfoB : 0xF0;
  I t = 0x80;
  static const int REGS[4][3] = {{4, 0xA6, 0xA2}, {3, 0xAC, 0xA8}, {2, 0xAE, 0xAA}, {1, 0xAD, 0xA9}};
  for (auto& r : REGS) {
    const int slot = r[0];
    if (!(p.slotmask & (0x10 << (slot - 1)))) continue;
    I f = base + slotdt_[slot - 1];
    if ((t & mA) && (p.lfosw & 1)) f += p.lfo[0].value;
    if ((t & mB) && (p.lfosw & 0x10)) f += p.lfo[1].value;
    if (slot != 1) t >>= 1;
    I block = p.pitch & 0x3800;
    blockCorrect(f, block);
    const I v = block | f;
    w(r[1], v >> 8);
    w(r[2], v);
  }
}

// §7.4
void Driver::fmVolOut(Part& p) {
  if (p.slotmask == 0) return;
  I v = p.oneshot ? p.oneshot - 1 : p.vol;
  if (vdFM_) v = ((256 - vdFM_) * v) >> 8;
  if (fadeLevel_ >= 2) v = ((256 - (fadeLevel_ >> 1)) * v) >> 8;
  I add[5] = {0, 0x80, 0x80, 0x80, 0x80};
  const I c = 255 - v;
  const I car = p.carrier & p.slotmask;
  I wr = car;
  for (int s = 1; s <= 4; s++)
    if (car & (0x10 << (s - 1))) add[s] = c;
  if (c != 255) {
    for (int set = 0; set < 2; set++) {
      if (!(p.lfosw & (set ? 0x20 : 0x02))) continue;
      const I m = p.vlfoA & p.slotmask;
      for (int s = 1; s <= 4; s++)
        if (m & (0x10 << (s - 1))) add[s] = std::clamp<I>(add[s] - p.lfo[set].value, 0, 255);
      wr |= m;
    }
  }
  static const int ORDER[4][2] = {{4, 0x4C}, {3, 0x44}, {2, 0x48}, {1, 0x40}};
  for (auto& o : ORDER) {
    const int s = o[0];
    if (!(wr & (0x10 << (s - 1)))) continue;
    const I x = std::max<I>(std::min<I>(add[s] + p.tl[s], 255) - 0x80, 0);
    w(ctxP_ + o[1] + ctxCh_ - 1, x);
  }
}

// §7.3
void Driver::fmKeyOn(Part& p) {
  if (p.note == REST) return;
  I& set = keyon_[ctxP_ ? 1 : 0][ctxCh_ - 1];
  set |= p.slotmask;
  if (p.sdelayCount) set &= p.sdelayMask;
  w(0x28, (ctxCh_ - 1) | set | (ctxP_ ? 4 : 0));
}

void Driver::fmKeyOff(Part& p, bool always) {
  if (!always && p.note == REST) return;
  I& set = keyon_[ctxP_ ? 1 : 0][ctxCh_ - 1];
  set &= ~p.slotmask;
  w(0x28, (ctxCh_ - 1) | set | (ctxP_ ? 4 : 0));
}

void Driver::fmSilencePart(Part& p) {
  static const int OFF[4][2] = {{0x80, 0}, {0x40, 4}, {0x20, 8}, {0x10, 12}};
  for (auto& o : OFF) {
    if (!(p.vwmask & o[0])) continue;
    w(ctxP_ + 0x40 + ctxCh_ - 1 + o[1], 0x7F);
    w(ctxP_ + 0x80 + ctxCh_ - 1 + o[1], 0x7F);
  }
  fmKeyOff(p, true);
}

void Driver::fmPanOut(Part& p) {
  w(ctxP_ + 0xB4 + ctxCh_ - 1, p.hlfoCount ? (p.pan & 0xC0) : p.pan);
}

void Driver::fmPanCopy(Part& p) {
  if (!fm3ch()) return;
  for (int idx : {PART_C, EXT, EXT + 1, EXT + 2}) part_[idx].pan = p.pan;
}

// §7.2
void Driver::fmVoice(Part& p, I n) {
  p.voice = n;
  if (p.mask) {
    int rec[26];
    record(n, rec);
    p.fbalg = rec[25];
    p.tl[1] = rec[5];
    p.tl[3] = rec[6];
    p.tl[2] = rec[7];
    p.tl[4] = rec[8];
    if (fm3ch() && p.vwmask) {
      if (!(p.slotmask & 0x10)) {
        const I old = fm3fbalg_;
        fm3fbalg_ = (rec[25] & 7) | (old & 0x38);
        p.fbalg = old & 0x38;
      } else {
        fm3fbalg_ = rec[25];
        p.fbalg = n;
      }
    }
    return;
  }
  fmVoiceSet(p, false);
}

void Driver::fmVoiceSet(Part& p, bool resend) {
  int rec[26];
  record(p.voice, rec);
  auto takeTl = [&] {
    p.tl[1] = rec[5];
    p.tl[3] = rec[6];
    p.tl[2] = rec[7];
    p.tl[4] = rec[8];
  };
  if (p.vwmask == 0) {
    takeTl();
    return;
  }
  fmSilencePart(p);
  I v = resend ? p.fbalg : rec[25];
  if (fm3ch()) {
    if (resend) v = fm3fbalg_;
    else if (!(p.slotmask & 0x10)) v = (fm3fbalg_ & 0x38) | (v & 7);
    fm3fbalg_ = v;
  }
  w(ctxP_ + 0xB0 + ctxCh_ - 1, v);
  p.fbalg = v;
  const int a = static_cast<int>(v & 7);
  if (!(p.vlfoA & 0x0F)) p.vlfoA = CARRIER[a];
  if (!(p.vlfoB & 0x0F)) p.vlfoB = CARRIER[a];
  p.carrier = CARRIER[a];
  const I tlmask = MODTL[a] & p.vwmask;
  static const int OFF[4] = {0, 4, 8, 12};
  const int base = static_cast<int>(ctxP_) + ctxCh_ - 1;
  for (int i = 0; i < 4; i++)
    if (p.vwmask & (0x80 >> i)) w(base + 0x30 + OFF[i], rec[1 + i]);
  for (int i = 0; i < 4; i++)
    if (tlmask & (0x80 >> i)) w(base + 0x40 + OFF[i], rec[5 + i]);
  for (int i = 0; i < 4; i++)
    if (p.vwmask & (0x08 >> i)) w(base + 0x50 + OFF[i], rec[9 + i]);
  for (int i = 0; i < 4; i++)
    if (p.vwmask & (0x80 >> i)) w(base + 0x60 + OFF[i], rec[13 + i]);
  for (int i = 0; i < 4; i++)
    if (p.vwmask & (0x08 >> i)) w(base + 0x70 + OFF[i], rec[17 + i]);
  for (int i = 0; i < 4; i++)
    if (p.vwmask & (0x80 >> i)) w(base + 0x80 + OFF[i], rec[21 + i]);
  takeTl();
}

// §7.2.1
void Driver::fmResend(Part& p) {
  if (p.vwmask == 0) return;
  I keep[5];
  std::copy(p.tl, p.tl + 5, keep);
  fmVoiceSet(p, true);
  std::copy(keep, keep + 5, p.tl);
  const I m = ~p.carrier & p.slotmask & 0xF0;
  static const int ORDER[4][2] = {{4, 0x4C}, {3, 0x44}, {2, 0x48}, {1, 0x40}};
  for (auto& o : ORDER)
    if (m & (0x10 << (o[0] - 1))) w(ctxP_ + o[1] + ctxCh_ - 1, keep[o[0]]);
  fmPanOut(p);
}

// §7.6
void Driver::fm3Check(Part& p) {
  bool need;
  if (p.slotmask == 0) need = false;
  else if (p.slotmask != 0xF0) need = true;
  else if (!(p.vlfoA & 0x0F)) need = false;
  else if (p.lfosw & 1) need = true;
  else if (!(p.vlfoB & 0x0F)) need = false;
  else need = (p.lfosw & 0x10) != 0;
  const int bit = fmBit(p);
  I mode;
  if (need) {
    fm3special_ |= bit;
    mode = 0x7F;
  } else {
    fm3special_ &= ~I(bit);
    mode = (fm3special_ == 0 && !slotdtOn_) ? 0x3F : 0x7F;
  }
  if (mode == fm3mode_) return;
  fm3mode_ = mode;
  w(0x27, mode & 0xCF);
  if (mode != 0x7F || bit == 1) return;
  const int savedP = ctxP_, savedCh = ctxCh_;
  ctxP_ = 0;
  ctxCh_ = 3;
  for (int idx : {PART_C, EXT, EXT + 1}) {
    if (&part_[idx] == &p) break;
    if (!part_[idx].mask) fmPitchOut(part_[idx]);
  }
  ctxP_ = savedP;
  ctxCh_ = savedCh;
}

// ---------------------------------------------------------------- SSG (§8)

void Driver::ssgPitchValue(Part& p, I n) {
  if ((n & 0x0F) == 0x0F) {
    p.note = REST;
    if (!(p.lfosw & 0x11)) p.pitch = 0;
    return;
  }
  p.note = n;
  I v = SSGT[semi(n)];
  const I o = n >> 4;
  if (o > 0) v = (v + 1) >> o;
  p.pitch = v;
}

void Driver::ssgPitchOut(Part& p) {
  if (p.pitch == 0) return;
  I x = p.pitch + p.portaAcc;
  if (!(p.extend & 1)) {
    x -= p.detune;
    if (p.lfosw & 1) x -= p.lfo[0].value;
    if (p.lfosw & 0x10) x -= p.lfo[1].value;
  } else {
    if (p.detune) {
      I y = (x * p.detune) >> 12;
      y = y >= 0 ? y + 1 : y - 1;
      x -= y;
    }
    if (p.lfosw & 0x11) {
      I y = ((p.lfosw & 1) ? p.lfo[0].value : 0) + ((p.lfosw & 0x10) ? p.lfo[1].value : 0);
      if (y) {
        y = (x * y) >> 12;
        y = y >= 0 ? y + 1 : y - 1;
        x -= y;
      }
    }
  }
  if (x >= 0x1000) x = 0xFFF;
  w((ctxCh_ - 1) * 2, x);
  w((ctxCh_ - 1) * 2 + 1, x >> 8);
}

void Driver::ssgVolOut(Part& p) {
  if (p.eext ? p.estate == 0 : p.estate == 3) return;
  const int reg = 7 + ctxCh_;
  I v = p.oneshot ? p.oneshot - 1 : p.vol;
  v = ((256 - vdSSG_) * v) >> 8;
  v = ((256 - fadeLevel_) * v) >> 8;
  if (v <= 0) {
    w(reg, 0);
    return;
  }
  if (p.eext) {
    if (p.elevel == 0) {
      w(reg, 0);
      return;
    }
    v = (((v * (p.elevel + 1)) >> 3) + 1) >> 1;
  } else {
    v += p.elevel;
    if (v <= 0) {
      w(reg, 0);
      return;
    }
    if (v > 15) v = 15;
  }
  if (!(p.lfosw & 0x22)) {
    w(reg, v);
    return;
  }
  v += ((p.lfosw & 2) ? p.lfo[0].value : 0) + ((p.lfosw & 0x20) ? p.lfo[1].value : 0);
  if (v < 0) {
    w(reg, 0);
    return;
  }
  if (v > 15) v = 15;
  w(reg, v);
}

void Driver::ssgKeyOn(Part& p) {
  if (p.note == REST) return;
  const I t = 1 << (ctxCh_ - 1), u = 1 << (ctxCh_ + 2);
  w(0x07, (reg7_ | t | u) & ~((t | u) & p.mixer));
  if (noiseMem_ != noiseLast_ && effPri_ == 0) {
    w(0x06, noiseMem_);
    noiseLast_ = noiseMem_;
  }
}

void Driver::envKeyOff(Part& p) {
  if (p.note == REST) return;
  p.estate = p.eext ? 4 : 2;
}

// §8.4
void Driver::envStart(Part& p) {
  if (!p.eext) {
    p.estate = 0;
    p.elevel = 0;
    p.ep[0] = p.ep[1];
    if (p.ep[1] == 0) {
      p.estate = 1;
      p.elevel = p.ep[2];
    }
    p.ep[3] = p.ep[4];
    p.ep[5] = p.ep[6];
    return;
  }
  p.ep[1] = p.ep[0] - 16;
  p.edrc = p.ep[2] < 16 ? (p.ep[2] - 16) * 2 : p.ep[2] - 16;
  p.ep[4] = p.ep[3] < 16 ? (p.ep[3] - 16) * 2 : p.ep[3] - 16;
  p.ep[6] = p.ep[5] * 2 - 16;
  p.elevel = p.eal;
  p.estate = 1;
  envStepOnce(p);
}

bool Driver::envStep(Part& p) {
  if (p.extend & 4) {
    const I n = taCount_ - lastTA_;
    if (n == 0) return false;
    bool changed = false;
    for (I k = 0; k < n; k++) changed |= envStepOnce(p);
    return changed;
  }
  return envStepOnce(p);
}

bool Driver::envStepOnce(Part& p) {
  const I before = p.elevel;
  if (!p.eext) {
    switch (p.estate) {
      case 0:
        if (--p.ep[0] == 0) {
          p.estate = 1;
          p.elevel = p.ep[2];
        }
        break;
      case 2:
        if (p.ep[6] == 0) {
          p.elevel = -15;
        } else if (--p.ep[5] == 0) {
          p.ep[5] = p.ep[6];
          p.elevel--;
          if (p.elevel < -15 || p.elevel > 14) p.elevel = -15;
        }
        break;
      default:   // 1 decay, 3 none
        if (p.ep[4] != 0 && --p.ep[3] == 0) {
          p.ep[3] = p.ep[4];
          p.elevel--;
        }
        break;
    }
    return p.elevel != before;
  }
  auto reload = [](I r) { return r < 16 ? (r - 16) * 2 : r - 16; };
  switch (p.estate) {
    case 1:
      if (p.ep[1] > 0) {
        p.elevel += p.ep[1];
        if (p.elevel < 15) {
          p.ep[1] = p.ep[0] - 16;
        } else {
          p.elevel = 15;
          p.estate = p.esl == 15 ? 3 : 2;
        }
      } else if (p.ep[0] != 0) {
        p.ep[1]++;
      }
      break;
    case 2:
      if (p.edrc > 0) {
        p.elevel -= p.edrc;
        if (p.elevel < 0 || p.elevel < p.esl) {
          p.elevel = p.esl;
          p.estate = 3;
        } else {
          p.edrc = reload(p.ep[2]);
        }
      } else if (p.ep[2] != 0) {
        p.edrc++;
      }
      break;
    case 3:
      if (p.ep[4] > 0) {
        p.elevel = std::max<I>(p.elevel - p.ep[4], 0);
        p.ep[4] = reload(p.ep[3]);
      } else if (p.ep[3] != 0) {
        p.ep[4]++;
      }
      break;
    case 4:
      if (p.ep[6] > 0) {
        p.elevel = std::max<I>(p.elevel - p.ep[6], 0);
        p.ep[6] = p.ep[5] * 2 - 16;
      } else if (p.ep[5] != 0) {
        p.ep[6]++;
      }
      break;
    default:
      break;
  }
  return p.elevel != before;
}

// §11.3: 0 keep (the note is a rest), 1 restore (the part plays again)
int Driver::drumCheck(Part& p, int b) {
  if ((p.mask & 1) || !(p.mask & 2) || effPri_ >= 2 || (b & 0x0F) == 0x0F) return 0;
  if (effPri_ == 1) effectEnd();
  p.mask &= ~I(2);
  return p.mask == 0 ? 1 : 0;
}

// ---------------------------------------------------------------- ADPCM and PPZ8 (§9, §10)

void Driver::adPitchValue(Part& p, I n) {
  if ((n & 0x0F) == 0x0F) {
    p.note = REST;
    if (!(p.lfosw & 0x11)) p.pitch = 0;
    return;
  }
  const I o = n >> 4, s = semi(n);
  if (kind_ == PPZ) {
    p.note = n;
    const uint32_t v = PPZT[s];
    p.pitch = o < 4 ? (v >> (4 - o)) : static_cast<uint32_t>(v << (o - 4));
    return;
  }
  I v = ADT[s];
  if (o >= 6) {
    if (v < 0x8000) {
      v *= 2;
      p.note = 0x60 | s;
    } else {
      p.note = 0x50 | s;
    }
  } else {
    v >>= (5 - o);
    p.note = n;
  }
  p.pitch = v;
}

void Driver::pcmPitchOut(Part& p) {
  if (p.pitch == 0) return;
  const I lfo = ((p.lfosw & 1) ? p.lfo[0].value : 0) + ((p.lfosw & 0x10) ? p.lfo[1].value : 0);
  if (kind_ == PPZ) {
    const uint32_t x = static_cast<uint32_t>(p.pitch + 16 * p.portaAcc);
    const I y = lfo + p.detune;
    const I r = std::clamp<I>(static_cast<I>(x) + static_cast<I>(x / 256) * y, 0, 0xFFFFFFFFLL);
    ev("PPZ pitch " + num(ctxCh_) + " " + num(r));
    return;
  }
  const I x = std::clamp<I>(p.pitch + p.portaAcc + 4 * lfo + p.detune, 0, 0xFFFF);
  w(0x109, x);
  w(0x10A, x >> 8);
}

void Driver::pcmVolOut(Part& p) {
  const bool ppz = kind_ == PPZ;
  const std::string ch = num(ctxCh_);
  auto off = [&] {
    if (ppz) ev("PPZ stop " + ch);
    else w(0x10B, 0);
  };
  I v = p.oneshot ? p.oneshot : p.vol;
  v = ((256 - (ppz ? vdPPZ_ : vdAD_)) * v) >> 8;
  if (fadeLevel_) {
    if (ppz) v = ((256 - fadeLevel_) * v) >> 8;
    else v = ((((256 - fadeLevel_) * (256 - fadeLevel_)) >> 8) * v) >> 8;
  }
  if (v == 0) {
    if (ppz) ev("PPZ vol " + ch + " 0");
    off();
    return;
  }
  if (p.eext) {
    if (p.elevel == 0) {
      off();
      return;
    }
    v = (((v * (p.elevel + 1)) >> 3) + 1) >> 1;
  } else if (p.elevel < 0) {
    if (v < -16 * p.elevel) {
      off();
      return;
    }
    v -= -16 * p.elevel;
  } else {
    v = std::min<I>(v + 16 * p.elevel, 255);
  }
  if (p.lfosw & 0x22) {
    const I y = ((p.lfosw & 2) ? p.lfo[0].value : 0) + ((p.lfosw & 0x20) ? p.lfo[1].value : 0);
    v += y;
    if (y >= 0) {
      if (v & 0xFF00) v = 255;
    } else if (v < 0) {
      v = 0;
    }
  }
  if (ppz) {
    if (v) ev("PPZ vol " + ch + " " + num(v >> 4));
    else ev("PPZ stop " + ch);
    return;
  }
  w(0x10B, v);
}

void Driver::pcmKeyOn(Part& p) {
  if (p.note == REST) return;
  if (kind_ == PPZ) {
    ev("PPZ play " + num(ctxCh_) + " " + num(p.voice < 0x80 ? 0 : 1) + " " + num(p.voice & 0x7F));
    return;
  }
  w(0x101, 0x02);
  w(0x100, 0x21);
  w(0x102, adStart_);
  w(0x103, adStart_ >> 8);
  w(0x104, adStop_);
  w(0x105, adStop_ >> 8);
  if (adRep1_ == 0 && adRep2_ == 0) {
    w(0x100, 0xA0);
    w(0x101, p.pan | 2);
  } else {
    w(0x100, 0xB0);
    w(0x101, p.pan | 2);
    w(0x102, adRep1_);
    w(0x103, adRep1_ >> 8);
    w(0x104, adRep2_);
    w(0x105, adRep2_ >> 8);
  }
}

void Driver::pcmKeyOff(Part& p) {
  if (kind_ == ADPCM) {
    if (releasing(p)) return;
    if (adRel_ != 0x8000) {
      w(0x100, 0x21);
      w(0x102, adRel_);
      w(0x103, adRel_ >> 8);
      w(0x104, adStop_);
      w(0x105, adStop_ >> 8);
      w(0x100, 0xA0);
    }
  }
  envKeyOff(p);
}

// ---------------------------------------------------------------- SSG effects (§11)

void Driver::effectStart(int e) {
  effNum_ = e;
  if (e < 0 || e > 12) return;   // effects 13-69 are not used by any song known
  if (effPri_ <= EFFECTS[e].priority) {
    part_[PART_I].mask |= 2;
    effIdx_ = e;
    effRow_ = 0;
    effectRow();
    effPri_ = EFFECTS[e].priority;
  }
}

void Driver::effectRow() {
  const Effect& fx = EFFECTS[effIdx_];
  if (effRow_ >= fx.rows) {
    effectEnd();
    return;
  }
  const int* r = fx.row[effRow_];
  effDur_ = r[0];
  w(0x04, r[1]);
  w(0x05, r[2]);
  effTone_ = r[2] * 256 + r[1];
  effNoise_ = r[3];
  noiseLast_ = r[3];
  w(0x06, r[3]);
  w(0x07, ((r[4] << 2) & 0x24) | (reg7_ & 0xDB));
  w(0x0A, r[5]);
  w(0x0B, r[6]);
  w(0x0C, r[7]);
  w(0x0D, r[8]);
  effToneSw_ = r[9];
  effNoiseSw_ = r[10];
  effNoiseCnt_ = r[10] & 15;
}

void Driver::effectStep() {
  if (--effDur_ != 0) {
    effTone_ += effToneSw_;
    w(0x04, effTone_);
    w(0x05, effTone_ >> 8);
    if (effNoiseSw_ && --effNoiseCnt_ == 0) {
      effNoiseCnt_ = effNoiseSw_ & 15;
      effNoise_ += effNoiseSw_ / 16;
      w(0x06, effNoise_);
      noiseLast_ = effNoise_;
    }
    return;
  }
  effRow_++;
  effectRow();
}

void Driver::effectEnd() {
  w(0x0A, 0);
  w(0x07, (reg7_ & 0xDB) | 0x24);
  effPri_ = 0;
  effNum_ = -1;
}

// ---------------------------------------------------------------- length scan (§14.1)

namespace {
struct NoSink : Sink {
  void reg(int, int) override {}
  void unit(const std::string&) override {}
};
}  // namespace

bool scan(Random& rng, const uint8_t* file, size_t size, Scan& out) {
  Driver d(rng, nullptr);
  d.init();
  if (!d.load(file, size).empty()) return false;
  d.start();
  d.rhythmKeys = 0;
  I total = 0, length = 0;
  out = Scan();
  constexpr I LIMIT = 4LL * 3600 * 1000000;   // a song that never says (no timer runs)
  for (;;) {
    d.event();
    const uint32_t u = d.nextEvent();
    if (u == 0 || total > LIMIT) {
      length = 0;
      break;
    }
    d.advance(u);
    total += u;
    const int lc = d.loopCount();
    if (lc == 1 && length == 0) {
      length = total / 1000;
    } else if (lc == -1) {
      length = total / 1000;
      out.loopMs = 0;
      break;
    } else if (d.barTicks() >= 65536) {
      length = total / 1000;
      out.loopMs = length;
      break;
    }
    if (lc == 2) {
      out.loopMs = total / 1000 - length;
      break;
    }
  }
  out.lengthMs = length;
  out.usesRhythm = d.rhythmKeys > 0;
  return true;
}

}  // namespace pmdc
