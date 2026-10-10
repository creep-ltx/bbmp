// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-retro, engine "vgemm": VGM Music Maker (Shiru, 2011), *.vge.
//
// VGM Music Maker is a tracker for the Sega Mega Drive / Genesis sound: the YM2612 (six
// FM channels, the sixth also plays samples) and the SN76489 (three tones and a noise).
// Its songs keep patterns of ten channels, FM instruments of four operators, and
// samples as WAV data. The editor exports a song as a VGM file, a log of every write to
// the two chips, frame by frame.
//
// This engine does what the editor's export does: it plays the song through a replayer
// written after the export's observed behaviour (vgm_mm.exe -vgm, run on the songs and on
// test songs made for the purpose, write for write), builds the same kind of VGM in
// memory, and lets Game Music Emu's YM2612 and SN76489 play it. Nothing of the editor's
// code is in here; the tables below (pitches, volumes, the PSG envelope) were measured
// from its output. docs/replayers.md says how closely it matches.
//
// File layout: "VGEfmtV1".."V3", the header, then TFM Music Maker's RLE over texts,
// the order list, instruments, sample names and settings, pattern lengths and 256
// patterns of 10 channels x 11 columns x 256 rows; then the sample descriptions (RLE)
// and the PCM of the samples.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <gme/gme.h>

#include "engine.h"

namespace {

using retro::le16;
using retro::le32;

struct Bad {
  std::string why;
};

// ---------------------------------------------------------------- the file

// TFM Music Maker's run-length scheme: 0x80 followed by a length (7 bits a byte, low
// first, the last byte has its top bit set) repeats the byte before it; a length of 0
// stands for 0x80 itself. The count includes the byte before, which was read already.
class Rle {
 public:
  Rle(const std::vector<uint8_t>& d, size_t at) : d_(d), p_(at) {}
  uint8_t get() {
    if (left_ > 0) {
      left_--;
      return ch_;
    }
    const uint8_t v = raw();
    if (v != 0x80) return v;
    const uint8_t prev = p_ >= 2 ? d_[p_ - 2] : 0;
    unsigned n = 0;
    for (int shift = 0;; shift += 7) {
      const uint8_t t = raw();
      if (shift < 28) n |= static_cast<unsigned>(t & 0x7f) << shift;
      if (t & 0x80) break;
    }
    if (n == 0) return 0x80;
    if (n > (1u << 24)) throw Bad{"a run in the file is too long"};
    ch_ = prev;
    left_ = static_cast<long>(n) - 2;
    return prev;
  }
  void read(uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = get();
  }
  void skip(size_t n) {
    for (size_t i = 0; i < n; i++) get();
  }
  size_t pos() const { return p_; }

 private:
  uint8_t raw() {
    if (p_ >= d_.size()) throw Bad{"the file ends too early"};
    return d_[p_++];
  }
  const std::vector<uint8_t>& d_;
  size_t p_;
  long left_ = 0;
  uint8_t ch_ = 0;
};

struct Op {
  int mult = 0, dt = 0, tl = 127, rs = 0, ar = 31, dr = 31, am = 0, sr = 31, rr = 15, sl = 0, ssg = 0;
};

struct Instrument {
  bool used = false;
  int alg = 0, fb = 0, ams = 0, fms = 0;
  Op op[4];
};

struct SampleInfo {
  int slot = -1, volume = 0x80, pitch = 0;
};

struct Pcm {
  uint32_t length = 0;
  unsigned rate = 0, bits = 0;
  size_t offset = 0;
};

enum { kRows = 256, kChannels = 10, kColumns = 11 };

struct Song {
  int version = 0;
  unsigned fmClock = 7600489, psgClock = 3546894, hz = 50;
  int speedEven = 6, speedOdd = 6, interleave = 1, pcmQuality = 11025;
  int orders = 0, loop = 0;
  std::string author, name, notes;
  uint8_t orderList[256] = {};
  uint8_t patternLength[256] = {};
  std::string insName[255];
  Instrument ins[255];
  std::string sampleName[255];
  SampleInfo sample[255];
  Pcm pcm[255];
  std::vector<uint8_t> patterns;   // [pattern][channel][column][row]
  int patternCount = 0;
  const std::vector<uint8_t>* file = nullptr;

