// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-t0ast: play a song of T0AST, "The 0ok Amazing Synth Tracker" (.t0ast), as raw
// PCM on stdout.
//
//   bbmp-t0ast <file> <track> <samplerate> [start-ms]
//   bbmp-t0ast --ref <file>      the whole song at 44100 Hz as 32-bit floats, no header
//                                (what the tracker's "Export as WAV" writes, less its header)
//
// T0AST is a softsynth tracker for 64k intros (2004-2006): 16 channels, each with its own
// synthesizer voice (oscillator, filter, vowel filter, delay, a 303 voice, links between
// channels for FM, ring modulation and chorus). This is a new player, written for bbmp
// from a description of how the tracker behaves (a clean room: its code was not looked
// at).
//
// Exactness. The tracker ran its synthesizer on the x87 unit at 64-bit precision with
// rounding toward zero, and its sound depends on that (every state value is a single,
// rounded toward zero from an extended result). On x86 (32 or 64 bit) this player does
// the same: long double is the x87 type, the rounding mode is set to toward-zero while
// the engine runs, and the x87 FSIN, FCOS and F2XM1 instructions are used through inline
// assembly. That gives the tracker's own WAV export bit for bit. On other CPUs the C
// library's sinl/cosl/exp2l stand in: the songs sound the same, but are not bit-exact
// (a few samples far below hearing in most songs; two feedback patches drift).
//
// The engine knows only 44100 Hz; other rates are resampled. The song plays once, as
// long as the tracker's export makes it (until a block of 2048 frames starts on the
// first tick of a row after the order list has come round).
//
// Same protocol as the other helpers: one JSON line describing the track, then signed
// 16-bit little-endian stereo until the song ends. A damaged file is refused with the
// reason; nothing in it is trusted.

#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "resample.h"

#if (defined(__x86_64__) || defined(__i386__)) && !defined(T0AST_PORTABLE)
#define T0AST_X87 1
#else
#define T0AST_X87 0
#endif

