// SPDX-License-Identifier: GPL-3.0-or-later
// A replayer for songs of Aquaplus' P/ECE music driver ("PiECE music driver" on Modland),
// written anew for this player.
//
// Where it comes from: the format is as the P/ECE SDK's documents describe it
// (musbin.txt, musbin2.txt and the MML reference mml.htm on the SDK's CD) and as the
// SDK's MML compiler (mucc 1.00) writes it; how the driver sounds (pitch, tempo,
// volume steps, envelope rates, vibrato, portamento, mixing) was measured on its output:
// the driver running in a P/ECE emulator. No code of the driver was read or used; its
// terms allow using it only in programs for the P/ECE.
//
// The driver mixes up to six parts at 16 kHz and does its envelopes, vibrato and
// portamento once every 8 ms (128 samples).

#include "piece.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace piece {

namespace {

constexpr int RATE = 16000;
constexpr int FRAME = 128;            // 8 ms: the driver's step for envelopes and LFOs
constexpr int PARTS = 6;
constexpr double TWO_PI = 6.283185307179586;

// Envelope levels are in steps of 1/16 octave of amplitude (16 = twice as loud); 127 is
// full. A rate r (0-255) moves the level by 2^(r/16)/400 steps per 8 ms.
double rate(int r) { return std::pow(2.0, r / 16.0) / 400.0; }
constexpr double ATTACK_START = 64;   // where an attack starts from
constexpr double RELEASE = 20.8;      // steps per 8 ms after the key is let go
constexpr double FULL = 127;

// One part's amplitude at full volume and envelope, before the mix, and the P/ECE's
// output: the parts are summed in 16 bits (which wrap) and the sum is scaled down.
constexpr double VOICE = 7874;
constexpr double OUT = 4450.0 / 7874.0 / 32768.0;

inline uint16_t rd16(const std::vector<uint8_t>& d, size_t p) {
  return p + 1 < d.size() ? static_cast<uint16_t>(d[p] | d[p + 1] << 8) : 0;
}

// how many bytes of arguments each command has (0xe0-0xff); -1: not a command
const int ARGS[32] = {
    0, 1, 2, 3, 1, 0, 1, 1, 1, 1, 4, 1, 1, 2, 0, 0,     // e0-ef
    1, 4, -1, -1, -1, -1, 0, -1, 0, 0, 1, 1, -1, -1, -1, -1};  // f0-ff

enum Stage { OFF, ATTACK, DECAY, SUSTAIN, RELEASE_ };

struct Part {
  bool used = false, ended = true, looped = false;
  uint32_t pos = 0;
  struct Call { uint32_t ret, addr; int count; };
  std::vector<Call> calls;
  struct Rep { uint32_t start; int count; };
  std::vector<Rep> reps;

  // sequencing
  int len = 24, wait = 0, gate = 100, keyOffIn = -1;
  bool slur = false, held = false;
  bool drums = false;
  int fixedNote = -1;
  int transpose = 0, detune = 0;
  int inst = 0, vol = 111, volOfs = 0;   // a part that sets no volume plays at 111
  int ar = 255, dr = 0, sl = 127, sr = 0;
  // vibrato, as the driver does it (the MML reference names the last two the other
  // way round): depth in 1/24 semitones, speed, how fast it grows, delay in 8 ms steps
  int vibDepth = 0, vibSpeed = 0, vibGrow = 0, vibDelay = 0;
  double vibNow = 0;          // the depth it has grown to
  int portStart = 0, portSpeed = 0;
  bool portOn = false;

  // the voice
  Stage stage = OFF;
  double level = 0;           // envelope, in 1/16 octaves
  double pitch = 0;           // in 1/256 semitones, 0 = C0
  double target = 0;
  double portStep = 0;        // per 8 ms
  int frames = 0;             // since key on
  double phase = 0;           // 0..1
  double vibPhase = 0;
  uint32_t noise = 1;         // for the drums
  double dstate[4] = {};      // drum state
  double dpos = 0;            // where in its drum sound
  int dInst = -1;
  double amp = 0;             // this frame's amplitude
};

}  // namespace

