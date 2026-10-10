// SPDX-License-Identifier: GPL-3.0-or-later
//
// Buzzic 1 (.buz, versions 1.0 and 1.1) and Buzzic 2 (.buz2), Stan/Rebels' 4k-intro
// trackers, played by a replayer written for this player from a description of how
// the formats and their player behave (a clean-room rewrite: no code of the trackers
// or of the earlier replayer was read to write it).
//
// Neither format has samples. A Buzzic 1 instrument is a handful of numbers (an
// oscillator, an envelope, a resonant low-pass, an echo, fold, clip and a compressor;
// 1.1 adds panning and a second filter); the whole song is computed into one buffer
// before it is heard. A Buzzic 2 instrument is a small program for a stack machine of
// floats, run once for every sample of every note on each channel, which writes into a
// window of ten rows and four seconds; each row is heard when it has been made.
//
// Everything is single-precision float, operation by operation, in the order the
// original does it: the replayer sounds bit for bit as the earlier one did when built
// without fused multiply-add (-ffp-contract=off) and linked with glibc's sinf, cosf
// and powf.

#include "engine.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int kRate = 44100;

float from_bits(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

const float kPi1 = from_bits(0x40490FDB);     // Buzzic 1's pi
const float k2Pi1 = from_bits(0x40C90FDB);
const float kPi2 = from_bits(0x40490FDA);     // Buzzic 2's pi, one bit less
const float k2Pi2 = from_bits(0x40C90FDA);
const float kKf = from_bits(0x473D6A7B);      // 0.35 * pi * 44100: Buzzic 2's filters
const float kCent = from_bits(0x3C23D70A);    // 0.01f

int32_t le32(const uint8_t* p) {
  return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                              static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24);
}
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
float lef(const uint8_t* p) { return from_bits(static_cast<uint32_t>(le32(p))); }
int8_t s8(uint8_t b) { return static_cast<int8_t>(b); }

float sign(float x) { return x > 0 ? 1.0f : -1.0f; }

// A widely published integer noise function, scaled to just above -1 .. 1.
float hash(int k) {
  uint32_t u = static_cast<uint32_t>(k);
  u = (u << 13) ^ u;
  uint32_t v = u * (u * u * 15731u + 789221u) + 1376312589u;
  v &= 0x7FFFFFFFu;
  return 1.0f - static_cast<float>(v) / 1073741824.0f;
}

// Radians per second of a note (a semitone number wrapped to a signed byte).
float note_freq(float two_pi, int n) {
  const float base = two_pi * 440.0f;
  return base * std::pow(2.0f, static_cast<float>(static_cast<int8_t>(n) - 45) / 12.0f);
}

int fail(synth_info* info, const char* why) {
  std::snprintf(info->error, sizeof(info->error), "%s", why);
  return 0;
}

void set_name(synth_info* info, const uint8_t* name, size_t room) {
  if (info->n_names >= SYNTH_MAX_NAMES) return;
  char* out = info->names[info->n_names++];
  size_t n = 0;
  while (n < room && n < 64 && name[n]) { out[n] = static_cast<char>(name[n]); ++n; }
  out[n] = 0;
}

// ---------------------------------------------------------------- Buzzic 1

struct B1 {
  int q = 0;                 // instrument record size: 148 (1.0) or 152 (1.1)
  bool v11 = false;
  const uint8_t* pat = nullptr;  // 64 patterns x 16 values
  const uint8_t* seq = nullptr;  // 64 sequences x 128 entries
  uint32_t rand = 1;         // Microsoft C's rand(), 1.0's noise
  long long rows_per = 0;    // ROW
  long long F = 0;
  std::vector<float> buf;    // (F + 1) stereo frames
};

// The stream rule: a negative value is taken from sequence -v at the row.
int b1_stream(const B1& b, int8_t v, int row) {
  if (v >= 0) return v;
  const int n = -v;
  if (n > 64) return 0;
  const int k = s8(b.seq[(n - 1) * 128 + row / 16]);
  if (k < 1 || k > 64) return 0;
  return s8(b.pat[(k - 1) * 16 + row % 16]);
}