namespace {

typedef long double X;   // the x87 extended type on x86

const int RATE = 44100;
const int BLOCK = 2048;
const size_t MAX_FILE = 4u << 20;        // the largest known song is 195 KB
const long long MAX_FRAMES = 30LL * 60 * RATE;

// ---- arithmetic (description section 1)

X xbits(uint64_t sig, uint16_t ex) {
#if T0AST_X87
  unsigned char b[sizeof(X)] = {};
  std::memcpy(b, &sig, 8);
  std::memcpy(b + 8, &ex, 2);
  X r;
  std::memcpy(&r, b, sizeof(X));
  return r;
#else
  // not the x87 type: build the same value
  const int e = int(ex & 0x7FFF) - 16383 - 63;
  X r = std::ldexp(X(sig >> 32) * X(4294967296.0) + X(sig & 0xFFFFFFFFu), e);
  return (ex & 0x8000) ? -r : r;
#endif
}

float fbits(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// The constants of description 1.2, by their bits where they are not exact.
X F0, FP, EPS, D255, PI, LAMBDA;
const X D256 = 1.0L / 256, D64 = 1.0L / 64, D8 = 0.125L, HALF = 0.5L;
float TB_ATTACK, F_01, F_09, F0_S, F_255_256, F_001, F_M001;

void constants() {
  F0 = xbits(0xA377EE5A535C112FULL, 0x3FF8);
  FP = xbits(0xA377EE5A535C112FULL, 0x3FF4);
  EPS = xbits(0x9ABE14CD44753B53ULL, 0x3FAF);
  D255 = xbits(0x8080808080808081ULL, 0x3FF7);
  LAMBDA = xbits(0xE666666666666666ULL, 0x3FFE);
  PI = xbits(0xC90FDAA22168C234ULL, 0x4000);
  TB_ATTACK = fbits(0x3D65206B);
  F_01 = fbits(0x3DCCCCCD);
  F_09 = fbits(0x3F666666);
  F0_S = fbits(0x3C2377EE);
  F_255_256 = fbits(0x3F7F0000);
  F_001 = fbits(0x3C23D70A);
  F_M001 = fbits(0xBC23D70A);
}

// s(x): round to single (toward zero, by the rounding mode in force)
inline float s(X x) { return static_cast<float>(x); }

// trunc(x): s(x), then toward zero as a 32-bit integer; out of range (or NaN) gives what
// the x87 gives, the "integer indefinite" 0x80000000
inline int32_t itrunc(X x) {
  const float f = s(x);
  if (!(f > -2147483648.0f && f < 2147483648.0f)) return INT32_MIN;
  return static_cast<int32_t>(f);
}

// frac(x): y = s(x); y - trunc(y), exactly, with y's sign
inline float frac(X x) {
  const float y = s(x);
  if (!std::isfinite(y)) return y != y ? y : 0.0f;
  return s(X(y) - X(std::trunc(y)));
}

#if T0AST_X87
inline X fsin(X a) {
  X r;
  __asm__("fsin" : "=t"(r) : "0"(a));
  return r;
}
inline X fcos(X a) {
  X r;
  __asm__("fcos" : "=t"(r) : "0"(a));
  return r;
}
inline X f2xm1(X a) {
  X r;
  __asm__("f2xm1" : "=t"(r) : "0"(a));
  return r;
}
#else
inline X fsin(X a) { return sinl(a); }
inline X fcos(X a) { return cosl(a); }
inline X f2xm1(X a) { return exp2l(a) - 1; }
#endif

// pow2(y): y rounded to single, then 2^y the x87 way; the result stays extended
X pow2(X y0) {
  const float y = s(y0);
  if (!std::isfinite(y)) return y > 0 ? X(INFINITY) : y < 0 ? X(0) : X(y);
  const float t = std::trunc(y);
  const X f = X(y) - X(t);
  const X r = 1 + f2xm1(f);
  const float tc = t > 20000 ? 20000 : t < -20000 ? -20000 : t;
  return std::ldexp(r, int(tc));
}

inline X xexp(X x) { return expl(x); }

// ---- the song (description sections 2, 3)

struct Cell {
  uint8_t note = 0, vol = 0xFF, fx[2] = {0, 0}, par[2] = {0, 0};
};

struct Instrument {
  uint8_t wave = 0, link = 0xFF;
  float linkAmount = 0, level = 1, startVol = 1, volFactor = 1, pitchFactor = 1;
  float attack = 0, decay = 0, sustain = 1, release = 0;
  uint8_t sustainFlag = 1;
};

struct Song {
  bool old = false;
  int channels = 0;
  Instrument ins[16];
  uint8_t mute[16] = {}, active[16] = {};
  std::string names[16];
  int orderLength = 0;
  uint8_t order[256];
  bool present[256] = {};
  std::vector<Cell> cells;   // 256 patterns x 64 rows x 16 channels
  uint8_t restart = 0;
  std::string title, author, message;
  Cell& at(int p, int r, int c) { return cells[(size_t(p) * 64 + size_t(r)) * 16 + size_t(c)]; }
};

struct Reader {
  const std::vector<uint8_t>& d;
  size_t pos = 0;
  explicit Reader(const std::vector<uint8_t>& data) : d(data) {}
  uint8_t u8() { return pos < d.size() ? d[pos++] : (pos++, 0); }   // zeros past the end
  uint32_t u32() {
    uint32_t v = u8();
    v |= uint32_t(u8()) << 8;
    v |= uint32_t(u8()) << 16;
    return v | uint32_t(u8()) << 24;
  }
  float f32() {
    const uint32_t u = u32();
    return fbits(u);
  }
  bool past() const { return pos > d.size(); }
  size_t left() const { return pos < d.size() ? d.size() - pos : 0; }
};

// Settings that are added to or multiplied by every sample: a value outside these
// bounds is not a song's (the real ones stay within a few units), and could only make
// infinities.
bool sane(float v, float lim) { return std::isfinite(v) && std::fabs(v) <= lim; }

std::string load(const std::vector<uint8_t>& data, Song& song) {
  if (data.size() < 8) return "not a T0AST song (too short)";
  if (!std::memcmp(data.data(), "T0AST\x01\x00\x01", 8)) song.old = false;
  else if (!std::memcmp(data.data(), "T0AST0OK", 8)) song.old = true;
  else return "not a T0AST song (no T0AST signature)";
  if (data.size() < 8 + 560 + (song.old ? 0 : 272) + 5) return "the file is cut short";
  Reader r(data);
  r.pos = 8;
  for (auto& i : song.ins) i.attack = F_001, i.release = F_M001;
  for (int c = 0; c < 16; ++c) song.ins[c].wave = r.u8();
  for (int c = 0; c < 16; ++c) song.ins[c].link = r.u8();
  for (int c = 0; c < 16; ++c) song.ins[c].linkAmount = r.f32();
  for (int c = 0; c < 16; ++c) song.ins[c].level = r.f32();
  for (int c = 0; c < 16; ++c) song.ins[c].startVol = r.f32();
  for (int c = 0; c < 16; ++c) song.ins[c].volFactor = r.f32();
  for (int c = 0; c < 16; ++c) song.ins[c].pitchFactor = r.f32();
  r.pos += 16 + 3 * 16 * 4;   // panning (ignored) and the demo player's offsets
  if (!song.old) {
    for (int c = 0; c < 16; ++c) song.ins[c].attack = r.f32();
    for (int c = 0; c < 16; ++c) song.ins[c].decay = r.f32();
    for (int c = 0; c < 16; ++c) song.ins[c].sustain = r.f32();
    for (int c = 0; c < 16; ++c) song.ins[c].sustainFlag = r.u8();
    for (int c = 0; c < 16; ++c) song.ins[c].release = r.f32();
  }
  for (const auto& i : song.ins) {
    if (!sane(i.linkAmount, 1e6f) || !sane(i.level, 1e6f) || !sane(i.startVol, 1e6f) ||
        !sane(i.volFactor, 2.0f) || !sane(i.pitchFactor, 2.0f) || !sane(i.attack, 2.0f) ||
        !sane(i.decay, 2.0f) || !sane(i.sustain, 2.0f) || !sane(i.release, 2.0f))
      return "an instrument has an impossible setting";
  }
  r.pos += 4;   // row highlights, drum mode, chip mode
  song.channels = r.u8();
  if (song.channels > 16) return "more than 16 channels";
  for (int c = 0; c < 16; ++c) song.active[c] = 1;
  for (int c = 0; c < song.channels; ++c) {
    if (r.u8()) {
      r.u8();
      song.mute[c] = r.u8();
      song.active[c] = r.u8();
    }
  }
  for (int c = 0; c < song.channels; ++c) {
    if (r.u8()) {
      const int n = r.u8();
      std::string name;
      for (int k = 0; k < 24; ++k) {
        const char ch = char(r.u8());
        if (k < n) name += ch;
      }
      song.names[c] = name;
    }
  }
  song.orderLength = int(r.u8());
  song.orderLength |= int(r.u8()) << 8;
  if (song.orderLength > 256) return "the order list is longer than 256";
  std::memset(song.order, 0xFF, sizeof song.order);
  song.order[0] = 0;
  for (int k = 0; k < song.orderLength; ++k) song.order[k] = r.u8();
  if (r.past()) return "the file is cut short";
  song.cells.assign(size_t(256) * 64 * 16, Cell());
  for (int p = 0; p < 256; ++p) {
    if (!r.u8()) continue;
    song.present[p] = true;
    for (int ch = 0; ch < song.channels; ++ch) {
      Cell c;
      int j = 0;
      while (j < 64) {
        if (r.past()) return "the file is cut short (in the patterns)";
        const uint8_t b = r.u8();
        if (b & 0x80) {
          const int n = b & 0x7F;
          if (n == 0) {
            c = Cell();
            song.at(p, j++, ch) = c;
          } else {
            const int k = r.u8();
            int q = (j - k) & 0xFF;
            if (q >= 64) return "a pattern copies rows from outside the pattern";
            for (int m = 0; m < n && j < 64; ++m) {
              if (q >= 64) return "a pattern copies rows from outside the pattern";
              c = song.at(p, q, ch);
              song.at(p, j++, ch) = c;
              ++q;
            }
          }
        } else {
          if (b & 1) c.note = r.u8();
          if (b & 2) c.vol = r.u8();
          if (b & 4) c.fx[0] = r.u8();
          if (b & 8) c.par[0] = r.u8();
          if (b & 16) c.fx[1] = r.u8();
          if (b & 32) c.par[1] = r.u8();
          if (b & 0x40) {
            const int n = r.u8();
            for (int m = 0; m < n && j < 64; ++m) song.at(p, j++, ch) = c;
          } else {
            song.at(p, j++, ch) = c;
          }
        }
      }
    }
  }
  if (r.past()) return "the file is cut short (in the patterns)";
  song.restart = r.u8();
  std::string* texts[3] = {&song.title, &song.author, &song.message};
  for (std::string* t : texts) {
    const uint32_t n = r.u32();
    if (n > r.left()) return "a text runs past the end of the file";
    t->assign(reinterpret_cast<const char*>(data.data()) + r.pos, n);
    r.pos += n;
  }
  return "";
}

// ---- the engine (description sections 4 to 13)

const double VOWEL[5][11] = {
    {8.11044e-06, 8.943665402, -36.83889529, 92.01697887, -154.337906, 181.6233289,
     -151.8651235, 89.09614114, -35.10298511, 8.388101016, -0.923313471},
    {4.36215e-06, 8.90438318, -36.55179099, 91.05750846, -152.422234, 179.1170248,
     -149.6496211, 87.78352223, -34.60687431, 8.282228154, -0.914150747},
    {3.33819e-06, 8.893102966, -36.49532826, 90.96543286, -152.4545478, 179.4835618,
     -150.315433, 88.43409371, -34.98612086, 8.407803364, -0.932568035},
    {1.13572e-06, 8.994734087, -37.2084849, 93.22900521, -156.6929844, 184.596544,
     -154.3755513, 90.49663749, -35.58964535, 8.478996281, -0.929252233},
    {4.09431e-07, 8.997322763, -37.20218544, 93.11385476, -156.2530937, 183.7080141,
     -153.2631681, 89.59539726, -35.12454591, 8.338655623, -0.910251753},
};

const int RETRIG_MUL[16] = {0, 0, 0, 0, 0, 0, 10, 8, 0, 0, 0, 0, 0, 0, 24, 32};
const int RETRIG_ADD[16] = {0, -1, -2, -4, -8, -16, 0, 0, 0, 1, 2, 4, 8, 16, 0, 0};

struct Tb303 {
  float x = 0, inc = 0, inc0 = 0;
  int64_t gpos = 0;
  int32_t glen = 1;
  float a1 = 0, b1 = 0, c0 = 0, e0 = 0, e1 = 0, y1 = 0, y2 = 0, amp = 0, amp0 = 0;
  int stage = 2;     // 0 attack, 1 decay (never entered), 2 idle
  int counter = 0;
  float rho = 1;
  int wave = 0;
  float cutoff = 0, reso = 0, envMod = 0, envDecay = 0, glideTime = 0, accentAmount = 0;
  bool accent = false, glide = false, keyOff = false;

  void constants() {
    e1 = s(xexp(6.109L + 1.5876L * X(envMod) + 2.1553L * X(cutoff) - 1.2L * (1 - X(reso))));
    e0 = s(xexp(5.613L - 0.8L * X(envMod) + 2.1553L * X(cutoff) - 0.7696L * (1 - X(reso))));
    e0 = s(X(e0) * PI / 44100);
    e1 = s(X(e1) * PI / 44100);
    e1 = s(X(e1) - X(e0));
    counter = 64;
  }
  void setReso(float v) {
    reso = v;
    rho = s(xexp(-1.2L + 3.455L * X(reso)));
    constants();
  }
  void init() {
    *this = Tb303();
    inc = F0_S;
    amp0 = 0.5f;
    stage = 2;
    rho = 1;
    accentAmount = 0.5f;
    glideTime = F_01;
    cutoff = F_09;
    constants();
    setReso(F_01);
    envMod = 1;
    constants();
    envDecay = F_01;
    glen = 1;
    gpos = 0;
    counter = 64;
  }
  void noteOn() {
    inc0 = inc;
    glen = itrunc(44100 * X(glideTime));
    if (glen == 0) glen = 1;
    gpos = glide ? 0 : int64_t(glen) - 1;
    stage = 0;
    c0 = e1;
    counter = 64;
    amp0 = accent ? s(HALF + X(accentAmount) * HALF) : 0.5f;
  }
  void off() {
    if (keyOff) {
      amp = 0;
      stage = 2;
    }
  }
  float sample(float sigma, float o) {
    if (counter >= 64) {
      const float W = s(X(e0) + X(c0));
      const float K = s(xexp(-X(W) / X(rho)));
      a1 = s(2 * fcos(X(W) * 2) * X(K));
      b1 = s(-X(K) * X(K));
      c0 = s(1 - X(a1) - X(b1));
      counter = 0;
    }
    float q = x;
    if (o != 0) {
      q = s(X(x) + X(o));
      if (q > 0.5f) {
        if (q < 16777216.0f) {
          // repeated "q - 1" while q > 1/2: each step is exact below 2^24
          const double n = std::ceil(double(q) - 0.5);
          q = static_cast<float>(double(q) - n);
        } else {
          for (int k = 0; k < 64 && q > 0.5f; ++k) q = s(X(q) - 1);
          if (q > 0.5f) q = 0;   // only a damaged song gets here
        }
      }
    }
    switch (wave) {
      case 1: q = std::signbit(q) ? -0.5f : 0.5f; break;
      case 2: q = s(fsin(X(q) * 4 * PI) * HALF); break;
      case 3: q = s((std::fabs((X(q) * 2 - HALF) * 4) - 1) * HALF); break;
      case 4: q = s((X(q) * 2 - HALF) * 2 * 1 * HALF); break;
      case 5: q = s((X(q) * 2 - HALF) * 2 * -1 * HALF); break;
      default: break;
    }
    const float y = s((X(a1) * X(y1) + X(b1) * X(y2)) + X(c0) * X(q) * X(amp));
    y2 = y1;
    y1 = y;
    ++counter;
    ++gpos;
    const float g = s(X(gpos) / X(glen));
    float k;
    if (g < 1) {
      k = s(X(inc0) * (1 - X(g)) + X(g) * X(sigma));
    } else {
      inc = sigma;
      k = inc;
    }
    x = s(X(x) + X(k));
    if (x > 0.5f) x = s(X(x) - 1);
    if (stage == 0) amp = s(X(amp) + (X(amp0) - X(amp)) * X(TB_ATTACK));
    return y;
  }
};

struct Voice {
  Instrument ins;
  uint8_t wave = 0, note = 255;
  float step = 0, phase = 0, pitchFactor = 0, runVol = 0, volFactor = 0, gain = 0;
  float env = 0;
  int envStage = 0;
  float feedback = 0, feedbackAmount = 0;
  // filter
  float cl = 0, ch = 0, rl = 0, rh = 0, fl = 0, fh = 0, d1 = 0, d2 = 0, d3 = 0, d4 = 0;
  int finetune = 0, harmony = 0;
  uint8_t distortion = 0;
  bool clip = false;
  int vowel = 0;
  double vmem[5][10] = {};
  bool delayOn = false;
  int32_t delayTime = 0;
  float delayFb = 0;
  uint32_t rd = 0, wr = 0;
  uint8_t chorus = 0, pan = 0x80;
  float link[8] = {};
  bool linkSet[8] = {};
  float lastL = 0, lastR = 0;
  Tb303 tb;

  void feedbacks() {
    fl = s(X(rl) + X(rl) / (1 - X(cl)) + EPS);
    fh = s(X(rh) + X(rh) / (1 - X(cl)) + EPS);
  }
};

struct ColState {
  uint8_t fx = 0, par = 0;
  int order = 0, pattern = 0, row = 0;
  uint8_t arp[3] = {};
  int arpPos = 0;
  int tBase = 0, tSpeed = 0, tDepth = 0, tPos = 0, tKind = 0;
  int retrig = 0;
  int loopStart = 0, loopCount = 0;
  bool loopActive = false;
};

struct ChanState {
  uint8_t note = 0, vol = 0;
  ColState col[2];
};

class Engine {
 public:
  Engine(Song& song, bool synth) : song_(song), synth_(synth) {
    if (synth_) lines_.assign(size_t(16) * 131072, 0.0f);
    noise_ = 0x12345678u;
    for (int c = 0; c < 16; ++c) {
      Voice& v = voice_[c];
      v = Voice();
      v.ins = song.ins[c];
      v.note = 255;
      v.link[1] = v.link[2] = 1;
      v.wave = v.ins.wave;
      v.cl = F_255_256;
      v.feedbacks();
      v.tb.init();
    }
    tickLen_ = 28224000 / (bpm_ * 256);
  }

  bool ended() const { return end_ && tick_ == 0; }

  // One block of 2048 frames (the export's unit); out may be null (only the time runs).
  void block(float* out) {
    int pos = 0;
    while (pos < BLOCK) {
      if (left_ == 0) tickStep();
      const int n = left_ < BLOCK - pos ? left_ : BLOCK - pos;
      if (synth_) {
        for (int k = 0; k < n; ++k) sample(out + 2 * (pos + k));
      }
      left_ -= n;
      pos += n;
    }
  }

 private:
  Song& song_;
  bool synth_;
  std::vector<float> lines_;
  Voice voice_[16];
  ChanState chan_[16];
  uint32_t noise_;
  float tailL_ = 0, tailR_ = 0;
  // sequencer
  int speed_ = 6, bpm_ = 125, tick_ = 6, frameDelay_ = 0, rowDelay_ = 0;
  bool breakPending_ = false, jumpPending_ = false, loopJump_ = false, end_ = false;
  int breakRow_ = 0, jumpTarget_ = 0;
  int curOrder_ = 0, curRow_ = 0, nextOrder_ = 0, nextRow_ = 0;
  uint8_t played_[256] = {};
  int tickLen_ = 0, left_ = 0, delayUnit_ = 0;

  // -- time
  void tickStep() {
    delayUnit_ = tickLen_;
    tick_ = (tick_ + 1) & 255;
    if (tick_ >= speed_ * (rowDelay_ + 1) + frameDelay_) rowStep();
    for (int c = 0; c < 16; ++c)
      for (int k = 0; k < 2; ++k) tickEffect(c, k);
    tickLen_ = 28224000 / (bpm_ * 256);
    left_ = tickLen_;
  }

  void rowStep() {
    tick_ = 0;
    frameDelay_ = 0;
    rowDelay_ = 0;
    loopJump_ = false;
    if (curOrder_ != nextOrder_) {
      curOrder_ = nextOrder_;
      if (played_[curOrder_]) end_ = true;
      ++played_[curOrder_];
    }
    curRow_ = nextRow_;
    int p = song_.order[curOrder_];
    while (p == 0xFE && curOrder_ < 255) p = song_.order[++curOrder_];
    if (p == 0xFF) {
      curOrder_ = song_.restart;
      p = song_.order[curOrder_];
      if (p == 0xFF) {
        curOrder_ = 0;
        p = song_.order[0];
      }
      end_ = true;
    }
    for (int c = 0; c < 16; ++c) {
      const Cell& cell = song_.at(p, curRow_, c);
      for (int k = 0; k < 2; ++k) {
        ColState& col = chan_[c].col[k];
        col.fx = cell.fx[k];
        col.par = cell.par[k];
        col.order = curOrder_;
        col.pattern = p;
        col.row = curRow_;
      }
      playRow(c, cell);
    }
    if (breakPending_ || jumpPending_) {
      if (breakPending_) {
        nextRow_ = breakRow_ & 63;
        nextOrder_ = (curOrder_ + 1) & 255;
        breakPending_ = false;
      } else {
        nextRow_ = 0;
      }
      if (jumpPending_) {
        nextOrder_ = jumpTarget_;
        jumpPending_ = false;
      }
    } else {
      nextRow_ = (curRow_ + 1) & 63;
      if (nextRow_ == 0) nextOrder_ = (curOrder_ + 1) & 255;
    }
  }

  // the cell's note and volume, then the row effects of both columns
  void playRow(int c, const Cell& cell) {
    ChanState& ch = chan_[c];
    Voice& v = voice_[c];
    if (song_.mute[c]) {
      keyOff(c);
      v.runVol = 0;
      ch.vol = 0;
    } else if (song_.active[c]) {
      if (cell.note == 254 || cell.note == 255) {
        ch.note = cell.note;
        keyOff(c);
      } else if (cell.note != 0) {
        ch.note = cell.note;
        noteCommand(c, cell.note - 1);
        const int vol = cell.vol > 64 ? 64 : cell.vol;
        ch.vol = uint8_t(vol);
        setVolume(c, vol);
      } else if (cell.vol <= 64) {
        ch.vol = cell.vol;
        setVolume(c, cell.vol);
      }
    }
    for (int k = 0; k < 2; ++k) rowEffect(c, k, cell.fx[k], cell.par[k]);
  }

  // -- notes
  void setVolume(int c, int v) { voice_[c].gain = s(X(v) / 64); }

  void declick(int c) {
    Voice& v = voice_[c];
    tailL_ = s(X(tailL_) + X(v.lastL));
    tailR_ = s(X(tailR_) + X(v.lastR));
    v.lastL = v.lastR = 0;
  }

  void setPitch(int c, int n) {
    Voice& v = voice_[c];
    X e = X(n - 45) + X(v.finetune) / 128;
    if (v.harmony != 0) e = e + 1 / X(v.harmony);
    v.step = s(pow2(e / 12) * F0);
  }

  void recomputePitch(int c, int n) {
    Voice& v = voice_[c];
    v.note = uint8_t(n);
    setPitch(c, n);
    v.tb.noteOn();
  }

  void startVoice(int c) {   // note-on steps 4 to 6
    Voice& v = voice_[c];
    v.pitchFactor = v.ins.pitchFactor;
    v.runVol = v.ins.startVol;
    v.volFactor = v.ins.volFactor;
    v.envStage = 1;
    v.env = v.ins.attack == 0 ? 1.0f : 0.0f;
    v.tb.noteOn();
  }

  void noteOn(int c, int n) {
    declick(c);
    voice_[c].note = uint8_t(n);
    setPitch(c, n);
    startVoice(c);
  }

  void keyOff(int c) {
    voice_[c].envStage = 4;
    voice_[c].tb.off();
  }

  void noteCommand(int c, int n) {   // a synth note: below 128 a note-on, 128 a key-off
    if (n < 128) noteOn(c, n);
    else if (n == 128) keyOff(c);
  }

  // -- effects
  void retrigger(int c, ColState& col, uint8_t x) {
    ChanState& ch = chan_[c];
    const int hi = x >> 4, lo = x & 15;
    const int r = lo ? lo : 1;
    const bool go = col.retrig != 0 && col.retrig % r == 0;
    ++col.retrig;
    if (go) {
      if (hi) {
        int v = ch.vol;
        if (RETRIG_MUL[hi]) v = v * RETRIG_MUL[hi] / 16;
        else v += 4 * RETRIG_ADD[hi];
        if (v < 0) v = 0;
        if (v > 63) v = 63;
        ch.vol = uint8_t(v);
        noteCommand(c, int(ch.note) - 1);
        setVolume(c, v);
      } else {
        noteCommand(c, int(ch.note) - 1);
      }
    }
    col.retrig &= 15;
  }

  void patternLoop(ColState& col, int lo) {
    if (loopJump_) return;
    if (lo == 0) {
      if (!col.loopActive) col.loopStart = curRow_;
      return;
    }
    if (col.loopActive) {
      if (col.loopCount == 0) {
        col.loopActive = false;
        return;
      }
      --col.loopCount;
    } else {
      col.loopCount = lo - 1;
      col.loopActive = true;
    }
    jumpPending_ = true;
    jumpTarget_ = curOrder_;
    breakPending_ = true;
    breakRow_ = col.loopStart;
    loopJump_ = true;
  }

  void copyInstrument(int c, int from) {
    Voice& d = voice_[c];
    const Voice& f = voice_[from];
    d.wave = f.wave;
    d.ins = f.ins;   // waveform, volumes, factors, level, link amount and byte, envelope
    d.cl = f.cl, d.ch = f.ch, d.rl = f.rl, d.rh = f.rh, d.fl = f.fl, d.fh = f.fh;
    d.d1 = f.d1, d.d2 = f.d2, d.d3 = f.d3, d.d4 = f.d4;
    d.feedbacks();
    d.vowel = f.vowel;
    d.finetune = f.finetune;
    d.harmony = f.harmony;
    if (d.note < 128) recomputePitch(c, d.note);
  }

  void rowEffect(int c, int k, uint8_t fx, uint8_t x) {
    Voice& v = voice_[c];
    ChanState& ch = chan_[c];
    ColState& col = ch.col[k];
    const int hi = x >> 4, lo = x & 15;
    switch (fx) {
      case 1: speed_ = (x + 1) & 255; break;
      case 2: jumpPending_ = true; jumpTarget_ = x; break;
      case 3: breakPending_ = true; breakRow_ = x; break;
      case 8: v.harmony = x - 128; recomputePitch(c, v.note); break;
      case 9:
        col.tBase = ch.vol;
        if (hi) col.tSpeed = hi;
        if (lo) col.tDepth = lo;
        if (!(col.tKind & 4)) col.tPos = 0;
        break;
      case 10: {
        const int m = ch.note;
        col.arpPos = 0;
        col.arp[0] = uint8_t(m);
        col.arp[1] = uint8_t(m + hi < 128 ? m + hi : 127);
        col.arp[2] = uint8_t(m + lo < 128 ? m + lo : 127);
        break;
      }
      case 11: v.feedbackAmount = s(X(x) / 255); break;
      case 12: v.finetune = x - 128; recomputePitch(c, v.note); break;
      case 13: setVolume(c, x < 63 ? x : 63); break;
      case 14: v.distortion = x; break;
      case 16: v.phase = s(X(x) * D256 * 2 * PI); break;
      case 17: retrigger(c, col, x); break;
      case 18:
        declick(c);
        v.note = 0;
        v.step = s(X(44100) / X(x + 1));
        startVoice(c);
        break;
      case 19:
        switch (hi) {
          case 0: if (lo <= 5) v.vowel = lo; break;
          case 1: if (lo <= 1) v.clip = lo != 0; break;
          case 2:
            if (lo <= 1) v.delayOn = lo != 0;
            else if (lo == 2 && synth_) std::fill_n(&lines_[size_t(c) * 131072], 131072, 0.0f);
            break;
          case 4: col.tKind = lo & 4; break;
          case 6: frameDelay_ = lo; break;
          case 0xA: v.wave = uint8_t(lo); break;
          case 0xB: patternLoop(col, lo); break;
          case 0xD: if (tick_ != lo) keyOff(c); break;
          case 0xE: rowDelay_ = lo; break;
          case 0xF: copyInstrument(c, lo); break;
          default: break;
        }
        break;
      case 20: bpm_ = x ? x : 1; break;
      case 21: v.chorus = x; break;
      case 22: v.rl = s(X(x) * D256); v.feedbacks(); break;
      case 23: v.rh = s(X(x) * D256); v.feedbacks(); break;
      case 24: v.pan = x; break;
      case 25: v.cl = s(X(x) * D256); v.feedbacks(); break;
      case 26: v.ch = s(X(x) * D256); v.feedbacks(); break;
      case 28:
        switch (hi) {
          case 0: v.tb.wave = lo; break;
          case 1: if (lo <= 1) v.tb.accent = lo != 0; break;
          case 2: if (lo <= 1) v.tb.glide = lo != 0; break;
          case 3: if (lo <= 1) v.tb.keyOff = lo != 0; break;
          default: break;
        }
        break;
      case 29: v.tb.cutoff = s(X(x) / 255); v.tb.constants(); break;
      case 30: v.tb.setReso(s(X(x) / 255)); break;
      case 31: v.tb.envMod = s(X(x) / 255); v.tb.constants(); break;
      case 32: v.tb.envDecay = s(X(x) / 255); break;
      case 33: v.tb.glideTime = s(X(x) / 255); break;
      case 34: v.tb.accentAmount = s(X(x) / 255); break;
      case 35: v.delayFb = s(X(x) / 255); break;
      case 36:
        if (x <= 0xEF) v.delayTime = delayUnit_ * x;
        else v.delayTime = v.delayTime / (x - 127);
        break;
      default: break;
    }
  }

  void tickEffect(int c, int k) {
    Voice& v = voice_[c];
    ChanState& ch = chan_[c];
    ColState& col = ch.col[k];
    const uint8_t x = col.par;
    const int hi = x >> 4, lo = x & 15;
    switch (col.fx) {
      case 4: {
        int vol = ch.vol;
        if (lo) vol = vol > lo ? vol - lo : 0;
        if (hi) vol = vol + hi < 64 ? vol + hi : 64;
        ch.vol = uint8_t(vol);
        setVolume(c, vol);
        break;
      }
      case 5: {
        const float kk = s(X(x) * FP);
        v.step = s(X(v.step) * (1 - X(kk)));
        break;
      }
      case 6: {
        const float kk = s(X(x) * FP);
        v.step = s(X(v.step) * (1 + X(kk)));
        break;
      }
      case 9: {
        const float w = s(X(col.tPos & 63) * D64);
        int vol = col.tBase + itrunc((X(w) - HALF) * 4 * X(col.tDepth));
        if (vol < 0) vol = 0;
        if (vol > 64) vol = 64;
        setVolume(c, vol);
        col.tPos = (col.tPos + col.tSpeed) & 63;
        break;
      }
      case 10: {
        const int a = int(col.arp[col.arpPos % 3]) - 1;
        if (a < 128) recomputePitch(c, a);
        col.arpPos = (col.arpPos + 1) % 3;
        break;
      }
      case 17: retrigger(c, col, x); break;
      case 19:
        if (hi == 0xC && tick_ != 0 && tick_ == lo) {
          ch.note = 0xFE;
          keyOff(c);
        } else if (hi == 0xD && tick_ != 0 && tick_ == lo) {
          playRow(c, song_.at(col.pattern, col.row, c));
        }
        break;
      default: break;
    }
  }

  // -- one sample (description section 9)
  float noise() {
    noise_ = noise_ * 0x524281u + 0x3133731u;
    const float f = fbits((noise_ & 0x7FFFFF) | 0x40000000);
    return s(X(f) - 3);
  }

  void sample(float* out) {
    for (auto& v : voice_)
      for (bool& b : v.linkSet) b = false;
    float lsum = 0, rsum = 0;
    for (int c = 0; c < 16; ++c) {
      Voice& v = voice_[c];
      const float o = s(X(v.link[0]) + X(v.feedback) * X(v.feedbackAmount));
      const float phi = frac(X(v.phase) + X(o));
      float w;
      switch (v.wave) {
        case 0: w = s(fsin(X(phi) * 2 * PI)); break;
        case 1: w = s(std::fabs((X(phi) - HALF) * 4) - 1); break;
        case 2: w = std::signbit(s(X(phi) - HALF)) ? -1.0f : 1.0f; break;
        case 3: w = s((X(phi) - HALF) * 2); break;
        case 4: w = s((X(phi) - HALF) * 2 * -1); break;
        case 5: w = noise(); break;
        case 6: w = v.tb.sample(v.step, o); break;
        default: w = 0; break;
      }
      w = s(X(w) + X(v.link[3]) - std::fabs(X(v.link[4])));
      w = s(X(w) * X(v.runVol) * X(v.gain) * X(v.env) + EPS);
      w = s(X(w) * X(v.link[1]) * std::fabs(X(v.link[2])));
      // filter
      v.d1 = s(X(v.d1) + X(v.cl) * (X(w) - X(v.d1) + X(v.fl) * (X(v.d1) - X(v.d2))) + EPS);
      v.d2 = s(X(v.d2) + X(v.cl) * (X(v.d1) - X(v.d2)) + EPS);
      v.d3 = s(X(v.d3) + X(v.ch) * (X(v.d2) - X(v.d3) + X(v.fh) * (X(v.d3) - X(v.d4))) + EPS);
      v.d4 = s(X(v.d4) + X(v.ch) * (X(v.d3) - X(v.d4)) + EPS);
      w = s(X(v.d2) - X(v.d4));
      v.feedback = w;
      // vowel filter, distortion, clipping, delay
      if (v.vowel >= 1 && v.vowel <= 5) {
        const double* k = VOWEL[v.vowel - 1];
        double* m = v.vmem[v.vowel - 1];
        X acc = X(k[0]) * X(w);
        for (int t = 0; t < 10; ++t) acc = acc + X(k[t + 1]) * X(m[t]);
        const float r = s(acc + EPS);
        for (int t = 9; t > 0; --t) m[t] = m[t - 1];
        m[0] = r;
        w = r;
      }
      if (v.distortion) w = s(X(w) * (1 + X(v.distortion) * HALF));
      if (v.clip) {
        if (w < -1) w = -1;
        else if (w > 1) w = 1;
      }
      if (v.delayOn) {
        float* line = &lines_[size_t(c) * 131072];
        v.rd = (v.rd + 1) & 131071;
        uint32_t r = uint32_t(v.delayTime);
        if (v.link[5] != 0)
          r += uint32_t(itrunc((1 + X(v.link[5])) * X(64 * (int(v.chorus) + 1))));
        v.wr = (v.rd + r) & 131071;
        w = s(X(w) + X(line[v.rd]) * X(v.delayFb));
        line[v.wr] = w;
      }
      // envelope
      switch (v.envStage) {
        case 1:
          if (v.env < 1) v.env = s(X(v.env) + X(v.ins.attack));
          else v.env = 1, v.envStage = 2;
          break;
        case 2:
          if (v.env > v.ins.sustain) v.env = s(X(v.env) + X(v.ins.decay));
          else v.env = v.ins.sustain, v.envStage = v.ins.sustainFlag ? 3 : 4;
          break;
        case 4:
          if (v.env > 0) v.env = s(X(v.env) + X(v.ins.release));
          else v.envStage = 0;
          break;
        default: break;
      }
      v.env = s(X(v.env) + EPS);
      if (v.env < 0) v.env = 0;
      else if (v.env > 1) v.env = 1;
      // links out
      const uint8_t b = v.ins.link;
      if (b != 0xFF) {
        Voice& t = voice_[b & 15];
        const int kind = (b & 0x7F) >> 4;
        if (!t.linkSet[kind]) {
          t.link[kind] = 0;
          t.linkSet[kind] = true;
        }
        t.link[kind] = s(X(t.link[kind]) + X(w) * X(v.ins.linkAmount));
      }
      // output
      if (b & 0x80) {
        const float val = s(X(w) * X(v.ins.level));
        const float p = s(X(v.pan) * D255 + EPS);
        const float l = s(X(val) * X(p));
        v.lastL = s(X(l) + EPS);
        lsum = s(X(lsum) + X(l));
        const float r = s(X(val) * (1 - X(p)));
        v.lastR = s(X(r) + EPS);
        rsum = s(X(rsum) + X(r));
      }
      // advance
      v.phase = frac(X(v.phase) + X(v.step));
      v.step = s(X(v.step) * X(v.pitchFactor) + EPS);
      v.runVol = s(X(v.runVol) * X(v.volFactor) + EPS);
    }
    lsum = s(X(lsum) + X(tailL_));
    tailL_ = s(X(tailL_) * LAMBDA + EPS);
    rsum = s(X(rsum) + X(tailR_));
    tailR_ = s(X(tailR_) * LAMBDA + EPS);
    out[0] = s(X(lsum) * D8);
    out[1] = s(X(rsum) * D8);
  }
};

// The engine runs with rounding toward zero; everything else with the usual rounding.
struct TowardZero {
  int was;
  TowardZero() : was(std::fegetround()) { std::fesetround(FE_TOWARDZERO); }
  ~TowardZero() { std::fesetround(was); }
};

__attribute__((noinline)) Engine* makeEngine(Song& song, bool synth) {
  TowardZero tz;
  return new Engine(song, synth);
}

__attribute__((noinline)) void runBlock(Engine& e, float* out) {
  TowardZero tz;
  e.block(out);
}

// The length in frames by the export's rule, or -1 past 30 minutes.
long long measure(Song& song) {
  Engine* e = makeEngine(song, false);
  long long frames = 0;
  while (!e->ended()) {
    if (frames >= MAX_FRAMES) {
      delete e;
      return -1;
    }
    runBlock(*e, nullptr);
    frames += BLOCK;
  }
  delete e;
  return frames;
}

// ---- output

struct Player {
  Engine* engine;
  long long left;              // frames still to make
  std::vector<float> buf = std::vector<float>(BLOCK * 2);
  int at = BLOCK;
  void frame(int16_t* f) {
    if (at == BLOCK) {
      if (left > 0) runBlock(*engine, buf.data());
      else std::fill(buf.begin(), buf.end(), 0.0f);
      left -= BLOCK;
      at = 0;
    }
    for (int c = 0; c < 2; ++c) {
      float v = buf[size_t(at) * 2 + c];
      if (!(v == v)) v = 0;              // NaN only from a damaged song
      if (v > 1) v = 1;
      if (v < -1) v = -1;
      f[c] = static_cast<int16_t>(std::lrint(double(v) * 32767.0));
    }
    ++at;
  }
};

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

void source(void* user, int16_t* frame) { static_cast<Player*>(user)->frame(frame); }

}  // namespace

int main(int argc, char** argv) {
  bool ref = false;
  int a = 1;
  if (a < argc && !std::strcmp(argv[a], "--ref")) ref = true, ++a;
  if (ref ? argc - a < 1 : argc - a < 3) {
    std::fprintf(stderr, "usage: %s <file> <track> <samplerate> [start-ms]\n"
                         "       %s --ref <file>   (44100 Hz, 32-bit float, no header)\n",
                 argv[0], argv[0]);
    return 2;
  }
  constants();
  std::vector<uint8_t> file;
  if (!readFile(argv[a], file) || file.empty()) return fail("can't read the file");
  if (file.size() > MAX_FILE) return fail("the file is too big for a T0AST song");
  Song song;
  const std::string why = load(file, song);
  if (!why.empty()) return fail(why);
  if (!ref && std::atoi(argv[a + 1]) != 0) return fail("a T0AST song has one track");
  int rate = ref ? RATE : std::atoi(argv[a + 2]);
  if (rate < 8000 || rate > 192000) rate = RATE;
  long long startMs = !ref && argc - a > 3 ? std::atoll(argv[a + 3]) : 0;
  if (startMs < 0) startMs = 0;

  const long long total = measure(song);
  if (total < 0) return fail("the song does not end within 30 minutes");

  if (!ref) {
    int patterns = 0;
    for (bool p : song.present) patterns += p;
    int muted = 0;
    for (int c = 0; c < 16; ++c) muted += song.mute[c] || !song.active[c];
    std::printf("{\"tracks\":1,\"track\":0,\"title\":");
    json(trim(latin1(song.title)));
    const std::string author = trim(latin1(song.author));
    if (!author.empty()) {
      std::printf(",\"author\":");
      json(author);
    }
    std::printf(",\"type\":\"T0AST\",\"duration_ms\":%lld,\"facts\":{", total * 1000 / RATE);
    std::printf("\"Format\":");
    json(song.old ? "T0AST0OK (the first version, no envelopes)" : "T0AST 1.0.1");
    std::printf(",\"Channels\":");
    json(std::to_string(song.channels) + (muted ? ", " + std::to_string(muted) + " muted" : ""));
    std::printf(",\"Patterns\":");
    json(std::to_string(patterns) + "; the order list has " + std::to_string(song.orderLength) + " positions");
    std::printf(",\"Synthesizer\":");
    json(T0AST_X87 ? "T0AST's, 44100 Hz, x87 arithmetic as the tracker's"
                   : "T0AST's, 44100 Hz (close to the tracker's, not bit-exact on this CPU)");
    std::printf("},\"comment\":");
    json(latin1(song.message));
    std::printf(",\"names\":[");
    int last = 15;
    while (last >= 0 && trim(latin1(song.names[last])).empty()) --last;
    for (int c = 0; c <= last; ++c) {
      if (c) std::putchar(',');
      json(trim(latin1(song.names[c])));
    }
    std::printf("]}\n");
    std::fflush(stdout);
  }

  Player player{makeEngine(song, true), total};
  if (ref) {
    while (player.left > 0) {
      runBlock(*player.engine, player.buf.data());
      player.left -= BLOCK;
      if (std::fwrite(player.buf.data(), 8, BLOCK, stdout) != size_t(BLOCK)) return 0;
    }
    std::fflush(stdout);
    return 0;
  }

  // skip to the start, whole blocks quickly, then frames
  long long skip = startMs >= total * 1000 / RATE ? total : startMs * RATE / 1000;
  const long long out = total - skip;
  while (skip >= BLOCK) {
    runBlock(*player.engine, player.buf.data());
    player.left -= BLOCK;
    skip -= BLOCK;
  }
  int16_t tmp[2];
  for (long long i = 0; i < skip; ++i) player.frame(tmp);

  std::vector<int16_t> buf(4096 * 2);
  if (rate == RATE) {
    long long n_left = out;
    while (n_left > 0) {
      const size_t n = n_left > 4096 ? 4096 : size_t(n_left);
      for (size_t i = 0; i < n; ++i) player.frame(&buf[2 * i]);
      if (std::fwrite(buf.data(), 4, n, stdout) != n) return 0;
      n_left -= (long long)n;
    }
  } else {
    Resampler rs(RATE, rate, source, &player);
    long long n_left = (long long)((unsigned long long)out * (unsigned long long)rate / RATE);
    while (n_left > 0) {
      const size_t n = n_left > 4096 ? 4096 : size_t(n_left);
      for (size_t i = 0; i < n; ++i) rs.frame(&buf[2 * i]);
      if (std::fwrite(buf.data(), 4, n, stdout) != n) return 0;
      n_left -= (long long)n;
    }
  }
  std::fflush(stdout);
  return 0;
}