struct Song {
  std::vector<uint8_t> d;
  std::string title, title2;
  int nparts = 0;
  uint16_t ptr[PARTS] = {};
  uint16_t drumTable = 0;
  Part part[PARTS];
  int tempo = 120;
  double tickAcc = 1.0;       // ticks due (the first at once)
  int frameLeft = 0;
  bool running = true;
  // measuring
  long long ticks = 0;
  long long ops = 0;          // commands read, for the measurement's limit
  double seconds = 0;
  bool allLooped = false;
  double loopedAt = -1;
};

namespace {

std::string cstr(const std::vector<uint8_t>& d, uint16_t p) {
  std::string s;
  if (!p) return s;
  for (size_t i = p; i < d.size() && d[i] && s.size() < 256; i++) s += static_cast<char>(d[i]);
  return s;
}

// The end of the repeat that starts at `p` (just after e4 n): the position of its e5.
uint32_t findNext(const std::vector<uint8_t>& d, uint32_t p) {
  int depth = 0;
  while (p < d.size()) {
    const uint8_t c = d[p];
    if (c == 0) return p;
    if (c < 0x7f || (c >= 0x80 && c < 0xe0)) { p++; continue; }
    if (c == 0x7f) { p += 2; continue; }
    const int n = ARGS[c - 0xe0];
    if (n < 0) return p;
    if (c == 0xe4) depth++;
    if (c == 0xe5) {
      if (depth == 0) return p;
      depth--;
    }
    p += 1 + n;
  }
  return p;
}

double noteFreq(double pitch) {
  // pitch in 1/256 semitones from C0; A4 (57 semitones up) is 440 Hz
  return 440.0 * std::pow(2.0, (pitch / 256.0 - 57.0) / 12.0);
}

void keyOn(Song& s, Part& p, int note) {
  (void)s;
  const double target = (std::clamp(note, 0, 95)) * 256.0 + p.detune;
  const bool legato = p.held && p.stage != OFF && p.stage != RELEASE_;
  if (p.portOn) {
    const double from = p.portStart ? target + p.portStart * 256.0 : p.pitch;
    p.pitch = (p.stage == OFF && !p.portStart) ? target : from;
    p.target = target;
    // speed: the compiler writes log2 of the slide per tick (in 1/256 semitones) in the
    // high nibble; the low nibble is taken as a fraction
    const int e = p.portSpeed >> 4, m = p.portSpeed & 15;
    p.portStep = std::ldexp(1.0 + m / 16.0, e);   // per tick
  } else {
    p.pitch = p.target = target;
    p.portStep = 0;
  }
  if (!legato) {
    if (p.inst != p.dInst) {
      std::memset(p.dstate, 0, sizeof p.dstate);
      p.dInst = p.inst;
    }
    std::memset(p.dstate, 0, sizeof p.dstate);
    p.phase = 0;                      // the driver starts each note's wave from its start
    p.dpos = 0;
    p.frames = 0;
    p.vibPhase = 0;
    p.vibNow = 0;
    if (p.ar < 16) {
      p.level = FULL;
      p.stage = DECAY;
    } else {
      p.level = ATTACK_START;
      p.stage = ATTACK;
    }
  }
}

void keyOff(Part& p) {
  if (p.stage != OFF) p.stage = RELEASE_;
}

void setInstrument(Part& p, int i) {
  p.inst = i;
  // an instrument brings its own envelope; the waves' is flat
  p.ar = 255; p.dr = 0; p.sl = 127; p.sr = 0;
}

// Read commands until the part has something to wait for (a note or a rest).
void step(Song& s, Part& p) {
  const auto& d = s.d;
  for (int guard = 0; guard < 100000; guard++) {
    s.ops++;
    if (p.pos >= d.size()) { p.ended = true; keyOff(p); return; }
    const uint8_t c = d[p.pos];
    if (c == 0x00) {
      if (!p.calls.empty()) {
        Part::Call& k = p.calls.back();
        if (--k.count > 0) p.pos = k.addr;
        else { p.pos = k.ret; p.calls.pop_back(); }
        continue;
      }
      p.ended = true;
      keyOff(p);
      return;
    }
    if (c < 0x7f) { p.len = c; p.pos++; continue; }
    if (c == 0x7f) {
      const int v = p.pos + 1 < d.size() ? d[p.pos + 1] : 0;
      p.len = v >= 0x7f ? v : v + 256;
      p.pos += 2;
      continue;
    }
    if (c == 0xec) {
      // "?" in the MML: a note at the pitch given, not transposed
      const int note = (p.pos + 1 < d.size() ? d[p.pos + 1] : 0x80) - 0x80;
      p.pos += 2;
      p.wait = std::max(1, p.len);
      keyOn(s, p, note);
      p.held = p.slur;
      p.keyOffIn = p.held ? -1 : std::max(1, p.len * p.gate / 100);
      return;
    }
    if (c < 0xe0 || c == 0xe0) {
      p.pos++;
      p.wait = std::max(1, p.len);
      if (c == 0xe0) {
        keyOff(p);
        p.held = false;
        p.keyOffIn = -1;
        return;
      }
      int note = c - 0x80;
      if (p.drums) {
        // the note picks a drum: its macro sets instrument, volume and pitch
        const int k = c - 0x80;
        const uint16_t tab = s.drumTable;
        if (tab && tab + 2u * k + 1 < d.size()) {
          const uint16_t m = rd16(d, tab + 2 * k);
          const uint32_t save = p.pos;
          p.pos = m;
          // run the macro's commands up to its end
          for (int g = 0; g < 64 && p.pos < d.size() && d[p.pos] != 0; g++) {
            const uint8_t mc = d[p.pos];
            if (mc >= 0xe0) {
              const int n = ARGS[mc - 0xe0];
              if (n < 0) break;
              const uint8_t a0 = p.pos + 1 < d.size() ? d[p.pos + 1] : 0;
              if (mc == 0xe8) setInstrument(p, a0);
              else if (mc == 0xe9) p.vol = a0;
              else if (mc == 0xec) p.fixedNote = a0 - 0x80;
              else if (mc == 0xeb) p.detune = static_cast<int8_t>(a0);
              else if (mc == 0xea && p.pos + 4 < d.size()) {
                p.ar = d[p.pos + 1]; p.dr = d[p.pos + 2]; p.sl = d[p.pos + 3]; p.sr = d[p.pos + 4];
              }
              p.pos += 1 + n;
            } else {
              p.pos++;
            }
          }
          p.pos = save;
        }
        note = p.fixedNote >= 0 ? p.fixedNote : 48;
      } else {
        note += p.transpose;
      }
      const bool wasHeld = p.held;
      keyOn(s, p, note);
      p.held = p.slur;
      if (p.held) p.keyOffIn = -1;
      else p.keyOffIn = std::max(1, p.len * p.gate / 100);
      (void)wasHeld;
      return;
    }
    const int n = ARGS[c - 0xe0];
    if (n < 0) { p.ended = true; keyOff(p); return; }   // not a command: stop here
    const uint32_t a = p.pos + 1;
    auto arg = [&](int i) -> int { return a + i < d.size() ? d[a + i] : 0; };
    p.pos += 1 + n;
    switch (c) {
      case 0xe1: p.gate = std::clamp(arg(0), 1, 99); break;   // 100 plays as 99
      case 0xe2:
        p.pos = rd16(d, a);
        if (p.calls.empty()) p.looped = true;
        break;
      case 0xe3: {
        const uint16_t to = rd16(d, a);
        const int cnt = arg(2);
        if (cnt > 0 && p.calls.size() < 32) {
          p.calls.push_back({p.pos, to, cnt});
          p.pos = to;
        }
        break;
      }
      case 0xe4:
        if (p.reps.size() < 32) p.reps.push_back({p.pos, arg(0)});
        break;
      case 0xe5:
        if (!p.reps.empty()) {
          Part::Rep& r = p.reps.back();
          if (r.count == 0) {                 // for ever
            if (p.calls.empty()) p.looped = true;
            p.pos = r.start;
          } else if (--r.count > 0) {
            p.pos = r.start;
          } else {
            p.reps.pop_back();
          }
        }
        break;
      case 0xf6:                               // leave on the last time round
        if (!p.reps.empty() && p.reps.back().count == 1) {
          p.pos = findNext(d, p.pos) + 1;
          p.reps.pop_back();
        }
        break;
      case 0xe6: p.transpose = static_cast<int8_t>(arg(0)); break;
      case 0xe7: if (arg(0)) s.tempo = arg(0); break;
      case 0xe8: setInstrument(p, arg(0)); break;
      // a volume ends the offset of v+ v- (as measured; the MML reference says it doesn't)
      case 0xe9: p.vol = arg(0); p.volOfs = 0; break;
      case 0xea: p.ar = arg(0); p.dr = arg(1); p.sl = arg(2); p.sr = arg(3); break;
      case 0xeb: p.detune = static_cast<int8_t>(arg(0)); break;
      case 0xed: p.portStart = static_cast<int8_t>(arg(0)); p.portSpeed = arg(1); break;
      case 0xee: p.portOn = true; break;
      case 0xef: p.portOn = false; break;
      case 0xf0: p.drums = arg(0) != 0; break;
      case 0xf1: p.vibDepth = arg(0); p.vibSpeed = arg(1); p.vibGrow = arg(2); p.vibDelay = arg(3); break;
      case 0xf8: p.slur = true; break;
      case 0xf9: p.slur = false; break;
      // the MML's v+ and v-: an offset to the volume, written as 127 + the offset (as
      // measured: fa 117 plays 10 steps quieter, fa 0 nearly silent, fa 240 loud)
      case 0xfa: p.volOfs = arg(0) - 127; break;
      case 0xfb: p.vol = std::clamp(p.vol + static_cast<int8_t>(arg(0)), 0, 255); break;
      default: break;
    }
  }
  p.ended = true;
}

void tick(Song& s) {
  for (Part& p : s.part) {
    if (!p.used || p.ended) continue;
    if (p.keyOffIn > 0 && --p.keyOffIn == 0) keyOff(p);
    if (--p.wait <= 0) step(s, p);
  }
  s.ticks++;
}

// The amplitude of volume plus envelope level, both in steps of 1/16 octave (127 full),
// as s = volume + level - 254 (0 = both full). As measured on the driver: it grows on
// above 0 up to s = 33 (V152 at full level plays 25 steps above V127), from 33 to about
// 38.75 it is half as loud (V160 at full level plays like V144), and beyond that it is
// nearly silent, until a falling envelope (a release) brings s down again: V200 is
// silent while held and sounds when it is let go, as the driver plays it.
double gain(double s) {
  if (s >= 38.75) return 0.004;
  if (s >= 33) return std::pow(2.0, (s - 16) / 16.0);
  return std::pow(2.0, s / 16.0);
}

// once every 8 ms: envelopes, vibrato, portamento
void frame(Song& s) {
  const double ticksPerFrame = FRAME / (40000.0 / s.tempo);
  for (Part& p : s.part) {
    if (!p.used) continue;
    switch (p.stage) {
      case ATTACK:
        p.level += rate(p.ar);
        if (p.level >= FULL) { p.level = FULL; p.stage = DECAY; }
        break;
      case DECAY:
        if (p.dr) {
          p.level -= rate(p.dr);
          if (p.level <= p.sl) p.stage = SUSTAIN;
        }
        break;
      case SUSTAIN:
        if (p.sr) p.level -= rate(p.sr);
        break;
      case RELEASE_:
        p.level -= RELEASE;
        break;
      case OFF:
        break;
    }
    if (p.stage != OFF && p.level <= 0) { p.level = 0; p.stage = OFF; }
    if (p.portStep > 0 && p.pitch != p.target) {
      const double st = p.portStep * ticksPerFrame;
      if (p.pitch < p.target) p.pitch = std::min(p.target, p.pitch + st);
      else p.pitch = std::max(p.target, p.pitch - st);
    }
    p.frames++;
    if (p.vibDepth && p.frames > p.vibDelay && p.vibNow < p.vibDepth) {
      p.vibNow = std::min<double>(p.vibDepth, p.vibNow + std::pow(2.0, p.vibGrow / 16.0) / 48.0);
    }
    if (p.vibNow > 0) p.vibPhase += TWO_PI * 0.0154 * std::pow(2.0, p.vibSpeed / 16.0) * FRAME / RATE;
    p.amp = p.stage == OFF ? 0 : gain(std::max(p.vol + p.volOfs, 0) + p.level - 2 * FULL);
  }
}

// in 1/256 semitones; it swings down first
double vibrato(const Part& p) {
  if (p.vibNow <= 0) return 0;
  return -p.vibNow * (256.0 / 24.0) * std::sin(p.vibPhase);
}

constexpr int TABLE = 32;

// The drums (instruments 6-16). The driver plays sampled drums of its own; these are
// made here instead, after how those sound (length, pitch, loudness, how noisy),
// measured on the driver's output at C4.
struct Noise {
  uint32_t s = 0x12345678u;
  double operator()() {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (s & 0xffffff) / 8388608.0 - 1.0;
  }
};

struct Drum {
  double len;                 // seconds
  double f0, f1, fTau;        // tone: from f0 to f1 Hz, time constant
  double tone, toneTau;       // tone level and decay (s)
  double noise, noiseTau;     // noise level and decay (s)
  double hp, lp;              // noise band, Hz (one-pole filters)
  double hold;                // seconds at full level before decaying
  int bursts;                 // hand clap: several short bursts
};

const Drum DRUMS[11] = {
    // 6: bass drum
    {0.18, 170, 50, 0.030, 1.00, 0.045, 0.02, 0.004, 200, 3000, 0.04, 1},
    // 7: snare, long (gated)
    {0.36, 220, 190, 0.050, 0.10, 0.050, 0.40, 0.160, 500, 3500, 0.03, 1},
    // 8: snare
    {0.12, 205, 185, 0.030, 0.60, 0.030, 0.55, 0.045, 700, 4000, 0.0, 1},
    // 9: open hi-hat
    {0.33, 0, 0, 1, 0, 1, 1.20, 0.140, 5000, 8000, 0.0, 1},
    // 10: closed hi-hat
    {0.09, 0, 0, 1, 0, 1, 1.30, 0.035, 5000, 8000, 0.0, 1},
    // 11: crash cymbal
    {0.62, 0, 0, 1, 0, 1, 0.65, 0.140, 2500, 8000, 0.04, 1},
    // 12: the same crash
    {0.62, 0, 0, 1, 0, 1, 0.65, 0.140, 2500, 8000, 0.04, 1},
    // 13-15: toms, high to low
    {0.15, 195, 160, 0.080, 0.70, 0.045, 0.10, 0.080, 700, 3000, 0.03, 1},
    {0.19, 150, 118, 0.100, 0.70, 0.055, 0.10, 0.080, 700, 3000, 0.04, 1},
    {0.24, 104, 80, 0.120, 0.70, 0.070, 0.09, 0.090, 700, 3000, 0.05, 1},
    // 16: hand clap
    {0.11, 0, 0, 1, 0, 1, 0.55, 0.030, 700, 3000, 0.0, 3},
};

const std::vector<float>& drum(int inst) {
  static std::vector<float> made[11];
  static bool done[11];
  const int k = std::clamp(inst, 6, 16) - 6;
  if (done[k]) return made[k];
  const Drum& d = DRUMS[k];
  const int n = static_cast<int>(d.len * RATE);
  std::vector<float>& out = made[k];
  out.resize(n + 1);
  Noise rnd;
  rnd.s ^= 0x9e3779b9u * (k + 1);
  double ph = 0, lpState = 0, hpState = 0, hpPrev = 0;
  const double aLp = 1 - std::exp(-TWO_PI * d.lp / RATE);
  const double aHp = std::exp(-TWO_PI * d.hp / RATE);
  for (int i = 0; i < n; i++) {
    const double t = double(i) / RATE;
    const double td = std::max(0.0, t - d.hold);
    double x = 0;
    if (d.tone > 0) {
      const double f = d.f1 + (d.f0 - d.f1) * std::exp(-t / d.fTau);
      ph += f / RATE;
      x += d.tone * std::exp(-td / d.toneTau) * std::sin(TWO_PI * ph);
    }
    // band-limited noise
    const double w = rnd();
    lpState += aLp * (w - lpState);
    const double hp = aHp * (hpState + lpState - hpPrev);
    hpPrev = lpState;
    hpState = hp;
    double env = std::exp(-td / d.noiseTau);
    if (d.bursts > 1) {
      const double b = std::fmod(t, 0.011);
      if (t < 0.011 * (d.bursts - 1)) env = std::exp(-b / 0.004);
      else env = std::exp(-(t - 0.011 * (d.bursts - 1)) / d.noiseTau);
    }
    x += d.noise * 2.5 * hp * env;
    // a short fade at the end
    const double left = d.len - t;
    if (left < 0.01) x *= left / 0.01;
    out[i] = static_cast<float>(std::clamp(x, -1.0, 1.0));
  }
  out[n] = 0;
  done[k] = true;
  return made[k];
}

// step i of the table waves: square, saw, triangle
double table(int inst, int i) {
  const double ph = (i + 0.5) / TABLE;
  switch (inst) {
    case 3: return i < TABLE / 2 ? 1 : -1;
    case 4: return 2 * ph - 1;
    default: return ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph;
  }
}

// one sample of a part at 16 kHz, -1..1
double voice(Part& p) {
  if (p.stage == OFF || p.amp <= 0) return 0;
  const double pitch = p.pitch + vibrato(p);
  const double f = noteFreq(pitch);
  const double inc = f / RATE;
  double x = 0;
  const double ph = p.phase;
  switch (p.inst) {
    // 0-2 are worked out from the phase; 3-5 are read from 32-step tables with
    // straight lines between the steps, which rounds their corners
    case 0: x = ph < 0.5 ? 1 : -1; break;
    case 1: x = 2 * ph - 1; break;
    case 2: x = ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph; break;
    case 3: case 4: case 5: {
      const double at = ph * TABLE;
      const int i = static_cast<int>(at);
      const double f = at - i;
      const double a = table(p.inst, i), b = table(p.inst, (i + 1) % TABLE);
      x = a + (b - a) * f;
      break;
    }
    default: {
      // drums: one-shot sounds made at C4 (note 48) and played faster or slower
      const std::vector<float>& t = drum(p.inst);
      const size_t i = static_cast<size_t>(p.dpos);
      if (i + 1 < t.size()) {
        const double f = p.dpos - i;
        x = t[i] + (t[i + 1] - t[i]) * f;
      }
      p.dpos += std::pow(2.0, (pitch / 256.0 - 48.0) / 12.0);
      break;
    }
  }
  p.phase += inc;
  if (p.phase >= 1) p.phase -= std::floor(p.phase);
  return x * p.amp;
}

}  // namespace