void b1_note(B1& b, const uint8_t* ins, int row) {
  float* out = b.buf.data();
  const long long F = b.F;
  int note = b1_stream(b, s8(ins[129]), row);
  if (note == 0) return;
  const int offset = s8(ins[134]);
  note = static_cast<int8_t>(note + offset);
  const int lenval = b.v11 ? b1_stream(b, s8(ins[131]), row) : ins[131];
  const long long L = static_cast<long long>(lenval) * 441;
  if (L <= 0) return;

  const float f0 = note_freq(k2Pi1, note);
  float f1 = f0;
  if (ins[130] != 0) f1 = note_freq(k2Pi1, b1_stream(b, s8(ins[130]), row) + offset);

  const float c = static_cast<float>(b1_stream(b, s8(ins[138]), row)) * kCent;
  const float m = static_cast<float>(s8(ins[139])) * kCent;
  const float q = static_cast<float>(b1_stream(b, s8(ins[140]), row)) * kCent;
  const float V = static_cast<float>(b1_stream(b, s8(ins[132]), row)) * kCent;
  const float W = static_cast<float>(b1_stream(b, s8(ins[144]), row)) * kCent;
  const float H = static_cast<float>(b1_stream(b, s8(ins[145]), row)) * kCent;
  const bool comp = ins[147] != 0;
  const float lo = static_cast<float>(s8(ins[146])) * kCent;
  const float hi = static_cast<float>(s8(ins[147])) * kCent;
  const float d = hi - lo;
  const float A = static_cast<float>(s8(ins[135])) * kCent;
  const float D = static_cast<float>(s8(ins[136])) * kCent;
  const float S = static_cast<float>(s8(ins[137])) * kCent;
  const int osc = ins[133];
  const int delay = s8(ins[141]);
  const int repeats = s8(ins[142]);
  const float damping = static_cast<float>(s8(ins[143])) * kCent;

  float g = 0, h = 0, gm = 0, left = 1, right = 1;
  if (b.v11) {
    g = static_cast<float>(b1_stream(b, s8(ins[149]), row)) * kCent;
    gm = static_cast<float>(s8(ins[150])) * kCent;
    h = static_cast<float>(b1_stream(b, s8(ins[151]), row)) * kCent;
    const int pan = s8(ins[148]);
    if (pan != 0) {
      left = 1.0f - static_cast<float>(pan) * kCent;
      right = 1.0f + static_cast<float>(pan) * kCent;
    }
  }

  float la = 0, lb = 0, u = 0, w = 0;   // the filters' memories
  const float Lf = static_cast<float>(L);
  const long long first = static_cast<long long>(row) * b.rows_per;
  for (long long k = 0; k < L && first + k <= F; ++k) {
    const float kf = static_cast<float>(k);
    const float p = kf / Lf;
    const float t = (kf / 44100.0f) * (f0 + (f1 - f0) * p);
    float y;
    switch (osc) {
      case 0: y = std::sin(t); break;
      case 1:
      case 2: {
        float saw;
        if (b.v11) saw = ((t - std::nearbyint(t / k2Pi1) * k2Pi1) / k2Pi1) * 2.0f;
        else saw = (((t - std::floor(t / k2Pi1) * k2Pi1) / k2Pi1) - 0.5f) * 2.0f;
        y = osc == 1 ? saw : sign(saw);
        break;
      }
      case 3:
        if (b.v11) {
          y = hash(static_cast<int>(k)) - 0.5f;
        } else {
          b.rand = b.rand * 214013u + 2531011u;
          y = static_cast<float>((b.rand >> 16) & 0x7FFF) / 32767.0f - 0.5f;
        }
        break;
      default: y = 0; break;
    }
    float env;
    if (p < A) env = p / A;
    else if (p < D) env = 1.0f - (0.3f * (p - A)) / (D - A);
    else if (p < S) env = 0.7f;
    else env = (0.7f * (1.0f - p)) / (1.0f - S);
    y = y * env;
    if (c != 0) {
      const float fc = c + m * p;
      const float e = q + q / (1.0f - fc);
      la = la + fc * ((y - la) + e * (la - lb));
      lb = lb + fc * (la - lb);
      y = lb;
    }
    if (b.v11 && g != 0) {
      const float fg = g + gm * p;
      const float o = (y + ((2.0f * std::cos((kPi1 * fg) * 0.25f)) * u) * h) - (w * h) * h;
      w = u;
      u = o;
      y = o;
    }
    if (H != 0) {
      if (y > H) y = H;
      else if (y < -H) y = -H;
    }
    if (W != 0) {
      if (!std::isfinite(y) || std::fabs(y) > 1000.0f) y = 0;
      for (int i = 0; i < 100000 && std::fabs(y) > W; ++i) y = (2.0f * W) * sign(y) - y;
      if (std::fabs(y) > std::fabs(W)) y = 0;
    }
    if (comp) {
      const float v = std::fabs(y) - lo;
      if (v > 0) y = (hi - d / (1.0f + (10.0f * v) / d)) * sign(y);
    }
    const float s = y * V;
    const long long fr = first + k;
    out[2 * fr] += s * left;
    out[2 * fr + 1] += s * right;
    if (repeats > 0) {
      float e = damping;
      for (int j = 1; j <= repeats; ++j) {
        const long long at = fr + static_cast<long long>(j) * delay * 441;
        if (at >= 0 && at <= F) {
          out[2 * at] += (s * left) * e;
          out[2 * at + 1] += (s * right) * e;
        }
        e = e * e;
      }
    }
  }
}