  const uint8_t* column(int pattern, int ch, int col) const {
    return &patterns[((static_cast<size_t>(pattern) * kChannels + ch) * kColumns + col) * kRows];
  }
  int rows(int pattern) const { return patternLength[pattern] ? patternLength[pattern] : 256; }
};

std::string text(const uint8_t* p, size_t n) {
  size_t k = 0;
  while (k < n && p[k]) k++;
  std::string s(reinterpret_cast<const char*>(p), k);
  for (char& c : s)
    if (c == '\r') c = ' ';
  return retro::field(s.data(), s.size());
}

Song readSong(const std::vector<uint8_t>& d) {
  Song s;
  s.file = &d;
  if (d.size() < 40 || std::memcmp(d.data(), "VGEfmtV", 7) != 0 || d[7] < '1' || d[7] > '3')
    throw Bad{"not a VGM Music Maker module"};
  s.version = d[7] - '0';
  size_t p = 8;
  const unsigned headerSize = le32(&d[p]), sampleDescSize = le32(&d[p + 4]);
  p += 8;
  if (s.version >= 3) {
    s.fmClock = le32(&d[p]);
    s.psgClock = le32(&d[p + 4]);
    s.hz = le16(&d[p + 8]);
    p += 10;
    if (s.fmClock < 1000000 || s.fmClock > 20000000) s.fmClock = 7600489;
    if (s.psgClock < 500000 || s.psgClock > 10000000) s.psgClock = 3546894;
    if (s.hz < 10 || s.hz > 1000) s.hz = 50;
  }
  // the editor lets a speed be 1..15 frames a row; a damaged file gets no more
  s.speedEven = std::max(1, std::min(15, static_cast<int>(d[p])));
  s.speedOdd = std::max(1, std::min(15, static_cast<int>(d[p + 1])));
  s.interleave = d[p + 2];
  s.pcmQuality = le16(&d[p + 3]);
  p += 3 + 8;
  s.orders = d[p];
  s.loop = d[p + 1];
  p += 2;
  if (s.orders == 0) throw Bad{"the song has no orders"};
  if (s.pcmQuality < 1000 || s.pcmQuality > 44100) s.pcmQuality = 11025;

  Rle r(d, p);
  uint8_t buf[512];
  r.read(buf, 64);
  s.author = text(buf, 64);
  r.read(buf, 64);
  s.name = text(buf, 64);
  r.read(buf, 384);
  s.notes = text(buf, 384);
  r.read(s.orderList, 256);
  for (int i = 0; i < 255; i++) {
    r.read(buf, 16);
    bool unused = true;
    for (int k = 0; k < 16; k++) unused = unused && buf[k] == 0xff;
    s.ins[i].used = !unused;
    if (!unused) s.insName[i] = text(buf, 16);
  }
  for (int i = 0; i < 255; i++) {
    r.read(buf, 43);
    Instrument& in = s.ins[i];
    in.alg = buf[0] & 7;
    in.fb = buf[1] & 7;
    in.ams = (buf[2] >> 4) & 3;
    in.fms = buf[2] & 7;
    for (int k = 0; k < 4; k++) {
      const uint8_t* o = &buf[3 + 10 * k];
      Op& op = in.op[k];
      op.mult = o[0] & 15;
      op.dt = static_cast<int8_t>(o[1]);
      op.tl = (o[2] ^ 0x7f) & 0x7f;
      op.rs = o[3] & 3;
      op.ar = (o[4] ^ 0x1f) & 0x1f;
      op.dr = (o[5] ^ 0x1f) & 0x1f;
      op.am = o[5] >> 7;
      op.sr = (o[6] ^ 0x1f) & 0x1f;
      op.rr = (o[7] ^ 0x0f) & 0x0f;
      op.sl = o[8] & 15;
      op.ssg = o[9] & 15;
    }
  }
  for (int i = 0; i < 255; i++) {
    r.read(buf, 16);
    s.sampleName[i] = text(buf, 16);
  }
  for (int i = 0; i < 255; i++) {
    const int n = s.version == 1 ? 2 : 4;
    r.read(buf, n);
    if (buf[0] == 0xff) continue;
    s.sample[i].slot = buf[0];
    s.sample[i].volume = buf[1];
    s.sample[i].pitch = n == 4 ? static_cast<int16_t>(le16(&buf[2])) : 0;
  }
  r.read(s.patternLength, 256);
  int maxPattern = 0;
  for (int i = 0; i < s.orders; i++) maxPattern = std::max(maxPattern, static_cast<int>(s.orderList[i]));
  s.patternCount = maxPattern + 1;
  s.patterns.resize(static_cast<size_t>(s.patternCount) * kChannels * kColumns * kRows);
  r.read(s.patterns.data(), s.patterns.size());

  // The samples: their descriptions follow the patterns (header and patterns take
  // <headerSize> bytes from offset 16), then the PCM of each slot in turn.
  const size_t descAt = 16 + static_cast<size_t>(headerSize);
  if (descAt < d.size()) {
    try {
      Rle sr(d, descAt);
      std::vector<uint8_t> desc(255 * 7);
      sr.read(desc.data(), desc.size());
      size_t at = descAt + sampleDescSize;
      for (int i = 0; i < 255; i++) {
        Pcm& pc = s.pcm[i];
        pc.length = le32(&desc[7 * i]);
        pc.rate = le16(&desc[7 * i + 4]);
        pc.bits = desc[7 * i + 6];
        if (!pc.length) continue;
        pc.offset = at;
        at += pc.length;
        if (at > d.size() || (pc.bits != 8 && pc.bits != 16) || pc.rate < 1000) {
          pc.length = 0;   // a damaged sample stays silent
          at = d.size();
        }
      }
    } catch (const Bad&) {
      // no usable samples
    }
  }
  return s;
}

// ---------------------------------------------------------------- measured tables

// The YM2612 F-numbers of the twelve semitones and the next C, block = octave; in
// between the pitch (1/32 semitone) goes in straight lines.
const int kFnum[13] = {645, 683, 724, 767, 812, 861, 912, 966, 1024, 1085, 1149, 1217, 1290};
// The volume column on FM: the output TL of a carrier is 127 - (127 - TL) * (127 - A) / 127.
const int kVolumeA[32] = {127, 127, 39, 37, 36, 34, 32, 31, 30, 29, 27, 25, 23, 21, 20, 18,
                          17,  15,  14, 13, 12, 11, 9,  8,  7,  6,  5,  4,  3,  2,  1,  0};
// Which operators (in register order 1, 3, 2, 4 = file order) are carriers.
const int kCarriers[8] = {0x8, 0x8, 0x8, 0x8, 0xc, 0xe, 0xe, 0xf};
const int kPitchMin = 12 * 32, kPitchMax = 108 * 32 - 1;
// The PSG envelope: a level 0..1 moved each frame by C * 1.25^rate (the release by
// 2 * RR), attack and decay measured separately.
const double kAttackC = 0.0009904, kDecayC = 0.0009904;
const size_t kMostBank = 32u << 20;   // bytes of samples

int fmFrequency(int pitch) {   // (block << 11) | fnum
  pitch = std::max(kPitchMin, std::min(kPitchMax, pitch));
  const int block = pitch / 384 - 1, inOct = pitch % 384, n = inOct / 32, k = inOct % 32;
  const int fnum = kFnum[n] + (kFnum[n + 1] - kFnum[n]) * k / 32;
  return block << 11 | fnum;
}

int psgPeriod(int pitch, double clock) {
  pitch = std::max(kPitchMin, std::min(kPitchMax, pitch));
  auto period = [clock](int semitone) {
    return clock / (32.0 * 440.0 * std::pow(2.0, (semitone - 45) / 12.0));
  };
  const int n = pitch / 32, k = pitch % 32;
  const double a = period(n), b = period(n + 1);
  const int p = static_cast<int>(a + (b - a) * k / 32.0);
  return std::max(0, std::min(1023, p));
}

// ---------------------------------------------------------------- the VGM being built

class Vgm {
 public:
  explicit Vgm(const Song& s) : song_(s) {}
  std::vector<uint8_t> cmd;
  long long now = 0;           // in 44100 Hz samples
  long long written = 0;       // time up to which waits are in <cmd>
  long long loopByte = -1, loopTime = 0;