Song* load(const std::vector<uint8_t>& data, std::string& why) {
  if (data.size() < 20 || data[0] != 0 || data[1] < 1 || data[1] > 16) {
    why = "not a P/ECE music driver song";
    return nullptr;
  }
  Song* s = new Song;
  s->d = data;
  const int np = data[1];
  const size_t hdr = 2 + 2 * (np + 3);
  if (hdr > data.size()) { delete s; why = "the song's header is cut short"; return nullptr; }
  for (int i = 0; i < np && i < PARTS; i++) s->ptr[i] = rd16(data, 2 + 2 * i);
  s->drumTable = rd16(data, 2 + 2 * np);
  s->title = cstr(data, rd16(data, 2 + 2 * np + 2));
  s->title2 = cstr(data, rd16(data, 2 + 2 * np + 4));
  for (int i = 0; i < PARTS; i++) {
    Part& p = s->part[i];
    if (s->ptr[i] && s->ptr[i] < data.size()) {
      p.used = true;
      p.ended = false;
      p.pos = s->ptr[i];
      p.wait = 1;
      p.noise = 0x9e3779b9u ^ (i * 0x85ebca6bu);
      s->nparts++;
    }
  }
  if (!s->nparts) { delete s; why = "the song has no parts"; return nullptr; }
  s->frameLeft = 0;
  return s;
}