// ---------------------------------------------------------------- Buzzic 2

enum { kMaxProgram = 1024, kMaxStack = 100, kMaxFilters = 120, kMaxEcho = 1 << 25 };

const int kOpSize[20] = {2, 5, 2, 1, 1, 1, 1, 1, 3, 1, 1, 5, 1, 1, 1, 1, 4, 1, 1, 1};

struct Op {
  uint8_t code;
  int8_t a, b, c, d;
  float x;        // code 1: the constant; 8: lo; 11: T1; 16: the damping factor
  float y, z, w;  // 8: hi, hi - lo; 11: T2, T3, Lv
  int filter;     // 12..14: which memories
  long long step; // 16: frames between echoes
};

struct Instrument {
  bool mute = false;
  int8_t note = 0, len = 0, vol = 0;
  std::vector<Op> ops;
};

struct B2 {
  float master = 0;
  int sc = 0;                       // sequences in use
  int8_t pat[64 * 16] = {};
  int8_t seq[64 * 256] = {};
  int row_len = 0;                  // ROW
  long long wn = 0;                 // window frames
  int row = 0, last_row = 0;        // the next row to make, the last one
  int pos = 0, ready = 0;           // frames of the made row handed out, made
  std::vector<float> window;
  std::vector<Instrument> ins;
  std::vector<float> konst;         // per note: what codes 0 and 2 push
};

int b2_stream(const B2& b, int8_t v, int row) {
  if (v >= 0) return v;
  const int n = -v;
  if (n > b.sc) return 0;
  const int k = b.seq[(n - 1) * 256 + row / 16];
  if (k < 1 || k > 64) return 0;
  return b.pat[(k - 1) * 16 + row % 16];
}