  void waitUntil(long long t) {
    long long n = t - written;
    while (n > 0) {
      if (n <= 16) {
        cmd.push_back(static_cast<uint8_t>(0x70 + n - 1));
        n = 0;
      } else {
        const long long k = std::min<long long>(n, 65535);
        cmd.push_back(0x61);
        cmd.push_back(static_cast<uint8_t>(k & 0xff));
        cmd.push_back(static_cast<uint8_t>(k >> 8));
        n -= k;
      }
    }
    written = std::max(written, t);
  }
  void fm(int port, int reg, int val) {
    waitUntil(now);
    cmd.push_back(static_cast<uint8_t>(0x52 + port));
    cmd.push_back(static_cast<uint8_t>(reg));
    cmd.push_back(static_cast<uint8_t>(val));
  }
  void psg(int val) {
    waitUntil(now);
    cmd.push_back(0x50);
    cmd.push_back(static_cast<uint8_t>(val));
  }
  void seek(uint32_t at) {
    waitUntil(now);
    cmd.push_back(0xe0);
    for (int i = 0; i < 4; i++) cmd.push_back(static_cast<uint8_t>(at >> (8 * i)));
  }
  void dacAt(long long t) {   // write the next byte of the data bank at time t
    waitUntil(t);
    cmd.push_back(0x80);
  }
  void markLoop() {
    waitUntil(now);
    loopByte = static_cast<long long>(cmd.size());
    loopTime = now;
  }

  std::vector<uint8_t> finish(const std::vector<uint8_t>& bank, long long total, bool loop) {
    waitUntil(total);
    cmd.push_back(0x66);
    std::vector<uint8_t> v(0x40, 0);
    auto put32 = [&v](size_t at, uint32_t x) {
      for (int i = 0; i < 4; i++) v[at + i] = static_cast<uint8_t>(x >> (8 * i));
    };
    std::memcpy(v.data(), "Vgm ", 4);
    put32(0x08, 0x150);
    put32(0x0c, song_.psgClock);
    put32(0x18, static_cast<uint32_t>(total));
    put32(0x24, song_.hz);
    v[0x28] = 0x09;
    v[0x2a] = 16;
    put32(0x2c, song_.fmClock);
    put32(0x34, 0x0c);
    // the data block of the samples
    v.push_back(0x67);
    v.push_back(0x66);
    v.push_back(0x00);
    const size_t bankAt = v.size();
    v.resize(v.size() + 4);
    put32(bankAt, static_cast<uint32_t>(bank.size()));
    v.insert(v.end(), bank.begin(), bank.end());
    const size_t cmdAt = v.size();
    v.insert(v.end(), cmd.begin(), cmd.end());
    if (loop && loopByte >= 0) {
      put32(0x1c, static_cast<uint32_t>(cmdAt + loopByte - 0x1c));
      put32(0x20, static_cast<uint32_t>(total - loopTime));
    }
    put32(0x04, static_cast<uint32_t>(v.size() - 4));
    return v;
  }

 private:
  const Song& song_;
};

// The registers as last written, so that only changes go out (as the editor's export
// does); key on/off (0x28) and the noise control always go out.
class Chips {
 public:
  explicit Chips(Vgm& v) : vgm_(v) {
    forget();
    for (int& t : tone_) t = -1;
    for (int& a : att_) a = -1;
  }
  // At the song's loop point the export forgets what it wrote to the YM2612 (so that
  // the loop sounds right), not what it wrote to the PSG.
  void forget() {
    for (auto& port : fm_)
      for (int& r : port) r = -1;
    for (bool& b : toneLow_) b = false;
  }
  void fm(int port, int reg, int val) {
    if (fm_[port][reg] == val) return;
    fm_[port][reg] = val;
    vgm_.fm(port, reg, val);
  }
  void fmAlways(int port, int reg, int val) {
    fm_[port][reg] = val;
    vgm_.fm(port, reg, val);
  }
  void freq(int ch, int blockFnum) { freqPair(ch / 3, 0xa4 + ch % 3, 0xa0 + ch % 3, blockFnum); }
  void freqPair(int port, int hiReg, int loReg, int blockFnum) {   // high then low, as a pair
    const int hi = blockFnum >> 8, lo = blockFnum & 0xff;
    if (fm_[port][hiReg] == hi && fm_[port][loReg] == lo) return;
    fmAlways(port, hiReg, hi);
    fmAlways(port, loReg, lo);
  }
  void key(int val) { vgm_.fm(0, 0x28, val); }
  // The latch byte with the low bits, and the data byte when the high bits change.
  // At the loop point the export forgets the low bits it wrote, not the high ones.
  void tone(int ch, int period) {
    if (tone_[ch] == period && toneLow_[ch]) return;
    const int old = tone_[ch];
    tone_[ch] = period;
    toneLow_[ch] = true;
    vgm_.psg(0x80 | ch << 5 | (period & 15));
    if (old < 0 || (old >> 4) != (period >> 4)) vgm_.psg(period >> 4 & 0x3f);
  }
  void noise(int val) { vgm_.psg(0xe0 | val); }
  void attenuation(int ch, int a) {
    if (att_[ch] == a) return;
    att_[ch] = a;
    vgm_.psg(0x90 | ch << 5 | a);
  }
  // the reset the export starts with (not remembered)
  void reset() {
    vgm_.fm(0, 0x22, 0);
    vgm_.fm(0, 0x27, 0);
    vgm_.fm(0, 0x2b, 0);
    for (int ch = 0; ch < 4; ch++) {
      vgm_.psg(0x9f | ch << 5);
      vgm_.psg(0x80 | ch << 5);
      vgm_.psg(0);
      att_[ch] = 15;
      if (ch < 3) tone_[ch] = 0;
    }
  }
  int fmReg(int port, int reg) const { return fm_[port][reg]; }

