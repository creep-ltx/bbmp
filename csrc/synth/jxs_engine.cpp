// SPDX-License-Identifier: GPL-3.0-or-later
//
// JayTrax (.jxs), Reinier van Vliet's synthesizer tracker for Windows (also known as
// Syntrax), played by a replayer written for this player from a description of how the
// format and its player behave (a clean-room rewrite: no code of the tracker, of its
// CrossX engine or of the earlier replayer was read to write it).
//
// An instrument is 16 small waves of 256 values that four "wave effects" (filters,
// morphing, distortion, noise, ...) rewrite on every tick, played as a wave or with a
// 16-bit sample; envelopes are waves too. Each channel has its own order list. The
// mix runs at 44100 Hz in integers (the resonance filters in doubles), with an echo
// and a short cross-fade at every tick against clicks.
//
// Two behaviours: by default the correct one (each track starts from the song as it
// was loaded, the dyna-morph effect has its sine, the random numbers start afresh and
// the cross-fade starts from silence). BBMP_JXS_TODAY=1 plays as the earlier replayer
// did (the silent pass that measures the length leaves instrument changes, tempo and
// the random numbers behind, and the dyna-morph sine is missing): only for checking
// this replayer against that one's renders.
//
// Arithmetic wraps at 32 bits where the original's does (build with -fwrapv), and the
// doubles must be computed step by step (-ffp-contract=off).

#include "engine.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr int kRate = 44100;
constexpr double kPi = 3.14159265359;
constexpr int kHeader = 52;
constexpr int kSubsong = 16564;
constexpr int kInstr = 8712;
constexpr long long kLongest = 3600LL * kRate;   // the length pass gives up here
constexpr int kPlayRequest = 4096;
constexpr int kPassRequest = 16384;

const char* const kNot = "not a JayTrax song";
const char* const kHeaderBad = "the header of the song makes no sense";
const char* const kCut = "the song is cut short or damaged";
const char* const kDamaged = "the song is damaged";

int32_t le32(const uint8_t* p) {
  return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                              static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24);
}
int16_t le16(const uint8_t* p) { return static_cast<int16_t>(p[0] | p[1] << 8); }

int16_t s16(int32_t x) { return static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(x))); }
uint16_t u16(int32_t x) { return static_cast<uint16_t>(static_cast<uint32_t>(x)); }

// C division (toward zero); the cases the machine can't divide give 0 and the wrap
int32_t tdiv(int32_t a, int32_t b) {
  if (b == 0) return 0;
  if (b == -1) return static_cast<int32_t>(0u - static_cast<uint32_t>(a));
  return a / b;
}