// Checks a program and turns it into operations; false if it can't be run.
bool b2_compile(const uint8_t* prog, int len, int row_len, std::vector<Op>& ops, long long& reach) {
  int at = 0, depth = 0, filters = 0;
  reach = 0;
  ops.clear();
  while (at < len) {
    const int code = prog[at];
    if (code > 19 || at + kOpSize[code] > len) return false;
    Op op{};
    op.code = static_cast<uint8_t>(code);
    if (kOpSize[code] > 1) op.a = s8(prog[at + 1]);
    if (kOpSize[code] > 2) op.b = s8(prog[at + 2]);
    if (kOpSize[code] > 3) op.c = s8(prog[at + 3]);
    if (kOpSize[code] > 4) op.d = s8(prog[at + 4]);
    int need = 0, delta = 0;
    switch (code) {
      case 0:
        if (op.a == -128) return false;
        delta = 1;
        break;
      case 1: op.x = lef(prog + at + 1); delta = 1; break;
      case 2: case 3: case 6: case 17: delta = 1; break;
      case 4: case 5: case 9: case 10: case 15: case 18: case 19: need = 2; delta = -1; break;
      case 7: need = 1; break;
      case 8:
        need = 1;
        op.x = static_cast<float>(op.a) * kCent;
        op.y = static_cast<float>(op.b) * kCent;
        op.z = op.y - op.x;
        break;
      case 11:
        need = 1;
        op.x = static_cast<float>(op.a) * kCent;
        op.y = static_cast<float>(op.b) * kCent;
        op.z = static_cast<float>(op.c) * kCent;
        op.w = static_cast<float>(op.d) * kCent;
        break;
      case 12: case 13: case 14:
        need = 3; delta = -2;
        op.filter = filters++;
        if (filters > kMaxFilters) return false;
        break;
      case 16: {
        need = 1;
        op.x = static_cast<float>(op.a) * kCent;
        const long long dly = op.b < 0 ? -op.b : op.b;
        op.step = static_cast<long long>(row_len / 8) * dly;
        const long long r = (static_cast<long long>(row_len / 8) * 2 * dly) * (op.c > 0 ? op.c : 0);
        if (r > reach) reach = r;
        break;
      }
    }
    if (depth < need) return false;
    depth += delta;
    if (depth > kMaxStack) return false;
    ops.push_back(op);
    at += kOpSize[code];
  }
  return len == 0 || depth == 1;
}