 private:
  Vgm& vgm_;
  int fm_[2][256];
  int tone_[3];
  bool toneLow_[3] = {true, true, true};
  int att_[4];
};

// ---------------------------------------------------------------- the replayer

struct PsgEnvelope {
  enum { kAttack, kDecay, kSustain, kRelease, kOff };
  int phase = kOff;
  double level = 0;
  void keyOn() {   // from silence, not from the level the last note left (measured)
    phase = kAttack;
    level = 0;
  }
  void keyOff() {
    if (phase != kOff) phase = kRelease;
  }
  static double rate(double c, int r) { return c * std::pow(1.25, r); }
  void step(const Op& o) {
    switch (phase) {
      case kAttack:
        level += o.ar >= 31 ? 2.0 : rate(kAttackC, o.ar);
        if (level >= 1) {
          level = 1;
          phase = kDecay;
        }
        break;
      case kDecay: {
        const double sl = 1.0 - o.sl / 15.0;
        if (level <= sl) {
          phase = kSustain;
          break;
        }
        if (o.dr) level -= rate(kDecayC, o.dr);
        if (level <= sl) {
          level = sl;
          phase = kSustain;
        }
        break;
      }
      case kSustain:
        if (o.sr) level -= rate(kDecayC, o.sr);
        break;
      case kRelease:
        if (o.rr) level -= rate(kDecayC, 2 * o.rr);
        break;
      default:
        break;
    }
    if (level < 0) level = 0;
  }
};

struct Channel {
  int note = -1;          // the note (semitones) now playing
  int ins = 1;            // instrument number (1..255)
  int volume = 31;        // 1..31
  int volumeFine = 31 * 8;   // for volume slides, in eighths
  int pitch = 0;          // 1/32 semitones, the base pitch (slides move it)
  int target = 0;         // porta target
  int pan = 0xc0;
  int pans[4] = {}, panCount = 0;   // the P effects of the row, written in turn
  // effects
  int slide = 0;          // per frame, this row
  int lastSlideUp = 0, lastSlideDown = 0, lastPorta = 0;
  int portaSpeed = 0;
  bool porta = false;
  int arp = 0, arpPos = 0;
  int vibSpeed = 0, vibDepth = 0, vibPos = 0;
  bool vibrato = false;
  int volSlide = 0, lastVolSlide = 0;
  int retrig = 0, cut = -1, delay = -1;
  int pulses = 0;          // key off/on pairs this frame (retrig, delay: always four, measured)
  bool cutNow = false;
  bool noteThisRow = false;
  int tlMod[4] = {-1, -1, -1, -1};
  int multMod[4] = {-1, -1, -1, -1};
  int fbMod = -1;
  int multSet[4] = {-1, -1, -1, -1}, fbSet = -1;   // the modifications already written
  bool modWrite = false;   // E0x-E3x / EFx this row: written after the key on
  bool maskOnly = false;   // E5x without a note: a key write with the mask
  int keyMask = 0xf;
  // what to do on the channel this frame
  bool keyOffNow = false, keyOnNow = false, loadNow = false;
  bool keyedOn = true;     // as far as the export knows: at the start it keys off
  bool dirty = false;      // the frequency is looked at this frame
  bool levelDirty = false; // the levels are
  PsgEnvelope env;
  int noiseMode = -1;     // PSG 4: the noise last written
};

class Player {
 public:
  Player(const Song& s, Vgm& v) : song_(s), vgm_(v), chips_(v) {}

  void prepare() { buildBank(); }
  void run();
  long long frames() const { return frame_; }
  std::vector<uint8_t> bank;   // the samples as the DAC plays them

 private:
  void startRow();
  void rowCell(int ch, const uint8_t* cell);
  void frameEffects(int ch);
  void tickEvents(int ch);
  void writeFm();
  void writePsg();
  void writeDac();
  void finishDac(long long until);
  int speedOfRow() const;
  int fmTl(const Channel& c, int k) const;
  void buildBank();