// a double to int as the x86 does it (truncated; out of range or NaN is INT_MIN)
int32_t trunc32(double x) {
  if (!(x > -2147483649.0 && x < 2147483648.0)) return INT32_MIN;
  return static_cast<int32_t>(x);
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// glibc's rand() (the TYPE_3 additive generator), so that the noise effect is the same
struct Rand {
  uint32_t r[31];
  int i;
  void seed() {
    int32_t v = 1;
    r[0] = 1;
    for (int k = 1; k < 31; ++k) {
      v = static_cast<int32_t>((16807LL * v) % 2147483647LL);
      r[k] = static_cast<uint32_t>(v);
    }
    i = 31;
    for (int k = 0; k < 313; ++k) next();   // the first result is r_344
  }
  uint32_t step() {   // r_i = r_{i-31} + r_{i-3}, for i >= 34; r_31..33 = r_0..2
    uint32_t v;
    if (i < 34) v = r[i % 31];
    else v = r[i % 31] + r[(i - 3) % 31];
    r[i % 31] = v;
    ++i;
    if (i >= 34 + 31 * 1000000) i -= 31 * 1000000;   // keeps i's residues, never < 34
    return v;
  }
  int32_t next() { return static_cast<int32_t>(step() >> 1); }
};

struct Slot {
  int32_t dest, src1, src2, osc, var1, var2, speed, oscspeed, type;
  int8_t oscflag, reset;
};

// what scripts 2..73 may change, kept apart so that it can be put back
struct Settings {
  int wave, wavelen, volume, volwave, volspeed, volloop, finetune;
  int pitchwave, pitchspeed, pitchloop, pitchdelay, arp;
  uint8_t resetwave[16];
  int panwave, panspeed, panloop;
  Slot slot[4];
};

struct Instr {
  Settings set;
  int sharing, loops, bidi;
  int32_t start, loop, end;
  bool has_sample = false;
  std::vector<int16_t> sample;
  int16_t waves[4096];
};

struct SlotState {
  int32_t osc, cnt1;
  double y1, y2;
  int32_t hp, bp, lp;
};

struct Channel {
  int instr;                   // 0 none, else 1..I
  int16_t w[4097];             // 16 working waves; w[4096] stays 0
  int pos, row;
  bool rowmute;
  int volpos, panpos, pitchpos, arpstep, pitchdelay;
  int note, bendamt, bendtarget, bendspeed, bent;
  const Instr* sample;         // whose sample plays, or none
  int64_t x, loopp, endp;
  bool loops, bidi, back, stopped;
  uint16_t phase;              // kept for the whole playback
  SlotState st[4];
  int volval, panval, freq;
  // the settings of the piece being mixed
  bool active, smode;
  int step, L, R, EL, ER, base, mask;
};

struct Song {
  int version, P, S, I;
  std::vector<uint8_t> subsongs;   // S * kSubsong
  std::vector<uint8_t> patterns;   // P * 320
  std::vector<Instr> instr;
  int8_t arp[256];
};

struct State {
  Song song;
  bool today = false;
  int16_t sine[256];
  // the track
  const uint8_t* sub = nullptr;
  int N = 0, echodelay = 1, amp = 100;
  int startbar, startstep, endbar, endstep, loopbar, loopstep, loopflag;
  uint8_t mute0[16];
  uint8_t echoamt[16];
  int16_t orderpat[16][256], orderlen[16][256];
  // what scripts change
  std::vector<Settings> sets;
  int bpm, groove;
  Rand rnd;
  // playback
  Channel ch[16];
  int T = 2200;
  int C = 0;
  int rowcount = 0;
  int gbar = 0, gstep = 0;
  bool ended = false, frozen = false;
  bool timing_only = false;
  int16_t echoL[65536], echoR[65536];
  int e = 0;
  int16_t dbuf[200];
  int dk = 0;
  int freq[16][128];
  // output of the engine's requests
  float buf[2 * kPlayRequest];
  int buf_n = 0, buf_pos = 0;
  bool done = false;
};

State* g = nullptr;

void release() {
  delete g;
  g = nullptr;
}

int fail(synth_info* info, const char* why) {
  std::snprintf(info->error, sizeof(info->error), "%s", why);
  return 0;
}

void copy_name(char* out, size_t room, const uint8_t* in, size_t n) {
  size_t k = 0;
  while (k < n && k + 1 < room && in[k]) {
    out[k] = static_cast<char>(in[k]);
    ++k;
  }
  out[k] = 0;
}

int tick_length(int bpm, int old) {
  if (bpm == 0) return old;
  return static_cast<int>(std::floor(82687.5 / bpm));
}

// ---- order lists ----

// §6.1: every channel one step before the start; false when the start can't be found
bool place_start(State& s) {
  const long long X = 64LL * s.startbar + s.startstep - 1;
  for (int c = 0; c < 16; ++c) {
    long long S = 0, prev = 0;
    int k = 0;
    for (; k < 256; ++k) {
      if (S >= X) break;
      prev = S;
      S += s.orderlen[c][k];
    }
    if (k == 256) return false;
    Channel& h = s.ch[c];
    h.pos = (k != X) ? k - 1 : k;
    h.row = static_cast<int>((X - prev) & 63);
    h.rowmute = s.mute0[c] == 1;
  }
  return true;
}

// §6.2: the loop position, on all 16 channels; false when it can't be found
bool place_loop(State& s) {
  const long long Y = 64LL * s.loopbar + s.loopstep;
  for (int c = 0; c < 16; ++c) {
    long long S = 0, prev = 0;
    int k = 0;
    for (; k < 256; ++k) {
      if (S > Y) break;
      prev = S;
      S += s.orderlen[c][k];
    }
    if (k == 256) return false;
    Channel& h = s.ch[c];
    h.pos = (k != Y) ? k - 1 : k;
    h.row = static_cast<int>((Y - prev) & 63);
  }
  return true;
}

void advance(State& s, int c) {
  Channel& h = s.ch[c];
  ++h.row;
  if (h.pos == -1 || (h.pos >= 0 && h.pos <= 255 && h.row == s.orderlen[c][h.pos])) {
    h.row = 0;
    ++h.pos;
    h.rowmute = s.mute0[c] == 1;
  }
}

// ---- a row ----

void reset_slots(State& s, Channel& h) {
  const Settings& in = s.sets[h.instr - 1];
  for (int k = 0; k < 4; ++k)
    if (in.slot[k].type != 0 && in.slot[k].reset != 0) h.st[k] = SlotState{};
}

void strike(State& s, Channel& h, int note, int n) {
  if (note == 0) return;
  if (n > s.song.I) return;
  if (n == 0 && h.instr == 0) return;
  h.arpstep = h.volpos = h.panpos = h.pitchpos = 0;
  h.note = note;
  h.bent = note;
  h.bendamt = h.bendtarget = h.bendspeed = 0;
  if (n != 0) {
    const Instr& in = s.song.instr[n - 1];
    const Instr* src = in.sharing ? &s.song.instr[in.sharing - 1] : &in;
    h.sample = src->has_sample ? src : nullptr;
    h.x = static_cast<int64_t>(in.start) * 256;
    h.loopp = static_cast<int64_t>(in.loop) * 256;
    h.endp = static_cast<int64_t>(in.end) * 256;
    h.loops = in.loops != 0;
    h.bidi = in.bidi != 0;
    h.back = false;
    h.stopped = false;
    const Settings& set = s.sets[n - 1];
    h.pitchdelay = set.pitchdelay;
    if (!s.timing_only)
      for (int w = 0; w < 16; ++w)
        if (set.resetwave[w]) std::memcpy(h.w + 256 * w, in.waves + 256 * w, 512);
    h.instr = n;
  }
  reset_slots(s, h);
}

int freq_of(const State& s, int ft, int i) { return s.freq[clampi(ft, 0, 15)][clampi(i, 0, 127)]; }

void script(State& s, Channel& h, int rownote, int d, int p, int sc) {
  if (h.instr == 0) return;
  Settings& in = s.sets[h.instr - 1];
  if (sc >= 14 && sc <= 57) {
    Slot& sl = in.slot[(sc - 14) / 11];
    switch ((sc - 14) % 11) {
      case 0: sl.dest = std::min(d, 15); break;
      case 1: sl.src1 = std::min(d, 15); break;
      case 2: sl.src2 = std::min(d, 15); break;
      case 3: sl.osc = std::min(d, 15); break;
      case 4: sl.var1 = d; break;
      case 5: sl.var2 = d; break;
      case 6: sl.speed = d; break;
      case 7: sl.oscspeed = d; break;
      case 8: sl.oscflag = static_cast<int8_t>(std::min(d, 1)); break;
      case 9: sl.type = std::min(d, 17); break;
      case 10: sl.reset = static_cast<int8_t>(std::min(d, 1)); break;
    }
    return;
  }
  if (sc >= 58 && sc <= 73) {
    in.resetwave[sc - 58] = static_cast<uint8_t>(std::min(d, 1));
    return;
  }
  switch (sc) {
    case 1: {
      int a;
      if (h.bent != 0) {
        a = freq_of(s, in.finetune, h.bent);
        h.note = h.bent;
      } else {
        a = freq_of(s, in.finetune, rownote);
      }
      h.bendamt = 0;
      h.bendtarget = freq_of(s, in.finetune, d) - a;
      h.bendspeed = 20 * p;
      h.bent = d;
      break;
    }
    case 2: in.wave = std::min(d, 15); break;
    case 3: in.wavelen = d > 192 ? 256 : d > 96 ? 128 : d > 48 ? 64 : 32; break;
    case 4: in.volume = d; break;
    case 5: in.volwave = std::min(d, 15); break;
    case 6: in.volspeed = d; break;
    case 7: in.volloop = d; break;
    case 8: in.finetune = std::min(d, 15); break;
    case 9: in.pitchwave = std::min(d, 15); break;
    case 10: in.pitchspeed = d; break;
    case 11: in.pitchloop = d; break;
    case 12: in.pitchdelay = d; break;
    case 13: in.arp = std::min(d, 15); break;
    case 74:
      s.bpm = clampi(d, 10, 220);
      s.T = tick_length(s.bpm, s.T);
      break;
    case 75: s.groove = std::min(d, 3); break;
    default: break;
  }
}

void play_row(State& s, int c) {
  Channel& h = s.ch[c];
  if (h.rowmute) return;
  if (h.pos < 0 || h.pos > 255) return;
  const int pat = s.orderpat[c][h.pos];
  if (pat < 0 || pat >= s.song.P) return;
  if (h.row < 0 || h.row > 63) return;
  const uint8_t* r = s.song.patterns.data() + (static_cast<size_t>(pat) * 64 + h.row) * 5;
  strike(s, h, r[0], r[2]);
  script(s, h, r[0], r[1], static_cast<int8_t>(r[3]), r[4]);
}

// ---- the instrument update and the wave effects ----

int wave_step(int pos, int speed, int loop) {
  pos += speed;
  if (pos >= 256) {
    pos = pos - 256 + loop;
    if (pos >= 256) pos = loop;
  }
  return pos & 255;   // only a guard: the settings keep it in 0..255
}

void update(State& s, Channel& h) {
  const Settings& in = s.sets[h.instr - 1];
  int x = 0;
  if (in.volwave != 0) {
    h.volpos = wave_step(h.volpos, in.volspeed, in.volloop);
    const int v = h.w[256 * ((in.volwave - 1) & 15) + h.volpos];
    x = -std::min(tdiv(v + 32768, 6), 10000);
  }
  h.volval = (((x + 10000) * in.volume) >> 8) - 10000;
  if (in.panwave == 0) {
    h.panval = 0;
  } else {
    h.panpos = wave_step(h.panpos, in.panspeed, in.panloop);
    h.panval = h.w[256 * ((in.panwave - 1) & 15) + h.panpos] >> 7;
  }
  const int k = s.song.arp[16 * (in.arp & 15) + h.arpstep];
  h.arpstep = (h.arpstep + 1) & 15;
  int f = freq_of(s, in.finetune, h.note + k);
  if (h.pitchdelay != 0) {
    --h.pitchdelay;
  } else if (in.pitchwave != 0) {
    h.pitchpos = wave_step(h.pitchpos, in.pitchspeed, in.pitchloop);
    f -= h.w[256 * ((in.pitchwave - 1) & 15) + h.pitchpos];
  }
  h.freq = f + h.bendamt;
  if (h.bendspeed > 0 && h.bendamt < h.bendtarget) {
    h.bendamt += h.bendspeed;
    if (h.bendamt > h.bendtarget) h.bendamt = h.bendtarget;
  } else if (h.bendspeed < 0 && h.bendamt > h.bendtarget) {
    h.bendamt += h.bendspeed;
    if (h.bendamt < h.bendtarget) h.bendamt = h.bendtarget;
  }
}

void svf(int16_t* w, int D, int A, SlotState& st, int F, int Q, int out) {
  const int32_t W = trunc32(2.0 * kPi * static_cast<double>(228 + F));
  const int32_t K = tdiv(W * 8, 100);
  const int32_t q = 707 + tdiv(1000 * Q, 128);
  int32_t hp = st.hp, bp = st.bp, lp = st.lp;
  for (int i = 0; i < 256; ++i) {
    const int32_t nhp = tdiv(bp * 1000, q) - lp + w[A + i];
    const int32_t nbp = bp - tdiv(K * tdiv(hp, 100), 100);
    const int32_t nlp = lp - tdiv(K * tdiv(bp, 100), 100);
    hp = nhp;
    bp = nbp;
    lp = nlp;
    const int32_t o = out == 13 ? lp : out == 14 ? hp : bp;
    w[D + i] = static_cast<int16_t>(clampi(o, -32767, 32767));
  }
  st.hp = hp;
  st.bp = bp;
  st.lp = lp;
}

void resonance(int16_t* w, int D, int A, SlotState& st, double centre, double width, bool whistle) {
  const double tp = 2.0 * kPi;
  double b2 = std::exp(-tp * (width / 22000.0));
  const double b1 = ((-4.0 * b2) / (1.0 + b2)) * std::cos(tp * (centre / 22000.0));
  const double a0 = (1.0 - b2) * std::sqrt(1.0 - (b1 * b1) / (4.0 * b2));
  if (whistle) b2 = b2 * 1.2;
  double y1 = st.y1, y2 = st.y2;
  for (int i = 0; i < 256; ++i) {
    double o = a0 * (w[A + i] / 32768.0) - b1 * y1 - b2 * y2;
    y2 = y1;
    y1 = o;
    if (o < -0.9999) o = -0.9999;
    if (o > 0.9999) o = 0.9999;
    w[D + i] = s16(trunc32(o * 32768.0));
  }
  st.y1 = y1;
  st.y2 = y2;
}

void effects(State& s, Channel& h) {
  const Settings& in = s.sets[h.instr - 1];
  int16_t* w = h.w;
  for (int k = 0; k < 4; ++k) {
    const Slot& sl = in.slot[k];
    SlotState& st = h.st[k];
    st.osc = (st.osc + s16(sl.oscspeed)) & 255;
    if (sl.type < 1 || sl.type > 17) continue;
    const int D = 256 * (sl.dest & 15), A = 256 * (sl.src1 & 15), B = 256 * (sl.src2 & 15);
    const bool has_osc = sl.osc != 0;
    const int osc = has_osc ? w[256 * ((sl.osc - 1) & 15) + st.osc] : 0;
    const bool osc1 = has_osc && sl.oscflag == 0;   // the oscillator drives the first parameter
    const bool osc2 = has_osc && sl.oscflag != 0;   // ... or the second
    const int om = tdiv(osc + 32768, 256);
    switch (sl.type) {
      case 1: {
        int n = s16(sl.speed);
        int j = s16(st.cnt1);
        if (n > 0) {
          const int full = n;
          n %= 512;   // 512 negations give every value back, -32768 too
          for (int t = 0; t < n; ++t) {
            j = (j + 1) & 255;
            w[D + j] = s16(-w[D + j]);
          }
          if (full != n) j = (j + (full - n)) & 255;   // full - n is a multiple of 512
          st.cnt1 = j;
        } else {
          st.cnt1 = j;
        }
        break;
      }
      case 2: {
        const int n = s16(sl.speed);
        int o = 0;
        for (int i = 0; i < 256; ++i) {
          w[D + i] = s16(w[D + i] + o);
          o = s16(o + n);
        }
        break;
      }
      case 3: {
        const int n = std::min<int>(s16(sl.speed), 12);
        for (int t = 0; t < n; ++t) {
          w[D] = static_cast<int16_t>((w[A + 255] + w[A + 1]) >> 1);
          for (int i = 1; i < 255; ++i) w[D + i] = static_cast<int16_t>((w[A + i - 1] + w[A + i + 1]) >> 1);
          w[D + 255] = static_cast<int16_t>((w[A + 254] + w[A]) >> 1);
        }
        break;
      }
      case 4: {
        st.cnt1 = (st.cnt1 + s16(sl.speed)) & 255;
        int j = st.cnt1;
        for (int i = 0; i < 256; ++i) {
          w[D + i] = static_cast<int16_t>((w[A + i] + w[B + j]) >> 1);
          j = (j + 1) & 255;
        }
        break;
      }
      case 5:
      case 6: {
        double centre, width;
        if (!has_osc) {
          centre = static_cast<double>(20 * sl.var1);
          width = static_cast<double>(16 * sl.var2);
        } else if (sl.oscflag != 0) {
          centre = static_cast<double>(20 * sl.var1);
          width = static_cast<double>(osc + 32768) / 16.0;
        } else {
          centre = static_cast<double>(osc + 32768) / 13.0;
          width = static_cast<double>(16 * sl.var2);
        }
        resonance(w, D, A, st, centre, width, sl.type == 6);
        break;
      }
      case 7: {
        const int m = osc1 ? om : s16(sl.var1);
        for (int i = 0; i < 256; ++i)
          w[D + i] = s16(tdiv(w[A + i] * m, 256) + tdiv(w[B + i] * (255 - m), 256));
        break;
      }
      case 8: {
        int q = osc1 ? om : s16(sl.var1);
        for (int i = 0; i < 256; ++i) {
          const int sv = (q >= 0 && q < 256) ? s.sine[q] : 0;
          const int m = (sv >> 8) + 128;
          w[D + i] = s16(tdiv(w[A + i] * m, 256) + tdiv(w[B + i] * (255 - m), 256));
          q = (q + 1) & 255;
        }
        break;
      }
      case 9: {
        const int m = osc1 ? om : s16(sl.var1);
        for (int i = 0; i < 256; ++i) {
          int a = tdiv(w[A + i] * m, 16) + 32768;
          if (a < 0) a = -a;
          a %= 131072;
          if (a > 65535) a = 131071 - a;
          w[D + i] = s16(a - 32768);
        }
        break;
      }
      case 10: {
        const int16_t first = w[D];
        std::memmove(w + D, w + D + 1, 255 * sizeof(int16_t));
        w[D + 255] = first;
        break;
      }
      case 11:
        if (st.cnt1 != 0) {
          --st.cnt1;
        } else {
          st.cnt1 = sl.var1;
          for (int i = 0; i < 128; ++i) w[D + i] = w[D + 2 * i];
          for (int i = 0; i < 128; ++i) w[D + 128 + i] = w[D + i];
        }
        break;
      case 12: {
        const int m = osc1 ? om : s16(sl.var1);
        for (int i = 0; i < 256; ++i) w[D + i] = static_cast<int16_t>(clampi(tdiv(w[A + i] * m, 16), -32767, 32767));
        break;
      }
      case 13:
      case 14:
      case 15: {
        const int factor = sl.type == 14 ? 32 : 16, divisor = sl.type == 14 ? 8 : 16;
        int F, Q;
        if (osc1) {
          F = tdiv(osc + 32768, divisor);
          Q = sl.var2;
        } else {
          F = sl.var1 * factor;
          Q = osc2 ? (osc + 32768) >> 8 : sl.var2;
        }
        svf(w, D, A, st, F, Q, sl.type);
        break;
      }
      case 16:
        for (int i = 0; i < 256; ++i) {
          const uint32_t r = static_cast<uint32_t>(s.rnd.next());
          w[D + i] = s16(static_cast<int32_t>(2u * r - 32768u));
        }
        break;
      case 17: {
        uint16_t a = u16(sl.var1), b = u16(sl.var2);
        if (osc2) b = static_cast<uint16_t>(om);
        if (osc1) a = static_cast<uint16_t>(om);
        const uint16_t stepq = u16(a + 256 * b);
        uint16_t q = 0;
        for (int i = 0; i < 256; ++i) {
          const int j = q >> 8, f = q & 255;
          w[D + i] = s16(((w[A + j + 1] * f) >> 8) + ((w[A + j] * (255 - f)) >> 8));
          q = static_cast<uint16_t>(q + stepq);
        }
        break;
      }
    }
  }
}

// ---- ticks ----

void tick(State& s) {
  bool rows = false;
  if (!s.frozen && !s.ended && --s.rowcount == 0) {
    s.rowcount = (s.gstep & 1) == 0 ? 8 - s.groove : 8 + s.groove;
    for (int c = 0; c < s.N; ++c) advance(s, c);
    if (++s.gstep == 64) {
      s.gstep = 0;
      ++s.gbar;
    }
    rows = true;
    if (s.gbar == s.endbar && s.gstep == s.endstep) {
      if (s.loopflag == 0) {
        s.ended = true;
        rows = false;
      } else if (!place_loop(s)) {
        s.frozen = true;   // endless: the instruments keep sounding
        rows = false;
      } else {
        s.ended = true;
        s.gbar = s.loopbar;
        s.gstep = s.loopstep;
      }
    }
  }
  for (int c = 0; c < s.N; ++c) {
    Channel& h = s.ch[c];
    if (rows) play_row(s, c);
    if (h.instr != 0 && !s.timing_only) {
      update(s, h);
      effects(s, h);
    }
  }
}

// ---- mixing ----

void piece_settings(State& s) {
  for (int c = 0; c < s.N; ++c) {
    Channel& h = s.ch[c];
    h.active = h.instr != 0;
    if (!h.active) continue;
    const Settings& in = s.sets[h.instr - 1];
    h.smode = h.sample != nullptr;
    h.base = 256 * (in.wave & 15);
    h.mask = 256 * (clampi(in.wavelen, 1, 256) - 1) + 255;
    const int F = std::max(h.freq, 10);
    h.step = s16(tdiv(256 * F, kRate));
    const int V = std::min(tdiv(h.volval + 10000, 39), 256);
    int L = 256, R = 256;
    if (h.panval > 0) L = 256 - h.panval;
    else if (h.panval < 0) R = 256 + h.panval;
    h.L = (L * V) >> 8;
    h.R = (R * V) >> 8;
    const int E = s.echoamt[c];
    h.EL = (E * h.L) >> 8;
    h.ER = (E * h.R) >> 8;
  }
}

inline int sample_at(const Instr* in, int64_t k) {
  if (k < 0 || k >= static_cast<int64_t>(in->sample.size())) return 0;
  return in->sample[static_cast<size_t>(k)];
}

// one frame before the echo; moves the channels on
void mix_frame(State& s, int e, int& outL, int& outR, int& eL, int& eR) {
  int sumL = 0, sumR = 0;
  eL = eR = 0;
  for (int c = 0; c < s.N; ++c) {
    Channel& h = s.ch[c];
    if (!h.active) continue;
    int a;
    if (h.smode) {
      if (h.stopped) {
        a = 0;
      } else {
        const int64_t i = h.x >> 8;
        const int f = static_cast<int>(h.x & 255);
        a = ((sample_at(h.sample, i + 1) * f) >> 8) + ((sample_at(h.sample, i) * (255 - f)) >> 8);
        if (!h.back) {
          h.x += h.step;
          if (h.x >= h.endp) {
            if (!h.loops) h.stopped = true;
            else if (!h.bidi) h.x -= h.endp - h.loopp;
            else {
              h.x -= h.step;
              h.back = true;
            }
          }
        } else {
          h.x -= h.step;
          if (h.x <= h.loopp) {
            h.x += h.step;
            h.back = false;
          }
        }
      }
    } else {
      const int i = h.phase >> 8, j = (i + 1) & 255, f = h.phase & 255;
      a = ((h.w[h.base + j] * f) >> 8) + ((h.w[h.base + i] * (255 - f)) >> 8);
      h.phase = static_cast<uint16_t>(static_cast<uint16_t>(h.phase + h.step) & h.mask);
    }
    sumL += (a * h.L) >> 8;
    sumR += (a * h.R) >> 8;
    eL += (a * h.EL) >> 8;
    eR += (a * h.ER) >> 8;
  }
  outL = tdiv(tdiv(sumL, s.N) + s.echoL[e], 2);
  outR = tdiv(tdiv(sumR, s.N) + s.echoR[e], 2);
  outL = clampi(tdiv(outL * s.amp, 100), -32760, 32760);
  outR = clampi(tdiv(outR * s.amp, 100), -32760, 32760);
}

void mix(State& s, float* out, int frames) {
  for (int t = 0; t < frames; ++t) {
    int L, R, eL, eR;
    mix_frame(s, s.e, L, R, eL, eR);
    if (s.dk < 100) {
      L = tdiv(L * s.dk, 100) + tdiv(s.dbuf[s.dk] * (100 - s.dk), 100);
      ++s.dk;
    }
    if (s.dk < 100) {
      R = tdiv(R * s.dk, 100) + tdiv(s.dbuf[s.dk] * (100 - s.dk), 100);
      ++s.dk;
    }
    out[2 * t] = static_cast<float>(L) / 32768.0f;
    out[2 * t + 1] = static_cast<float>(R) / 32768.0f;
    s.echoL[s.e] = s16(tdiv(tdiv(eL, s.N) + s.echoL[s.e], 2));
    s.echoR[s.e] = s16(tdiv(tdiv(eR, s.N) + s.echoR[s.e], 2));
    s.e = (s.e + 1) % s.echodelay;
  }
}

// §8.4: 100 frames ahead with the settings as they are, to fade from at the tick
void lookahead(State& s) {
  struct Keep { uint16_t phase; int64_t x; bool back, stopped; } keep[16];
  for (int c = 0; c < s.N; ++c) keep[c] = {s.ch[c].phase, s.ch[c].x, s.ch[c].back, s.ch[c].stopped};
  int e = s.e;
  for (int t = 0; t < 100; ++t) {
    int L, R, eL, eR;
    mix_frame(s, e, L, R, eL, eR);
    s.dbuf[2 * t] = static_cast<int16_t>(L);
    s.dbuf[2 * t + 1] = static_cast<int16_t>(R);
    e = (e + 1) % s.echodelay;
  }
  for (int c = 0; c < s.N; ++c) {
    s.ch[c].phase = keep[c].phase;
    s.ch[c].x = keep[c].x;
    s.ch[c].back = keep[c].back;
    s.ch[c].stopped = keep[c].stopped;
  }
  s.dk = 0;
}

// one request of R frames (§5.2); out == nullptr: the silent length pass
int request(State& s, float* out, int R) {
  int done = 0;
  while (R > 0) {
    if (s.ended) break;
    int piece;
    if (s.C < R) {
      piece = s.C;
      s.C = s.T;
    } else {
      piece = R;
      s.C -= R;
    }
    R -= piece;
    if (out) {
      piece_settings(s);
      mix(s, out + 2 * done, piece);
    }
    done += piece;
    if (s.C == s.T) {
      if (out) {
        piece_settings(s);
        lookahead(s);
      }
      tick(s);
    }
  }
  return done;
}

// §5.3: the start of the length pass and of the playback
void restart(State& s) {
  for (int c = 0; c < 16; ++c) {
    Channel& h = s.ch[c];
    const uint16_t phase = h.phase;
    std::memset(&h, 0, sizeof(Channel));
    h.phase = phase;
  }
  s.T = tick_length(s.bpm, s.T);
  s.C = s.T;
  s.rowcount = 1;
  s.gbar = s.startbar;
  s.gstep = s.startstep - 1;
  if (s.gstep < 0) {
    s.gstep += 64;
    --s.gbar;
  }
  s.ended = false;
  s.frozen = !place_start(s);
  std::memset(s.echoL, 0, sizeof(s.echoL));
  std::memset(s.echoR, 0, sizeof(s.echoR));
  s.dk = 0;
}

void load_settings(State& s) {
  s.sets.resize(s.song.instr.size());
  for (size_t i = 0; i < s.song.instr.size(); ++i) s.sets[i] = s.song.instr[i].set;
  s.bpm = le32(s.sub + 80);
  s.groove = le32(s.sub + 84);
}

// ---- loading ----

bool need(size_t size, long long off, long long n) { return off >= 0 && n >= 0 && off + n <= static_cast<long long>(size); }

int open_song(State& s, const uint8_t* data, size_t size, int track, synth_info* info) {
  Song& song = s.song;
  if (size < kHeader) return fail(info, kNot);
  song.version = le16(data);
  if (song.version != 3456 && song.version != 3457) return fail(info, kNot);
  const int32_t P = le32(data + 4), S = le32(data + 8), I = le32(data + 12);
  if (S < 1 || S > 256 || P < 0 || P > 65536 || I < 0 || I > 4096) return fail(info, kHeaderBad);
  song.P = P;
  song.S = S;
  song.I = I;

  // walk the whole file first: nothing is taken before it is known to be there
  long long off = kHeader;
  const long long sub_off = off;
  off += static_cast<long long>(S) * kSubsong;
  const long long pat_off = off;
  off += static_cast<long long>(P) * 320;
  if (!need(size, 0, off)) return fail(info, kCut);
  for (int p = 0; p < P; ++p) {
    if (!need(size, off, 4)) return fail(info, kCut);
    const int32_t n = le32(data + off);
    if (n < 0 || n > 4096) return fail(info, kCut);
    off += 4;
    if (!need(size, off, n)) return fail(info, kCut);
    off += n;
  }
  std::vector<long long> ins_off(I), smp_len(I, -1);
  for (int i = 0; i < I; ++i) {
    if (!need(size, off, kInstr)) return fail(info, kCut);
    ins_off[i] = off;
    const bool has = le32(data + off + 512) != 0;
    const int32_t L = le32(data + off + 516);
    off += kInstr;
    if (has) {
      if (L < 0 || static_cast<unsigned long long>(L) > size) return fail(info, kCut);
      if (!need(size, off, L)) return fail(info, kCut);
      smp_len[i] = L;
      off += L;
    }
  }
  if (!need(size, off, 256)) return fail(info, kCut);
  const long long arp_off = off;

  if (track >= S) {
    if (S == 1) return fail(info, "the song has one track");
    char why[64];
    std::snprintf(why, sizeof(why), "the song has %d tracks", S);
    return fail(info, why);
  }
  const uint8_t* sub = data + sub_off + static_cast<long long>(track) * kSubsong;
  const int N = le16(sub + 146);
  if (N < 1 || N > 16) return fail(info, "the track has no channels");

  // the sanity of every instrument and pattern
  if (static_cast<uint16_t>(le16(sub + 148)) < 1) return fail(info, kDamaged);
  for (int i = 0; i < I; ++i) {
    const uint8_t* q = data + ins_off[i];
    const int wave = le16(q + 34), len = le16(q + 36);
    const int volw = le16(q + 40), vols = le16(q + 42), voll = le16(q + 44), ft = le16(q + 46);
    const int pw = le16(q + 48), ps = le16(q + 50), pl = le16(q + 52), arp = le16(q + 56);
    const int panw = le16(q + 74), pans = le16(q + 76), panl = le16(q + 78);
    const int sharing = song.version == 3456 ? 0 : le16(q + 494);
    if (wave < 0 || wave > 15 || len < 1 || len > 256) return fail(info, kDamaged);
    if (volw < 0 || volw > 16 || pw < 0 || pw > 16 || panw < 0 || panw > 16) return fail(info, kDamaged);
    if (voll < 0 || voll > 255 || pl < 0 || pl > 255 || panl < 0 || panl > 255) return fail(info, kDamaged);
    if (vols < 0 || ps < 0 || pans < 0) return fail(info, kDamaged);
    if (arp < 0 || arp > 15 || ft < 0 || ft > 15 || sharing < 0 || sharing > I) return fail(info, kDamaged);
    for (int k = 0; k < 4; ++k) {
      const uint8_t* sl = q + 92 + 40 * k;
      for (int f = 0; f < 3; ++f) {
        const int32_t v = le32(sl + 4 * f);
        if (v < 0 || v > 15) return fail(info, kDamaged);
      }
      const int32_t o = le32(sl + 12);
      if (o < 0 || o > 16) return fail(info, kDamaged);
    }
  }
  for (long long r = 0; r < 64LL * P; ++r) {
    const uint8_t* row = data + pat_off + 5 * r;
    if (row[0] > 127 || (row[4] == 1 && row[1] > 127)) return fail(info, kDamaged);
  }
  // a tempo the tick length can be made of (no real song is near either end)
  const int32_t bpm0 = le32(sub + 80);
  if (bpm0 < 0 || bpm0 > 1000) return fail(info, kDamaged);

  // take the song
  song.subsongs.assign(data + sub_off, data + sub_off + static_cast<long long>(S) * kSubsong);
  song.patterns.assign(data + pat_off, data + pat_off + static_cast<long long>(P) * 320);
  std::memcpy(song.arp, data + arp_off, 256);
  song.instr.resize(I);
  for (int i = 0; i < I; ++i) {
    const uint8_t* q = data + ins_off[i];
    Instr& in = song.instr[i];
    Settings& st = in.set;
    st.wave = le16(q + 34);
    st.wavelen = le16(q + 36);
    st.volume = le16(q + 38);
    st.volwave = le16(q + 40);
    st.volspeed = le16(q + 42);
    st.volloop = le16(q + 44);
    st.finetune = le16(q + 46);
    st.pitchwave = le16(q + 48);
    st.pitchspeed = le16(q + 50);
    st.pitchloop = le16(q + 52);
    st.pitchdelay = le16(q + 54);
    st.arp = le16(q + 56);
    std::memcpy(st.resetwave, q + 58, 16);
    st.panwave = le16(q + 74);
    st.panspeed = le16(q + 76);
    st.panloop = le16(q + 78);
    for (int k = 0; k < 4; ++k) {
      const uint8_t* sl = q + 92 + 40 * k;
      Slot& o = st.slot[k];
      o.dest = le32(sl);
      o.src1 = le32(sl + 4);
      o.src2 = le32(sl + 8);
      o.osc = le32(sl + 12);
      o.var1 = le32(sl + 16);
      o.var2 = le32(sl + 20);
      o.speed = le32(sl + 24);
      o.oscspeed = le32(sl + 28);
      o.type = le32(sl + 32);
      o.oscflag = static_cast<int8_t>(sl[36]);
      o.reset = static_cast<int8_t>(sl[37]);
    }
    if (song.version == 3456) {
      in.sharing = in.loops = in.bidi = 0;
      in.start = in.loop = in.end = 0;
    } else {
      in.sharing = le16(q + 494);
      in.loops = le16(q + 496);
      in.bidi = le16(q + 498);
      in.start = le32(q + 500);
      in.loop = le32(q + 504);
      in.end = le32(q + 508);
    }
    for (int k = 0; k < 4096; ++k) in.waves[k] = le16(q + 520 + 2 * k);
    if (smp_len[i] >= 0) {
      in.has_sample = true;
      const long long frames = smp_len[i] / 2;
      in.sample.resize(static_cast<size_t>(frames));
      const uint8_t* p = q + kInstr;
      for (long long k = 0; k < frames; ++k) in.sample[static_cast<size_t>(k)] = le16(p + 2 * k);
      if (song.version == 3456) in.end = static_cast<int32_t>(frames);
    }
  }
  // the sample points inside the sample that plays
  for (int i = 0; i < I; ++i) {
    Instr& in = song.instr[i];
    const Instr& src = in.sharing ? song.instr[in.sharing - 1] : in;
    if (!src.has_sample) continue;
    const int32_t F = static_cast<int32_t>(src.sample.size());
    in.end = std::clamp(in.end, 0, F);
    in.start = std::clamp(in.start, 0, in.end);
    if (in.loops) in.loop = std::clamp(in.loop, 0, in.end);
  }

  // the track
  s.sub = song.subsongs.data() + static_cast<size_t>(track) * kSubsong;
  const uint8_t* t = s.sub;
  s.N = N;
  s.startbar = le32(t + 88);
  s.startstep = le32(t + 92);
  s.endbar = le32(t + 96);
  s.endstep = le32(t + 100);
  s.loopbar = le32(t + 104);
  s.loopstep = le32(t + 108);
  s.loopflag = le16(t + 112);
  s.echodelay = static_cast<uint16_t>(le16(t + 148));
  std::memcpy(s.echoamt, t + 150, 16);
  s.amp = le16(t + 166);
  std::memcpy(s.mute0, song.subsongs.data() + 64, 16);   // subsong 0's, for every track
  for (int c = 0; c < 16; ++c)
    for (int k = 0; k < 256; ++k) {
      s.orderpat[c][k] = le16(t + 180 + 1024 * c + 4 * k);
      s.orderlen[c][k] = le16(t + 180 + 1024 * c + 4 * k + 2);
    }

  for (int ft = 0; ft < 16; ++ft)
    for (int i = 0; i < 128; ++i)
      s.freq[ft][i] = static_cast<int>(std::floor(220.0 * std::pow(2.0, (16.0 * (i + 3) - ft) / 192.0) + 0.5));
  for (int k = 0; k < 256; ++k)
    s.sine[k] = s.today ? 0 : static_cast<int16_t>(trunc32(std::sin(kPi * k / 128.0) * 32760.0));

  // names
  info->rate = kRate;
  info->tracks = S;
  copy_name(info->title, sizeof(info->title), t + 114, 32);
  std::snprintf(info->type, sizeof(info->type), "JayTrax");
  info->n_names = 0;
  for (int i = 0; i < I && info->n_names < SYNTH_MAX_NAMES; ++i)
    copy_name(info->names[info->n_names++], sizeof(info->names[0]), data + ins_off[i] + 2, 32);

  // the length pass: silent, in requests as the earlier player made them
  load_settings(s);
  s.rnd.seed();
  s.timing_only = !s.today;   // in the correct behaviour only its length counts
  restart(s);
  long long frames = 0;
  bool endless = false;
  for (;;) {
    const int got = request(s, nullptr, kPassRequest);
    frames += got;
    if (got < kPassRequest) break;
    if (frames >= kLongest) {
      endless = true;
      break;
    }
  }
  s.timing_only = false;
  if (frames == 0 || (s.startbar == s.endbar && s.startstep == s.endstep)) {
    info->n_names = 0;
    return fail(info, "the track is empty");
  }
  info->duration_ms = endless ? 0 : static_cast<long>(frames * 1000 / kRate);

  if (!s.today) {
    load_settings(s);
    s.rnd.seed();
    s.T = 2200;   // as at the very start (a tempo of 0 keeps it)
  }
  restart(s);
  return 1;
}

}  // namespace

extern "C" int synthw_jxs_open(const char* path, const uint8_t* data, size_t size, int track,
                               synth_info* info) {
  (void)path;
  release();
  g = new (std::nothrow) State();
  if (!g) return fail(info, "out of memory");
  const char* today = std::getenv("BBMP_JXS_TODAY");
  g->today = today && today[0] && std::strcmp(today, "0") != 0;
  const int ok = open_song(*g, data, size, track, info);
  if (!ok) release();
  return ok;
}

extern "C" int synthw_jxs_render(float* out, int frames) {
  if (!g) return 0;
  int done = 0;
  while (done < frames) {
    if (g->buf_pos >= g->buf_n) {
      if (g->done) break;
      g->buf_n = request(*g, g->buf, kPlayRequest);
      g->buf_pos = 0;
      if (g->buf_n < kPlayRequest) g->done = true;
      if (g->buf_n == 0) break;
    }
    const int n = std::min(frames - done, g->buf_n - g->buf_pos);
    std::memcpy(out + 2 * done, g->buf + 2 * g->buf_pos, sizeof(float) * 2 * n);
    g->buf_pos += n;
    done += n;
  }
  return done;
}

extern "C" void synthw_jxs_close(void) { release(); }