void b2_note(B2& b, Instrument& in) {
  if (in.ops.empty()) return;
  const int r = b.row;
  const int len = b2_stream(b, in.len, r);
  const long long Ln = static_cast<long long>(len) * b.row_len / 8;
  const int note = b2_stream(b, in.note, r);
  if (Ln <= 0 || note == 0) return;
  const float Vi = static_cast<float>(b2_stream(b, in.vol, r)) * kCent;
  const long long n = Ln < b.wn ? Ln : b.wn;
  const float Lnf = static_cast<float>(Ln);
  const float tR = static_cast<float>(static_cast<long long>(r) * b.row_len) / 44100.0f;
  const size_t nops = in.ops.size();
  b.konst.resize(nops);
  for (size_t i = 0; i < nops; ++i) {
    const Op& op = in.ops[i];
    if (op.code == 0) b.konst[i] = static_cast<float>(b2_stream(b, op.a, r)) * kCent;
    else if (op.code == 2) b.konst[i] = note_freq(k2Pi2, note + op.a);
  }
  const Op* ops = in.ops.data();
  const float* konst = b.konst.data();
  float* win = b.window.data();
  const long long wn = b.wn;
  float m0[kMaxFilters], m1[kMaxFilters];
  float st[kMaxStack + 4];

  for (int ch = 0; ch < 2; ++ch) {
    for (int i = 0; i < kMaxFilters; ++i) m0[i] = m1[i] = 0;
    for (long long s = 0; s < n; ++s) {
      const float sf = static_cast<float>(s);
      const float tRel = sf / Lnf;
      const float tN = sf / 44100.0f;
      const float tG = tN + tR;
      int sp = 0;
      for (size_t i = 0; i < nops; ++i) {
        const Op& op = ops[i];
        switch (op.code) {
          case 0: case 2: st[sp++] = konst[i]; break;
          case 1: st[sp++] = op.x; break;
          case 3: st[sp++] = tRel; break;
          case 4: case 18: {
            const float a = st[--sp];
            const float bb = st[sp - 1];
            st[sp - 1] = std::sin(a * (op.code == 4 ? tN : tG) + bb);
            break;
          }
          case 5: case 19: {
            const float a = st[--sp];
            const float bb = st[sp - 1];
            const float x = (a * (op.code == 5 ? tN : tG) + bb) / k2Pi2;
            st[sp - 1] = x - std::nearbyint(x);
            break;
          }
          case 6: st[sp++] = hash(static_cast<int>(s)); break;
          case 7: st[sp - 1] = std::pow(2.0f, st[sp - 1]); break;
          case 8: {
            float x = st[sp - 1];
            float v = std::fabs(x) - op.x;
            if (v > 0) {
              v = op.y - op.z / (1.0f + v / op.z);
              x = x > 0 ? v : -v;
            }
            st[sp - 1] = x;
            break;
          }
          case 9: { const float a = st[--sp]; st[sp - 1] = a * st[sp - 1]; break; }
          case 10: { const float a = st[--sp]; st[sp - 1] = a + st[sp - 1]; break; }
          case 11: {
            const float T1 = op.x, T2 = op.y, T3 = op.z, Lv = op.w;
            float f;
            if (tRel < T1) f = tRel / T1;
            else if (tRel < T2) f = 1.0f - ((1.0f - Lv) * (tRel - T1)) / (T2 - T1);
            else if (tRel < T3) f = Lv;
            else f = (Lv * (1.0f - tRel)) / (1.0f - T3);
            st[sp - 1] = st[sp - 1] * f;
            break;
          }
          case 12: case 13: case 14: {
            const float a = st[--sp];
            const float q = st[--sp];
            const float x = st[sp - 1];
            const float f = a / kKf;
            float& M0 = m0[op.filter];
            float& M1 = m1[op.filter];
            const float e = q + q / (1.0f - f);
            M0 = M0 + f * ((x - M0) + e * (M0 - M1));
            M1 = M1 + f * (M0 - M1);
            st[sp - 1] = op.code == 13 ? x - M1 : M1;
            break;
          }
          case 15: {
            const float a = st[--sp];
            const float bb = st[sp - 1];
            st[sp - 1] = ch == 0 ? bb * std::sqrt(1.0f - a * a) : bb * a;
            break;
          }
          case 16: {
            const float x = st[sp - 1];
            float y = x * Vi;
            const bool ping = op.b < 0;
            for (int j = 1; j <= op.c; ++j) {
              y = y * op.x;
              const long long at = s + j * op.step;
              if (at >= wn) continue;
              const bool here = !ping || ((j & 1) ? ch == 0 : ch == 1);
              win[2 * at + ch] += here ? y : y * 0.0f;
            }
            st[sp - 1] = x * Vi;
            break;
          }
          case 17: st[sp++] = win[2 * s + ch]; break;
        }
      }
      win[2 * s + ch] += st[0] * Vi;
    }
  }
}

// Makes the next row into the window; false when the song has ended.
bool b2_next_row(B2& b) {
  if (b.row > b.last_row) return false;
  const long long R = b.row_len;
  float* win = b.window.data();
  std::memmove(win, win + 2 * R, sizeof(float) * 2 * static_cast<size_t>(b.wn - R));
  std::memset(win + 2 * (b.wn - R), 0, sizeof(float) * 2 * static_cast<size_t>(R));
  for (Instrument& in : b.ins)
    if (!in.mute) b2_note(b, in);
  b.row++;
  b.pos = 0;
  b.ready = b.row_len;
  return true;
}

// ---------------------------------------------------------------- the engine

struct State {
  int kind = 0;             // 0 nothing open, 1 Buzzic 1, 2 Buzzic 2
  B1 b1;
  B2 b2;
  long long b1_pos = 0, b1_end = 0;
  float b1_gain = 0;
};

State* g = nullptr;

void release() {
  delete g;
  g = nullptr;
}

const char* kNot = "not a Buzzic song";
const char* kCut = "the song is cut short";