  const Song& song_;
  Vgm& vgm_;
  Chips chips_;
  Channel ch_[kChannels];
  int order_ = 0, row_ = 0, tick_ = 0, rowSpeed_ = 1;
  int speedEven_ = 6, speedOdd_ = 6, interleave_ = 1;
  long long frame_ = 0;
  bool periodicNoise_ = false;
  bool noiseWrite_ = false;
  int loopRow_ = -1, loopCount_ = -1, loopOrder_ = 0;
  int jumpOrder_ = -1, jumpRow_ = -1;
  bool loopEndSeen_ = false;
  int lfo_ = 0;
  bool lfoSet_ = false, ch3Set_ = false;
  int speedRow_ = 0;       // rows since the pattern's start or the last Fxx (even/odd speed)
  bool ch3Mode_ = false;
  int ch3Offset_[4] = {};
  bool looped_ = false;
  // samples
  uint32_t sampleStart_[255] = {}, sampleLength_[255] = {}, playLength_[255] = {};
  long long dacAcc_ = 0;
  int playing_ = -1;          // sample index being played
  long long dacDone_ = 0;     // bytes of it written
  bool dacOn_ = false;
  int rowSample_ = -1;        // Sxx of this row
  int lastSample_ = -1;
  int sampleRetrig_ = 0, sampleCut_ = 0, sampleDelay_ = 0;   // Q9x, QCx, QDx of this row
};

int Player::speedOfRow() const {
  const int il = interleave_ ? interleave_ : 1;
  int sp = (speedRow_ / il) % 2 == 0 ? speedEven_ : speedOdd_;
  return sp ? sp : 1;
}

void Player::rowCell(int ch, const uint8_t* cell) {
  Channel& c = ch_[ch];
  const int note = cell[0], vol = cell[1], ins = cell[2];
  c.slide = 0;
  c.panCount = 0;
  c.porta = false;
  c.volSlide = 0;
  c.retrig = 0;
  c.cut = -1;
  c.delay = -1;
  c.noteThisRow = note > 1;
  c.keyOffNow = c.keyOnNow = c.loadNow = false;
  const bool hadVibrato = c.vibrato;
  c.vibrato = false;
  c.arpPos = 0;   // the arpeggio starts again on every row
  if (note > 1) c.dirty = c.levelDirty = true;
  if (vol) c.levelDirty = true;
  for (int e = 0; e < 4; e++) {
    const int fx = cell[3 + 2 * e];
    if ((fx == 0 && cell[4 + 2 * e]) || (fx >= 1 && fx <= 6) || fx == 14) c.dirty = true;
    if (fx == 5 || fx == 6 || fx == 8 || fx == 9 || fx == 10 || fx == 12 || fx == 13 || fx == 14) c.levelDirty = true;
  }
  // an instrument number counts only together with a note (measured)
  if (ins && note > 1) {
    if (ins != c.ins) {
      for (int k = 0; k < 4; k++) c.tlMod[k] = c.multMod[k] = c.multSet[k] = -1;
      c.fbMod = c.fbSet = -1;
    }
    c.ins = ins;
  }
  if (vol) {
    c.volume = std::min(31, static_cast<int>(vol));
    c.volumeFine = c.volume * 8;
  }
  bool hasNote = note > 1;
  int newPitch = 0;
  if (hasNote) {
    const int n = (~note) & 0xff;
    newPitch = n * 32;
  }
  bool portaRow = false, keepArp = false;
  c.keyMask = 0xf;
  c.modWrite = c.maskOnly = false;
  bool maskSet = false;
  for (int e = 0; e < 4; e++) {
    const int fx = cell[3 + 2 * e], val = cell[4 + 2 * e];
    switch (fx) {
      case 0:
        if (val) {
          c.arp = val;
          keepArp = true;
        }
        break;
      case 1:
        if (val) c.lastSlideUp = val;
        c.slide = c.lastSlideUp;
        break;
      case 2:
        if (val) c.lastSlideDown = val;
        c.slide = -c.lastSlideDown;
        break;
      case 3:
        if (val) c.lastPorta = val;
        c.porta = true;
        portaRow = true;
        break;
      case 4:
      case 6:
        if (fx == 4) {
          if (val >> 4) c.vibSpeed = val >> 4;
          if (val & 15) c.vibDepth = val & 15;
        } else {
          c.volSlide = val;
        }
        c.vibrato = true;
        break;
      case 5:
        c.porta = true;
        portaRow = true;
        c.volSlide = val;
        break;
      case 8:
      case 9:
      case 12:
      case 13: {
        const int k = fx == 8 ? 0 : fx == 9 ? 1 : fx == 12 ? 2 : 3;
        c.tlMod[k] = val & 0x7f;
        break;
      }
      case 10:
        if (val) c.lastVolSlide = val;
        c.volSlide = c.lastVolSlide;
        break;
      case 14: {
        const int x = val >> 4, y = val & 15;
        if (x <= 3) {
          c.multMod[x] = y;
          c.modWrite = true;
        } else if (x == 5) {
          c.keyMask = y;
          maskSet = true;
        } else if (x == 6) {
          // pattern loop: one count for all channels, every E6x of the row takes from
          // it, it starts again once a row with E6x does not jump (measured: E62 on
          // three channels plays the block twice)
          if (y == 0) {
            loopRow_ = row_;
            loopOrder_ = order_;
          } else if (loopRow_ >= 0) {
            loopEndSeen_ = true;
            if (loopCount_ < 0) loopCount_ = y;
            if (loopCount_ > 0) {
              loopCount_--;
              jumpOrder_ = loopOrder_;
              jumpRow_ = loopRow_;
            }
          }
        } else if (x == 9) c.retrig = y;
        else if (x == 12) c.cut = y;
        else if (x == 13) c.delay = y;
        else if (x == 14) {
          for (int k = 0; k < 4; k++) c.tlMod[k] = c.multMod[k] = c.multSet[k] = -1;
          c.fbMod = c.fbSet = -1;
        } else if (x == 15) {
          c.fbMod = y & 7;
          c.modWrite = true;
        }
        break;
      }
      case 15:
        speedRow_ = 0;
        if (val == 0) {
          speedEven_ = song_.speedEven;
          speedOdd_ = song_.speedOdd;
          interleave_ = song_.interleave;
        } else if ((val >> 4) == 0) {
          interleave_ = val & 15;
        } else {
          speedEven_ = val >> 4;
          speedOdd_ = val & 15 ? val & 15 : val >> 4;
        }
        break;
      case 21:   // L: LFO
        lfo_ = val >> 4 ? 8 | (val & 7) : 0;
        lfoSet_ = true;
        break;
      case 23:   // N: noise mode
        periodicNoise_ = val != 0;
        break;
      case 25: {   // P: pan
        const int l = val >> 4, r = val & 15;
        c.pan = (l && !r) ? 0x80 : (!l && r) ? 0x40 : 0xc0;
        c.pans[c.panCount++] = c.pan;
        break;
      }
      case 28:   // S: sample (on any channel, it plays on FM 6)
        rowSample_ = val;
        break;
      case 26: {   // Q: sample retrig/cut/delay
        const int x = val >> 4, y = val & 15;
        if (x == 9) sampleRetrig_ = y;
        else if (x == 12) sampleCut_ = y;
        else if (x == 13) sampleDelay_ = y;
        break;
      }
      case 11: {   // B: channel 3 special mode
        const int x = val >> 4, y = val & 15;
        if (x == 0) {
          ch3Mode_ = y != 0;
          ch3Set_ = true;
        } else if (x <= 3) {
          ch3Offset_[x] = y;
        }
        break;
      }
      default:
        break;
    }
  }
  // a note that slides (3xx) is not a new note: the arpeggio goes on
  const bool newNote = hasNote && !(portaRow && c.note >= 0);
  if (!keepArp && newNote) c.arp = 0;
  // The vibrato's phase goes on from where it was, unless a row without vibrato and
  // without a note ended it (a note ending it keeps the phase; measured).
  if (hadVibrato && !c.vibrato && !newNote) c.vibPos = 0;
  if (hadVibrato && !c.vibrato) c.dirty = true;   // the pitch goes back
  if (maskSet && note <= 1 && ch < 6) c.maskOnly = true;
  if (note == 1) {
    c.keyOffNow = c.keyedOn;
    c.keyedOn = false;
    if (ch >= 6) c.env.keyOff();
  }
  if (hasNote) {
    if (portaRow && c.note >= 0) {
      c.target = newPitch;
      c.portaSpeed = c.lastPorta;
    } else {
      c.note = newPitch / 32;
      c.pitch = newPitch;
      c.target = newPitch;
      c.porta = false;
      if (c.delay > 0) {   // (a delay the row is too short for: it never sounds)
        // EDx: the old note ends and the new one is set up now, it sounds at frame x
        c.keyOffNow = c.keyedOn;
        c.keyedOn = false;
        c.loadNow = true;
        if (ch >= 6) c.env.keyOff();
      } else {
        c.keyOffNow = c.keyedOn;
        c.keyOnNow = true;
        c.loadNow = true;
        c.keyedOn = true;
        if (ch >= 6 && !maskSet) c.env.keyOn();   // (E5x keeps a PSG note silent)
      }
    }
  }
}

void Player::startRow() {
  const int pat = song_.orderList[order_];
  jumpOrder_ = -1;
  jumpRow_ = -1;
  loopEndSeen_ = false;
  rowSample_ = -1;
  sampleRetrig_ = sampleCut_ = sampleDelay_ = 0;
  for (int ch = 0; ch < kChannels; ch++) {
    uint8_t cell[11];
    for (int col = 0; col < kColumns; col++) cell[col] = song_.column(pat, ch, col)[row_];
    rowCell(ch, cell);
  }
  if (loopEndSeen_ && jumpRow_ < 0) loopCount_ = -1;
}

// E9x retrig, ECx cut, EDx delay, on the frames of the row they name.
void Player::tickEvents(int ch) {
  Channel& c = ch_[ch];
  c.pulses = 0;
  c.cutNow = false;
  if (c.delay > 0 && c.delay < rowSpeed_ && tick_ == c.delay && c.note >= 0) {
    if (ch < 6) {
      c.pulses = 4;
    } else {
      c.env.keyOn();
      c.keyOnNow = true;
      c.levelDirty = true;
    }
  }
  if (c.retrig > 0 && tick_ % c.retrig == 0 && c.note >= 0) {
    if (ch < 6) {
      c.pulses = 4;
    } else {
      c.env.keyOn();
      if (!c.noteThisRow || tick_) c.keyOnNow = true;
    }
  }
  if (c.cut > 0 && c.cut < rowSpeed_ && tick_ == c.cut) {
    if (ch < 6) c.cutNow = true;
    else c.env.keyOff();
  }
}

void Player::frameEffects(int ch) {
  Channel& c = ch_[ch];
  if (c.porta || c.slide || c.arp || c.vibrato) c.dirty = true;
  if (c.volSlide) c.levelDirty = true;
  if (c.porta && c.portaSpeed) {
    if (c.pitch < c.target) c.pitch = std::min(c.target, c.pitch + c.portaSpeed);
    else if (c.pitch > c.target) c.pitch = std::max(c.target, c.pitch - c.portaSpeed);
  }
  if (c.slide) c.pitch = std::max(kPitchMin, std::min(kPitchMax, c.pitch + c.slide));
  if (c.volSlide) {
    const int up = c.volSlide >> 4, down = c.volSlide & 15;
    c.volumeFine += up ? up : -down;
    c.volumeFine = std::max(8, std::min(31 * 8, c.volumeFine));
    c.volume = c.volumeFine / 8;
  }
}

// Vibrato: a sine of 32 steps a period, the step (position / 2) moving by the speed
// each frame; the depth is in 1/16 semitone.
int vibratoOffset(const Channel& c) {
  const int step = (c.vibPos >> 1) & 31;
  return static_cast<int>(2 * c.vibDepth * std::sin(2 * M_PI * step / 32.0));
}

int Player::fmTl(const Channel& c, int k) const {
  const Instrument& in = song_.ins[c.ins - 1];
  int tl = c.tlMod[k] >= 0 ? c.tlMod[k] : in.op[k].tl;
  if (kCarriers[in.alg] >> k & 1) {
    const int a = kVolumeA[std::max(1, std::min(31, c.volume))];
    tl = 127 - (127 - tl) * (127 - a) / 127;
  }
  return tl;
}

void Player::writeFm() {
  // the LFO (Lxy) and channel 3's special mode (B0x) first
  if (lfoSet_) chips_.fm(0, 0x22, lfo_);
  if (ch3Set_) chips_.fm(0, 0x27, ch3Mode_ ? 0x40 : 0);
  lfoSet_ = ch3Set_ = false;
  // key off, instrument, key on
  for (int ch = 0; ch < 6; ch++) {
    Channel& c = ch_[ch];
    const int port = ch / 3, r = ch % 3, keyCh = port * 4 + r;
    for (int k = 0; k < c.panCount; k++) {
      const Instrument& in = song_.ins[c.ins - 1];
      chips_.fm(port, 0xb4 + r, c.pans[k] | in.ams << 4 | in.fms);
    }
    c.panCount = 0;
    if (c.keyOffNow) chips_.key(keyCh);
    if (c.loadNow) {
      const Instrument& in = song_.ins[c.ins - 1];
      chips_.fm(port, 0xb0 + r, (c.fbSet >= 0 ? c.fbSet : in.fb) << 3 | in.alg);
      chips_.fm(port, 0xb4 + r, c.pan | in.ams << 4 | in.fms);
      for (int k = 0; k < 4; k++) {
        const Op& o = in.op[k];
        const int dt = o.dt < 0 ? 4 + std::min(3, -o.dt) : std::min(3, o.dt);
        chips_.fm(port, 0x30 + 4 * k + r, dt << 4 | (c.multSet[k] >= 0 ? c.multSet[k] : o.mult));
      }
      for (int k = 0; k < 4; k++) chips_.fm(port, 0x50 + 4 * k + r, in.op[k].rs << 6 | in.op[k].ar);
      for (int k = 0; k < 4; k++) chips_.fm(port, 0x60 + 4 * k + r, in.op[k].am << 7 | in.op[k].dr);
      for (int k = 0; k < 4; k++) chips_.fm(port, 0x70 + 4 * k + r, in.op[k].sr);
      for (int k = 0; k < 4; k++) chips_.fm(port, 0x80 + 4 * k + r, in.op[k].sl << 4 | in.op[k].rr);
      for (int k = 0; k < 4; k++) chips_.fm(port, 0x90 + 4 * k + r, in.op[k].ssg);
    }
    if (c.keyOnNow) chips_.key(c.keyMask << 4 | keyCh);
    if (c.maskOnly) {
      chips_.key(c.keyMask << 4 | keyCh);
      c.keyedOn = c.keyMask != 0;
    }
    if (c.modWrite) {   // E0x-E3x, EFx of this row, after the key on
      const Instrument& in = song_.ins[c.ins - 1];
      chips_.fm(port, 0xb0 + r, (c.fbMod >= 0 ? c.fbMod : in.fb) << 3 | in.alg);
      for (int k = 0; k < 4; k++) {
        const Op& o = in.op[k];
        const int dt = o.dt < 0 ? 4 + std::min(3, -o.dt) : std::min(3, o.dt);
        chips_.fm(port, 0x30 + 4 * k + r, dt << 4 | (c.multMod[k] >= 0 ? c.multMod[k] : o.mult));
        c.multSet[k] = c.multMod[k];
      }
      c.fbSet = c.fbMod;
    }
  }
  // retrig and cut, frequency and levels
  for (int ch = 0; ch < 6; ch++) {
    Channel& c = ch_[ch];
    const int port = ch / 3, r = ch % 3, keyCh = port * 4 + r;
    for (int i = 0; i < c.pulses; i++) {
      if (c.keyedOn) chips_.key(keyCh);
      chips_.key(c.keyMask << 4 | keyCh);
      c.keyedOn = true;
    }
    if (c.cutNow && c.keyedOn) {
      chips_.key(keyCh);
      c.keyedOn = false;
    }
    if (c.note < 0 || !(c.dirty || c.levelDirty)) continue;
    int pitch = c.pitch;
    if (c.arp) {
      const int step = c.arpPos % 3;
      pitch += 32 * (step == 1 ? c.arp >> 4 : step == 2 ? c.arp & 15 : 0);
    }
    if (c.vibrato) pitch += vibratoOffset(c);
    if (c.dirty) {
      chips_.freq(ch, fmFrequency(pitch));
      if (ch == 2 && ch3Mode_) {   // the other three operators, B1y-B3y semitones above
        static const int hiReg[4] = {0, 0xac, 0xae, 0xad}, loReg[4] = {0, 0xa8, 0xaa, 0xa9};
        for (int x = 1; x <= 3; x++)
          chips_.freqPair(0, hiReg[x], loReg[x], fmFrequency(pitch + 32 * ch3Offset_[x]));
      }
    }
    if (c.levelDirty)
      for (int k = 0; k < 4; k++) chips_.fm(port, 0x40 + 4 * k + r, fmTl(c, k));
  }
}

void Player::writePsg() {
  const double clock = song_.psgClock;
  for (int ch = 6; ch < 9; ch++) {
    Channel& c = ch_[ch];
    if (c.note < 0 || !c.dirty) continue;
    int pitch = c.pitch;
    if (c.arp) {
      const int step = c.arpPos % 3;
      pitch += 32 * (step == 1 ? c.arp >> 4 : step == 2 ? c.arp & 15 : 0);
    }
    if (c.vibrato) pitch += vibratoOffset(c);
    // the instrument's detune (operator 4's DT) moves the period itself
    const int dt = song_.ins[c.ins - 1].op[3].dt;
    chips_.tone(ch - 6, std::max(0, std::min(1023, psgPeriod(pitch, clock) + dt)));
  }
  // PSG 4: its "note" picks the noise (0 high, 1 middle, 2 low, 3 PSG 3's pitch) and
  // follows pitch effects; the register is written on every note and when it changes
  Channel& n = ch_[9];
  if (n.note >= 0 && (n.keyOnNow || n.dirty)) {
    int pitch = n.pitch;
    if (n.arp) {
      const int step = n.arpPos % 3;
      pitch += 32 * (step == 1 ? n.arp >> 4 : step == 2 ? n.arp & 15 : 0);
    }
    if (n.vibrato) pitch += vibratoOffset(n);
    const int mode = (std::max(kPitchMin, std::min(kPitchMax, pitch)) / 32) & 3;
    if (n.keyOnNow || mode != n.noiseMode) chips_.noise((periodicNoise_ ? 0 : 4) | mode);
    n.noiseMode = mode;
  }
  for (int ch = 6; ch < 10; ch++) {
    Channel& c = ch_[ch];
    const Op& o = song_.ins[c.ins - 1].op[3];
    c.env.step(o);
    const int tl = c.tlMod[3] >= 0 ? c.tlMod[3] : o.tl;   // (Dxx works on the PSG too)
    const int q = c.volumeFine * (127 - tl) / 254;   // 4 * volume * (127 - TL) / 127, in eighths
    int att = 15;
    if (q > 0 && c.env.level > 0) att = 15 - std::min(15, static_cast<int>(std::floor(q * c.env.level * 15 / 127 + 1 + 1e-9)));
    if (c.env.phase == PsgEnvelope::kOff) att = 15;
    chips_.attenuation(ch - 6, att);
  }
}

void Player::run() {
  speedEven_ = song_.speedEven;
  speedOdd_ = song_.speedOdd;
  interleave_ = song_.interleave;
  chips_.reset();
  // the pans of the first row are set before the first frame
  {
    const int pat = song_.orderList[0];
    for (int ch = 0; ch < 6; ch++) {
      for (int e = 0; e < 4; e++) {
        const int fx = song_.column(pat, ch, 3 + 2 * e)[0], val = song_.column(pat, ch, 4 + 2 * e)[0];
        if (fx == 25) {
          const int l = val >> 4, r = val & 15;
          ch_[ch].pan = (l && !r) ? 0x80 : (!l && r) ? 0x40 : 0xc0;
        }
      }
    }
    for (int ch = 0; ch < 6; ch++) chips_.fm(ch / 3, 0xb4 + ch % 3, ch_[ch].pan);
  }
  order_ = 0;
  row_ = 0;
  tick_ = 0;
  const long long frameLen = 44100 / song_.hz;
  for (;;) {
    vgm_.now = frame_ * frameLen;
    if (tick_ == 0) {
      if (row_ == 0 && order_ == song_.loop && !looped_) {
        vgm_.markLoop();
        chips_.forget();
        for (int ch = 0; ch < 6; ch++) ch_[ch].keyedOn = true;
        dacOn_ = false;
        looped_ = true;
      }
      rowSpeed_ = speedOfRow();
      startRow();
      rowSpeed_ = speedOfRow();
    }
    for (int ch = 0; ch < kChannels; ch++) {
      tickEvents(ch);
      frameEffects(ch);
    }
    writeFm();
    writePsg();
    writeDac();
    for (int ch = 0; ch < kChannels; ch++) {
      Channel& c = ch_[ch];
      c.arpPos++;
      if (c.vibrato) c.vibPos += c.vibSpeed;
      c.keyOffNow = c.keyOnNow = c.loadNow = c.maskOnly = c.modWrite = false;
      c.dirty = c.levelDirty = false;
    }
    frame_++;
    if (++tick_ >= rowSpeed_) {
      tick_ = 0;
      if (jumpRow_ >= 0) {
        order_ = jumpOrder_;
        row_ = jumpRow_;
      } else if (++row_ >= song_.rows(song_.orderList[order_])) {
        row_ = 0;
        speedRow_ = -1;
        if (++order_ >= song_.orders) break;
      }
      speedRow_++;
    }
    if (frame_ > 3600LL * song_.hz) break;   // an hour: a pattern loop gone wrong
  }
  vgm_.now = frame_ * frameLen;
}

// The samples as the export lays them out in the VGM's data block: each resampled to
// the song's PCM quality with its own volume and pitch, 8 bits unsigned, followed by
// 17 bytes of silence, in the order of the sample list.
void Player::buildBank() {
  for (int i = 0; i < 255; i++) {
    SampleInfo si = song_.sample[i];
    sampleLength_[i] = 0;
    // volume and pitch belong to the WAV: samples that share one take the last one's
    for (int j = 254; j > i; j--)
      if (song_.sample[j].slot == si.slot) {
        si.volume = song_.sample[j].volume;
        si.pitch = song_.sample[j].pitch;
        break;
      }
    if (si.slot < 0 || si.slot >= 255 || !song_.pcm[si.slot].length) continue;
    const Pcm& pc = song_.pcm[si.slot];
    const std::vector<uint8_t>& d = *song_.file;
    const size_t n = pc.bits == 16 ? pc.length / 2 : pc.length;
    auto at = [&](size_t k) -> double {
      if (k >= n) k = n - 1;
      if (pc.bits == 16) return static_cast<int16_t>(le16(&d[pc.offset + 2 * k]));
      return (static_cast<int>(d[pc.offset + k]) - 128) * 256.0;
    };
    // the pitch byte: 0..255 plays up to twice as fast, -1..-255 down to half as fast
    double step = static_cast<double>(pc.rate) / song_.pcmQuality;
    if (si.pitch >= 0) step *= 1.0 + si.pitch / 255.0;
    else step /= 1.0 - si.pitch / 255.0;
    const size_t out = static_cast<size_t>(n / step);
    // Up to 255 entries may share a WAV, each slowed to half its speed and stretched
    // to 44 times its length: a file of a few hundred kB could ask for gigabytes. The
    // samples of a song take well under 1 MB; what doesn't fit into 32 MB stays silent.
    if (bank.size() + out + 17 > kMostBank) continue;
    sampleStart_[i] = static_cast<uint32_t>(bank.size());
    for (size_t k = 0; k < out; k++) {
      const double pos = k * step;
      const size_t a = static_cast<size_t>(pos);
      const double f = pos - a;
      double v = at(a) * (1 - f) + at(a + 1) * f;
      // the volume byte: about volume / 130 (fitted to the export's data, not exact)
      v = v * si.volume / 130.0;
      const int b = 128 + static_cast<int>(std::floor(v / 256.0 + 0.5));
      bank.push_back(static_cast<uint8_t>(std::max(1, std::min(255, b))));
    }
    sampleLength_[i] = static_cast<uint32_t>(out);
    bank.insert(bank.end(), 17, 0x80);
    // The export plays one byte a frame more than the quality asks for, and as many as
    // the sample lasts at that speed (into the silence behind it).
    const double played = static_cast<double>(song_.pcmQuality) + song_.hz;
    double lengthStep = static_cast<double>(pc.rate) / played;
    if (si.pitch >= 0) lengthStep *= 1.0 + si.pitch / 255.0;
    else lengthStep /= 1.0 - si.pitch / 255.0;
    playLength_[i] = static_cast<uint32_t>(n / lengthStep) + 1;
  }
}

// Sample playing on the DAC (channel 6): a frame's bytes spread evenly over the frame,
// from the frame on which the sample starts; when it is over, the DAC is switched off.
void Player::finishDac(long long frameStart) {
  // A byte at the start of every frame, then one each time an accumulator that runs
  // through the whole song passes a sample of the PCM quality (measured).
  const long long frameLen = 44100 / song_.hz;
  auto next = [&](long long t) {
    if (playing_ < 0) return;
    if (dacDone_ >= playLength_[playing_]) {
      const long long keep = vgm_.now;
      vgm_.now = std::max(t, vgm_.written);
      chips_.fmAlways(0, 0x2b, 0);
      vgm_.now = keep;
      dacOn_ = false;
      playing_ = -1;
      return;
    }
    vgm_.dacAt(t);
    dacDone_++;
  };
  next(frameStart);
  for (long long s = 0; s < frameLen; s++) {
    dacAcc_ += song_.pcmQuality;
    if (dacAcc_ >= 44100) {
      dacAcc_ -= 44100;
      next(frameStart + s);
    }
  }
}

// Sxx at the row's first frame, or at QDx's; Q9x starts it again every x frames of the
// row, QCx stops the bytes (the DAC stays on); Q without S means the last sample.
void Player::writeDac() {
  int s = -1;
  const int which = rowSample_ >= 0 ? rowSample_ : lastSample_;
  if (tick_ == 0 && rowSample_ >= 0 && sampleDelay_ == 0) s = rowSample_;
  if (sampleDelay_ > 0 && tick_ == sampleDelay_) s = which;
  if (sampleRetrig_ > 0 && tick_ % sampleRetrig_ == 0) s = which;
  if (sampleCut_ > 0 && tick_ == sampleCut_) playing_ = -1;
  if (s >= 0) {
    lastSample_ = s;
    if (s < 255 && sampleLength_[s]) {
      vgm_.seek(sampleStart_[s]);
      if (!dacOn_) chips_.fmAlways(0, 0x2b, 0x80);
      dacOn_ = true;
      playing_ = s;
      dacDone_ = 0;
    }
  }
  finishDac(vgm_.now);
}

// ---------------------------------------------------------------- the engine

class VgemmEngine : public retro::Engine {
 public:
  ~VgemmEngine() override {
    if (emu_) gme_delete(emu_);
  }