void destroy(Song* s) { delete s; }
std::string title(const Song* s) { return s->title; }
std::string title2(const Song* s) { return s->title2; }
int parts(const Song* s) { return s->nparts; }

int render16k(Song* s, float* out, int n) {
  for (int i = 0; i < n; i++) {
    if (s->frameLeft <= 0) {
      // the driver works in steps of 8 ms: it counts the tempo's ticks there, starts
      // and ends notes, then moves envelopes, vibrato and portamento on
      s->tickAcc += s->tempo * 24.0 * FRAME / (60.0 * RATE);
      while (s->tickAcc >= 1.0) {
        s->tickAcc -= 1.0;
        tick(*s);
      }
      frame(*s);
      s->frameLeft += FRAME;
    }
    bool sounding = false, playing = false;
    double sum = 0;
    for (Part& p : s->part) {
      if (!p.used) continue;
      if (!p.ended) playing = true;
      if (p.stage != OFF) sounding = true;
      sum += voice(p) * VOICE;
    }
    if (!playing && !sounding) return i;
    // the driver sums the parts in 16 bits
    long v = std::lround(sum);
    v = static_cast<int16_t>(static_cast<uint16_t>(v & 0xffff));
    out[i] = static_cast<float>(v * OUT);
    s->frameLeft -= 1;
    for (Part& p : s->part) {
      if (p.phase >= 1) p.phase -= 1;
    }
  }
  return n;
}

Length measure(const std::vector<uint8_t>& data) {
  Length len;
  std::string why;
  Song* s = load(data, why);
  if (!s) return len;
  // run the sequencer alone, tick by tick, for at most 30 minutes of song and as many
  // commands as no real song comes near (a damaged one can loop inside a call for ever,
  // or spend 100000 commands on every tick)
  const double maxSeconds = 30 * 60;
  const long long maxOps = 50000000;
  double t = 0;
  while (t < maxSeconds && s->ops < maxOps) {
    tick(*s);
    t += 60.0 / (s->tempo * 24.0);
    bool any = false, all = true;
    for (Part& p : s->part) {
      if (!p.used) continue;
      if (!p.ended) any = true;
      if (!p.ended && !p.looped) all = false;
    }
    if (!any) { len.seconds = t + 0.2; break; }
    if (all) { len.seconds = t; len.loops = true; break; }
  }
  if (len.seconds == 0) len.seconds = t;
  destroy(s);
  return len;
}

}  // namespace piece