int open_b1(const uint8_t* data, size_t size, int track, synth_info* info) {
  B1& b = g->b1;
  b.q = size == 44712 ? 148 : 152;
  b.v11 = b.q == 152;
  const uint8_t* base = data + 128 * b.q;
  const int n = le32(base);
  b.pat = base + 4;
  b.seq = base + 1156;
  const int pats = le32(base + 17540), seqs = le32(base + 17548);
  const float master = lef(base + 17556);
  const int bpm = le32(base + 17560), first = le32(base + 17564), last = le32(base + 17568);
  if (n < 1 || n > 128 || pats < 0 || pats > 64 || seqs < 0 || seqs > 64 || bpm < 15 || bpm > 1000 ||
      !(master >= 0 && master <= 16) || first < 0 || last < first || last > 127)
    return fail(info, kNot);
  if (track != 0) return fail(info, "the song has one track");
  b.rows_per = 2646000 / (4 * bpm);
  const long long rows = 16LL * (last + 1);
  b.F = rows * b.rows_per;
  if (b.F > 600LL * kRate) return fail(info, "the song is longer than ten minutes");
  const long long start = 16LL * first * b.rows_per;

  b.buf.assign(2 * static_cast<size_t>(b.F + 1), 0.0f);
  b.rand = 1;
  for (int i = 0; i < n; ++i) {
    const uint8_t* ins = data + static_cast<size_t>(i) * b.q;
    if (ins[128]) continue;
    for (long long r = 0; r < rows; ++r) b1_note(b, ins, static_cast<int>(r));
  }
  g->b1_pos = start;
  g->b1_end = b.F;
  g->b1_gain = master * 0.9765625f;

  for (int i = 0; i < n; ++i) set_name(info, data + static_cast<size_t>(i) * b.q, 128);
  std::snprintf(info->type, sizeof(info->type), "%s", b.v11 ? "Buzzic 1.1" : "Buzzic");
  info->duration_ms = static_cast<long>(static_cast<double>(b.F - start) * 1000.0 / 44100.0);
  return 1;
}

int open_b2(const uint8_t* data, size_t size, int track, synth_info* info) {
  B2& b = g->b2;
  if (track != 0) return fail(info, "the song has one track");
  const int revision = le16(data + 6);
  b.master = lef(data + 8);
  const int bpm = le32(data + 12), first = le32(data + 16), last = le32(data + 20);
  if (bpm < 8 || bpm > 2000 || !(b.master >= 0 && b.master <= 100))
    return fail(info, "the header of the song makes no sense");
  if (first < 0 || last < first || last >= 256) return fail(info, "the song has no rows to play");

  size_t at = 156;
  auto have = [&](unsigned long long bytes) { return at <= size && bytes <= size - at; };
  if (!have(8)) return fail(info, kCut);
  const int pats = le32(data + at), sc = le32(data + at + 4);
  at += 8;
  long long P = 64, S = 64, Ls = 128;
  if (revision >= 1) {
    if (!have(12)) return fail(info, kCut);
    P = le32(data + at);
    S = le32(data + at + 4);
    Ls = le32(data + at + 8);
    at += 12;
    if (P < 1 || P > 4096 || S < 1 || S > 4096 || Ls < 1 || Ls > 65536)
      return fail(info, "the tables of the song make no sense");
  }
  if (pats < 0 || pats > 64 || sc < 0 || sc > 64)
    return fail(info, "the song has more patterns or sequences than Buzzic 2 knows");
  b.sc = sc;

  if (!have(16ULL * P)) return fail(info, kCut);
  for (long long p = 0; p < P && p < 64; ++p)
    for (int j = 0; j < 16; ++j) b.pat[p * 16 + j] = s8(data[at + p * 16 + j]);
  at += 16 * P;
  if (!have(static_cast<unsigned long long>(S) * Ls)) return fail(info, kCut);
  for (long long s = 0; s < S && s < 64; ++s)
    for (long long e = 0; e < Ls && e < 256; ++e) {
      const int v = s8(data[at + s * Ls + e]);
      b.seq[s * 256 + e] = static_cast<int8_t>(v >= 0 && v <= 64 ? v : 0);
    }
  at += S * Ls;
  if (!have(8192)) return fail(info, kCut);
  at += 8192;
  if (!have(4)) return fail(info, kCut);
  const int n = le32(data + at);
  at += 4;
  if (n < 1 || n > 128) return fail(info, "the song has no instruments");

  const float rowf = 2646000.0f / (static_cast<float>(bpm) * 4.0f);
  b.row_len = static_cast<int>(rowf);
  if (b.row_len < 8) return fail(info, "the header of the song makes no sense");

  long long reach = 0;
  b.ins.resize(n);
  for (int i = 0; i < n; ++i) {
    if (!have(9232)) return fail(info, kCut);
    const uint8_t* rec = data + at;
    const int e = le32(rec + 388);
    if (e < 0 || e > 64) return fail(info, kCut);
    at += 9232;
    if (!have(552ULL * e)) return fail(info, kCut);
    at += 552 * static_cast<size_t>(e);

    Instrument& in = b.ins[i];
    in.mute = rec[128] != 0;
    in.note = s8(rec[129]);
    in.len = s8(rec[130]);
    in.vol = s8(rec[131]);
    const int plen = le32(rec + 9100);
    long long r = 0;
    if (in.note == -128 || in.len == -128 || in.vol == -128 || plen < 0 || plen > kMaxProgram ||
        !b2_compile(rec + 8076, plen, b.row_len, in.ops, r))
      return fail(info, "an instrument of the song can't be run");
    if (r > reach) reach = r;
    uint8_t name[128];
    std::memcpy(name, rec, 128);
    name[127] = 0;
    set_name(info, name, 128);
  }
  if (reach > 2LL * kMaxEcho) return fail(info, "an echo of the song is longer than can be");

  b.wn = 10LL * b.row_len + 176400;
  b.window.assign(2 * static_cast<size_t>(b.wn), 0.0f);
  b.row = first * 16;
  b.last_row = (last + 1) * 16 - 1;
  b.pos = b.ready = 0;
  const long long rows = 16LL * (last - first + 1);
  std::snprintf(info->type, sizeof(info->type), "Buzzic 2");
  info->duration_ms = static_cast<long>(static_cast<double>(rows * b.row_len) * 1000.0 / 44100.0);
  return 1;
}

}  // namespace