  bool open(const std::vector<uint8_t>& file, const std::string&, int, int rate, retro::SongInfo& info,
            std::string& error) override {
    std::vector<uint8_t> vgm;
    try {
      const Song song = readSong(file);
      Vgm v(song);
      Player player(song, v);
      player.prepare();
      player.run();
      const long long total = player.frames() * (44100 / song.hz);
      if (const char* dump = std::getenv("BBMP_RETRO_VGEMM_VGM")) {   // the chip writes, to compare
        const std::vector<uint8_t> withLoop = v.finish(player.bank, total, true);
        if (FILE* f = std::fopen(dump, "wb")) {
          std::fwrite(withLoop.data(), 1, withLoop.size(), f);
          std::fclose(f);
        }
      }
      vgm = v.finish(player.bank, total, false);
      info.title = retro::to_utf8(song.name);
      info.author = retro::to_utf8(song.author);
      info.comment = retro::to_utf8(song.notes);
      for (int i = 0; i < 255; i++)
        if (song.ins[i].used) info.names.push_back(retro::to_utf8(song.insName[i]));
      for (int i = 0; i < 255; i++)
        if (song.sample[i].slot >= 0) info.names.push_back(retro::to_utf8(song.sampleName[i]));
      info.type = "VGM Music Maker";
      info.duration_ms = total * 1000 / 44100;
      end_ = total * rate / 44100;
    } catch (const Bad& bad) {
      error = bad.why;
      return false;
    }
    if (const char* bad = gme_open_data(vgm.data(), static_cast<long>(vgm.size()), &emu_, rate)) {
      error = std::string("Game Music Emu: ") + bad;
      return false;
    }
    gme_ignore_silence(emu_, 1);
    if (const char* bad = gme_start_track(emu_, 0)) {
      error = std::string("Game Music Emu: ") + bad;
      return false;
    }
    return true;
  }

  size_t render(int16_t* out, size_t frames) override {
    if (at_ >= end_) return 0;
    if (static_cast<long long>(frames) > end_ - at_) frames = static_cast<size_t>(end_ - at_);
    if (gme_play(emu_, static_cast<int>(frames * 2), out)) return 0;
    at_ += static_cast<long long>(frames);
    return frames;
  }

 private:
  Music_Emu* emu_ = nullptr;
  long long at_ = 0, end_ = 0;
};

}  // namespace

REGISTER_ENGINE("vgemm", VgemmEngine)