extern "C" int synthw_buz_open(const char* path, const uint8_t* data, size_t size, int track,
                               synth_info* info) {
  (void)path;
  release();
  g = new State;
  info->rate = kRate;
  info->tracks = 1;
  info->n_names = 0;
  int ok;
  if ((size == 44712 || size == 45224) && [&] {
        const int n = le32(data + 128 * (size == 44712 ? 148 : 152));
        return n >= 1 && n <= 128;
      }()) {
    g->kind = 1;
    ok = open_b1(data, size, track, info);
  } else if (size >= 156 && std::memcmp(data, "buz2", 4) == 0 && le16(data + 4) == 2) {
    g->kind = 2;
    ok = open_b2(data, size, track, info);
  } else {
    ok = fail(info, kNot);
  }
  if (!ok) {
    info->n_names = 0;
    release();
  }
  return ok;
}

extern "C" int synthw_buz_render(float* out, int frames) {
  if (!g) return 0;
  int done = 0;
  if (g->kind == 1) {
    const float* buf = g->b1.buf.data();
    const float gain = g->b1_gain;
    while (done < frames && g->b1_pos < g->b1_end) {
      for (int c = 0; c < 2; ++c) {
        float v = buf[2 * g->b1_pos + c] * gain;
        if (v != v) v = 0;
        out[2 * done + c] = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
      }
      ++g->b1_pos;
      ++done;
    }
  } else if (g->kind == 2) {
    B2& b = g->b2;
    while (done < frames) {
      if (b.pos >= b.ready && !b2_next_row(b)) break;
      const float* win = b.window.data();
      while (done < frames && b.pos < b.ready) {
        for (int c = 0; c < 2; ++c) {
          float v = win[2 * b.pos + c] * b.master;
          if (v != v) v = 0;
          out[2 * done + c] = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
        }
        ++b.pos;
        ++done;
      }
    }
  }
  return done;
}

extern "C" void synthw_buz_close(void) { release(); }
