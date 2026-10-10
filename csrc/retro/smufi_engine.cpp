// SPDX-License-Identifier: GPL-3.0-or-later
// Picatune (.smufi): BeRo's small synthesizer tracker of 2006 (not Picatune 2, which is
// recognised and refused). Written for this player in a clean room, from a description
// of the format only.
//
// A song is an XML text: operators placed on a grid (generators, mixers, delays,
// distortion, filters, compressors, a master), each taking its input from the operator
// right above it; a mixer is two places wide and takes two. A generator has three
// oscillators (sine with feedback, slope, square, noise), an envelope, and its own order
// list and patterns of 16 rows. The synthesizer is mono and makes 32-bit floats at
// 44100 Hz, one row at a time, rounding toward zero while it computes a row; after the
// song the sound rings out for a while and fades. A windowed-sinc resampler (32 taps, 512
// phases) brings it to the output rate.
//
// The output is meant to come out the same bit for bit on x86-64 with glibc: the
// arithmetic follows the description's order of operations in doubles, the oscillator
// steps are computed in the x87's extended precision with glibc's exp2l, and sin, cos and
// log are glibc's. Elsewhere the music is the same but the last bits may differ.
//
// Knobs, for comparisons:
//   BBMP_RETRO_PICATUNE_NOTEBASE=<n>   note base instead of 46 (45: the pitch of the
//                                      earliest exported programs)
//   BBMP_RETRO_PICATUNE_BARE           no ring-out, no fade-out, no release at the end
//   BBMP_RETRO_PICATUNE_FADEIN=<n>     fade-in over n samples (0 or less: none)
//   BBMP_RETRO_PICATUNE_STREAM=<file>  the synthesizer's 44100 Hz floats, from the start
//                                      to the end, are written to <file> as they are made

#include <cfenv>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "engine.h"

#ifdef __clang__
#pragma STDC FENV_ACCESS ON
#endif

using namespace retro;

namespace {

const int kRate = 44100;
const int kRows = 16;
const long long kMaxElements = 2000000;
const size_t kMaxOpen = 64;
const size_t kMaxFile = 64u << 20;
const int kMaxOperators = 4096;
const long long kMemoryLimit = 1LL << 27;
const long long kMaxSong = 6LL * 3600 * kRate;

// ------------------------------------------------------------------- the XML reader

struct Node {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attrs;
  std::vector<size_t> kids;
  const std::string* attr(const char* key) const {
    for (const auto& a : attrs)
      if (a.first == key) return &a.second;
    return nullptr;
  }
};

bool nameChar(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-' || c == ':' || c == '.';
}
bool blank(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

void putUtf8(std::string& out, unsigned long cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | cp >> 6);
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | cp >> 12);
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x110000) {
    out += static_cast<char>(0xF0 | cp >> 18);
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

std::string unescape(const std::string& s) {
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '&') {
      const size_t semi = s.find(';', i + 1);
      if (semi != std::string::npos && semi - i <= 10) {
        const std::string name = s.substr(i + 1, semi - i - 1);
        const char* named = name == "amp" ? "&" : name == "lt" ? "<" : name == "gt" ? ">"
                          : name == "quot" ? "\"" : name == "apos" ? "'" : nullptr;
        if (named) {
          out += named;
          i = semi + 1;
          continue;
        }
        if (name.size() >= 2 && name[0] == '#') {
          const bool hex = name[1] == 'x';
          const std::string digits = name.substr(hex ? 2 : 1);
          putUtf8(out, std::strtoul(digits.c_str(), nullptr, hex ? 16 : 10));
          i = semi + 1;
          continue;
        }
      }
    }
    out += s[i++];
  }
  return out;
}

// Reads the whole text into <nodes> (node 0 is the invisible root). False on an error.
bool readXml(const std::vector<uint8_t>& t, std::vector<Node>& nodes) {
  const size_t n = t.size();
  nodes.assign(1, Node());
  std::vector<size_t> open{0};
  long long elements = 0;
  size_t i = 0;
  while (i < n) {
    if (t[i] != '<') {
      i++;
      continue;
    }
    size_t p = i + 1;
    if (p < n && (t[p] == '?' || t[p] == '!')) {
      const bool comment = n - p >= 3 && t[p + 1] == '-' && t[p + 2] == '-';
      size_t k = p + 1;
      while (k < n && !(t[k] == '>' && (!comment || (t[k - 1] == '-' && t[k - 2] == '-')))) k++;
      if (k >= n) return true;
      i = k + 1;
      continue;
    }
    if (p < n && t[p] == '/') {
      size_t q = p + 1;
      while (q < n && nameChar(t[q])) q++;
      const std::string name(t.begin() + p + 1, t.begin() + q);
      while (q < n && t[q] != '>') q++;
      if (open.size() > 1 && !name.empty() && nodes[open.back()].name == name) open.pop_back();
      i = q < n ? q + 1 : n;
      continue;
    }
    size_t q = p;
    while (q < n && nameChar(t[q])) q++;
    if (q == p) return false;
    if (++elements > kMaxElements) return false;
    Node node;
    node.name.assign(t.begin() + p, t.begin() + q);
    bool selfClosing = false;
    for (;;) {
      if (q >= n) return false;
      const unsigned char c = t[q];
      if (c == '>') {
        q++;
        break;
      }
      if (c == '/') {
        selfClosing = true;
        q++;
        continue;
      }
      if (blank(c)) {
        q++;
        continue;
      }
      const size_t a = q;
      while (q < n && nameChar(t[q])) q++;
      if (q == a) return false;
      std::string key(t.begin() + a, t.begin() + q);
      while (q < n && blank(t[q])) q++;
      if (q >= n || t[q] != '=') continue;
      q++;
      while (q < n && blank(t[q])) q++;
      if (q >= n || (t[q] != '"' && t[q] != '\'')) return false;
      const uint8_t quote = t[q];
      size_t e = q + 1;
      while (e < n && t[e] != quote) e++;
      if (e >= n) return false;
      std::string value = unescape(std::string(t.begin() + q + 1, t.begin() + e));
      q = e + 1;
      bool replaced = false;
      for (auto& kv : node.attrs)
        if (kv.first == key) {
          kv.second = std::move(value);
          replaced = true;
          break;
        }
      if (!replaced) node.attrs.emplace_back(std::move(key), std::move(value));
    }
    const size_t parent = open.back();
    nodes.push_back(std::move(node));
    const size_t me = nodes.size() - 1;
    nodes[parent].kids.push_back(me);
    if (!selfClosing) {
      if (open.size() >= kMaxOpen) return false;
      open.push_back(me);
    }
    i = q;
  }
  return true;
}

// Numbers out of attribute values.
double realOf(const Node& e, const char* key, double dflt) {
  const std::string* s = e.attr(key);
  if (!s) return dflt;
  std::string text = *s;
  for (char& c : text)
    if (c == ',') c = '.';
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || std::isinf(v) || std::isnan(v)) return dflt;
  return v;
}
float singleOf(const Node& e, const char* key, double dflt) {
  return static_cast<float>(realOf(e, key, dflt));
}
long long wholeOf(const Node& e, const char* key, long long dflt) {
  const std::string* s = e.attr(key);
  if (!s) return dflt;
  char* end = nullptr;
  const long long v = std::strtoll(s->c_str(), &end, 10);
  return end == s->c_str() ? dflt : v;
}
std::string textOf(const Node& e, const char* key) {
  const std::string* s = e.attr(key);
  return s ? *s : std::string();
}
uint8_t lowByte(long long v) { return static_cast<uint8_t>(static_cast<unsigned long long>(v) & 0xFF); }
int8_t signedByte(long long v) { return static_cast<int8_t>(lowByte(v)); }

// ------------------------------------------------------------- numbers of the sound

// The extended-precision part, where the description asks for it.
#if LDBL_MANT_DIG == 64
typedef long double Extended;
#else
typedef double Extended;   // not bit-exact, but close
#endif

const float kStep0 = 440.0f / 44100.0f;   // 0x3c2377ee

// An oscillator step for semitone offset <k> and finetune <f>, computed the way the
// table of the description was made: extended precision, rounding toward zero.
float stepFor(int k, int f) {
  const int saved = std::fegetround();
  std::fesetround(FE_TOWARDZERO);
  volatile Extended x = (static_cast<Extended>(k) + static_cast<Extended>(f) / 128) / 12;
#if LDBL_MANT_DIG == 64
  volatile Extended e = exp2l(x);
#else
  volatile Extended e = std::exp2(x);
#endif
  volatile Extended m = e * static_cast<Extended>(kStep0);
  volatile float s = static_cast<float>(m);
  std::fesetround(saved);
  return s;
}

// A single read as its bit pattern, and back.
float fromBits(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

// ------------------------------------------------------------------- the song

enum Kind { MASTER, NOP, MIX, DELAY, DISTORTION, FILTER, COMPRESSOR, GENERATOR };
enum Note { NO_NOTE = 0, NOTE_OFF = 200, NOTE_CUT = 201 };
enum EnvState { IDLE, ATTACK, DECAY, SUSTAIN, RELEASE };

struct Row {
  uint8_t note = NO_NOTE;   // 1..120, NOTE_OFF, NOTE_CUT
  uint8_t volume = 255;
};

struct Oscillator {
  uint8_t mode = 0, waveform = 0, phaseStart = 0;
  int8_t transpose = 0, finetune = 0, color = 0;
  float volume = 1;
  float width = 0, slopeA = 0, slopeB = 0, feedback = 0;
  float step = 0, phase = 0, last = 0;
};

struct Generator {
  long long attack = 1, decay = 1, release = 1;
  float sustain = 0.5f, phaseFactor = 1;
  bool continuous = true;
  Oscillator osc[3];
  uint8_t order[256];
  bool given[256] = {};
  Row patterns[256][kRows];
  float volume = 1;
  EnvState env = IDLE;
  long long counter = 0;
  float level = 0;
  Generator() { std::memset(order, 255, sizeof(order)); }
};

struct Operator {
  Kind kind = NOP;
  int32_t x = 0, y = 0;
  int in1 = -1, in2 = -1;
  float a = 1, b = 1, c = 1;        // mix: L R; distortion: A C; compressor: T Q G
  float feedback = 0.5f;            // delay
  std::vector<float> ring;
  size_t ringPos = 0;
  float cutoff = 1, resonance = 0, amount = 0, s1 = 0, s2 = 0;
  bool highpass = false;
  std::unique_ptr<Generator> gen;
  std::vector<float> out;
};

int noteOf(const std::string* s) {
  if (!s || s->empty() || *s == "...") return NO_NOTE;
  if (*s == "off") return NOTE_OFF;
  if (*s == "cut") return NOTE_CUT;
  if (s->size() != 3) return NO_NOTE;
  static const char* names[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
  const char d = (*s)[2];
  if (d < '0' || d > '9') return NO_NOTE;
  for (int i = 0; i < 12; i++)
    if ((*s)[0] == names[i][0] && (*s)[1] == names[i][1]) return 1 + 12 * (d - '0') + i;
  return NO_NOTE;
}

long long envelopeTime(const Node& e, const char* key) {
  long long ms = wholeOf(e, key, 50);
  if (ms < 0) ms = 0;
  if (ms > 100000000) return 2000000000;    // far past the limit either way
  long long n = ms * kRate / 1000;
  if (n < 1) n = 1;
  if (n > 2000000000) n = 2000000000;
  return n;
}

// The clip of the description: ((|v - lo| + lo) + hi - |v - hi|) / 2, step by step.
inline double clip(double v, double lo, double hi) {
  double p = v - lo;
  double q = std::fabs(p) + lo;
  q = q + hi;
  double r = v - hi;
  double s = q - std::fabs(r);
  return s * 0.5;
}

// ------------------------------------------------------------------- the engine

class Picatune : public Engine {
 public:
  ~Picatune() override {
    if (stream_) std::fclose(stream_);
  }

  bool open(const std::vector<uint8_t>& file, const std::string&, int track, int rate, SongInfo& info,
            std::string& error) override {
    error = load(file, track, info);
    if (!error.empty()) return false;
    rate_ = rate;
    frames_ = end_ * rate / kRate;
    info.tracks = 1;
    info.type = "Picatune";
    info.duration_ms = frames_ * 1000 / rate;
    if (rate != kRate) makeTable();
    if (const char* path = std::getenv("BBMP_RETRO_PICATUNE_STREAM"))
      if (*path) stream_ = std::fopen(path, "wb");
    return true;
  }

  size_t render(int16_t* out, size_t frames) override {
    size_t made = 0;
    while (made < frames && next_ < frames_) {
      const float s = frameAt(next_++);
      const float sigma = s * 32767.0f;
      int16_t v;
      if (sigma >= 32767.0f) v = 32767;
      else if (sigma <= -32768.0f) v = -32768;
      else v = static_cast<int16_t>(std::lrintf(sigma));
      out[2 * made] = out[2 * made + 1] = v;
      made++;
    }
    if (made < frames) {
      while (rowStart_ < end_) nextRow();    // the stream to its end, for the record
    }
    return made;
  }

 private:
  std::vector<Operator> ops_;
  int master_ = -1;
  std::vector<int> runOrder_;
  long long tick_ = 0, rowLen_ = 0, orders_ = 1;
  long long songEnd_ = 0, end_ = 0, fadeOut_ = 0, fadeIn_ = 0;
  int noteBase_ = 46;
  bool bare_ = false;
  // song state
  int order_ = 0, row_ = 0;
  bool ended_ = false, released_ = false;
  long long fadeCount_ = 0;
  uint32_t seed_ = 0x12345678;
  std::map<int, float> steps_;
  // the stream: buf_[0] is stream position bufStart_
  std::vector<float> buf_;
  long long bufStart_ = 0, rowStart_ = 0;
  std::FILE* stream_ = nullptr;
  // output
  int rate_ = kRate;
  long long frames_ = 0, next_ = 0;
  std::vector<float> table_;

  std::string load(const std::vector<uint8_t>& file, int track, SongInfo& info) {
    {
      const size_t head = file.size() < 4096 ? file.size() : 4096;
      const std::string start(file.begin(), file.begin() + head);
      if (start.find("<track") == std::string::npos || start.find("rowhighliohtingminor=") == std::string::npos)
        return "not a Picatune song";
    }
    if (file.size() > kMaxFile) return "too big for a Picatune song";
    std::vector<Node> nodes;
    if (!readXml(file, nodes)) return "the XML of this Picatune song is broken";
    const Node* trk = nullptr;
    for (size_t k : nodes[0].kids)
      if (nodes[k].name == "track") {
        trk = &nodes[k];
        break;
      }
    if (!trk) return "the XML of this Picatune song is broken";
    std::vector<const Node*> opNodes;
    for (size_t k : trk->kids)
      if (nodes[k].name == "operator") opNodes.push_back(&nodes[k]);

    for (const char* a : {"patternlength", "ticksperrow", "rowsperbeat", "inaccuratebpm", "doloop"})
      if (trk->attr(a)) return pt2(a);
    static const char* kinds[8] = {"master", "nop", "mix", "delay", "distortion", "filter", "compressor", "generator"};
    std::vector<int> kindOf;
    for (const Node* o : opNodes) {
      const std::string type = textOf(*o, "type");
      int kind = -1;
      for (int k = 0; k < 8; k++)
        if (type == kinds[k]) kind = k;
      if (kind < 0) return pt2("an operator of a kind Picatune 1 doesn't have");
      for (const char* a : {"syncto", "amprange", "synctonote"})
        if (o->attr(a)) return pt2(a);
      kindOf.push_back(kind);
    }
    if (track != 0) return "no such track";

    const long long bpm = wholeOf(*trk, "bpm", 125), speed = wholeOf(*trk, "speed", 6);
    if (bpm < 1 || bpm > 255 || speed < 1 || speed > 255) return "tempo out of range";
    tick_ = (44100LL * 640) / (bpm * 256);
    rowLen_ = tick_ * speed;
    const long long fadeTicks = wholeOf(*trk, "fadeinticks", 0);
    fadeIn_ = fadeTicks > 0 && fadeTicks < 1000000 ? fadeTicks * tick_ : 0;
    if (const char* f = std::getenv("BBMP_RETRO_PICATUNE_FADEIN")) {
      const long v = std::atol(f);
      fadeIn_ = v > 0 ? v : 0;
    }
    if (const char* nb = std::getenv("BBMP_RETRO_PICATUNE_NOTEBASE")) noteBase_ = std::atoi(nb);
    bare_ = std::getenv("BBMP_RETRO_PICATUNE_BARE") != nullptr;

    long long delaySum = 0;
    for (size_t i = 0; i < opNodes.size(); i++) {
      if (static_cast<int>(i) >= kMaxOperators) return "too many operators";
      const Node& e = *opNodes[i];
      ops_.emplace_back();
      Operator& op = ops_.back();
      op.kind = static_cast<Kind>(kindOf[i]);
      op.x = static_cast<int32_t>(static_cast<uint32_t>(static_cast<unsigned long long>(wholeOf(e, "x", 0))));
      op.y = static_cast<int32_t>(static_cast<uint32_t>(static_cast<unsigned long long>(wholeOf(e, "y", 0))));
      switch (op.kind) {
        case MASTER:
          if (master_ < 0) master_ = static_cast<int>(i);
          break;
        case NOP:
          break;
        case MIX:
          op.a = singleOf(e, "amplifyleft", 1);
          op.b = singleOf(e, "amplifyright", 1);
          break;
        case DELAY: {
          op.feedback = singleOf(e, "feedback", 0.5);
          const long long ticks = wholeOf(e, "ticks", 6), ms = wholeOf(e, "milliseconds", 0);
          if (ticks < 0 || ticks > 100000 || ms < 0 || ms > 600000) return "a delay out of range";
          const long long len = ms * kRate / 1000 + tick_ * ticks;
          delaySum += len;
          if (len > 67108864 || delaySum > kMemoryLimit) return "a delay out of range";
          op.ring.assign(len > 1 ? len : 1, 0.0f);
          break;
        }
        case DISTORTION:
          op.a = singleOf(e, "amplify", 1);
          op.c = singleOf(e, "clipat", 1);
          break;
        case FILTER: {
          op.cutoff = singleOf(e, "cutoff", 1);
          op.resonance = singleOf(e, "resonance", 0);
          op.highpass = textOf(e, "filtertype") == "highpass";
          if (op.cutoff == 1.0f) {
            op.amount = static_cast<float>(static_cast<double>(op.resonance) * 2);
          } else {
            volatile Extended r = op.resonance, c = op.cutoff;
            volatile Extended q = r / (1 - c);
            volatile Extended sum = q + r;
            op.amount = static_cast<float>(sum);
          }
          break;
        }
        case COMPRESSOR:
          op.a = singleOf(e, "threshold", 1);
          op.b = singleOf(e, "ratio", 1);
          op.c = singleOf(e, "gain", 1);
          if (op.b == 0) op.b = 1;
          break;
        case GENERATOR:
          op.gen.reset(new Generator);
          readGenerator(e, nodes, *op.gen);
          info.names.push_back(textOf(e, "name"));
          break;
      }
    }
    if (master_ < 0) return "no master operator";
    long long memory = (static_cast<long long>(ops_.size()) + 2) * rowLen_;
    for (const Operator& op : ops_) memory += static_cast<long long>(op.ring.size());
    if (memory > kMemoryLimit) return "too slow or too many operators for a Picatune song";
    if (!wire()) return "the operators feed each other in a circle";

    int top = -1;
    for (const Operator& op : ops_)
      if (op.gen)
        for (int s = 0; s < 256; s++)
          if (op.gen->order[s] != 255 && s > top) top = s;
    orders_ = top + 1 > 0 ? top + 1 : 1;
    songEnd_ = orders_ * kRows * rowLen_;
    if (songEnd_ > kMaxSong) return "too long for a Picatune song";
    ringOut();

    info.title = textOf(*trk, "name");
    info.author = textOf(*trk, "author");
    std::vector<std::string> names;
    for (auto& n : info.names)
      if (!n.empty()) names.push_back(n);
    info.names = names;
    for (int i : runOrder_) ops_[i].out.assign(rowLen_, 0.0f);
    return "";
  }

  static std::string pt2(const std::string& what) {
    return "a Picatune 2 song (" + what + "), which this Picatune 1 replayer can't play";
  }

  void readGenerator(const Node& e, const std::vector<Node>& nodes, Generator& g) {
    g.attack = envelopeTime(e, "attack");
    g.decay = envelopeTime(e, "decay");
    g.release = envelopeTime(e, "release");
    g.sustain = singleOf(e, "sustain", 0.5);
    g.continuous = wholeOf(e, "continuous", 1) != 0;
    g.phaseFactor = singleOf(e, "phasefactor", 1);
    g.osc[0].mode = 1;
    for (size_t k : e.kids) {
      const Node& c = nodes[k];
      if (c.name == "oscillator") {
        const long long i = wholeOf(c, "index", -1);
        if (i < 0 || i > 2) continue;
        Oscillator& o = g.osc[i];
        o.waveform = lowByte(wholeOf(c, "waveform", o.waveform));
        o.mode = lowByte(wholeOf(c, "mode", o.mode));
        o.transpose = signedByte(wholeOf(c, "transpose", 0));
        o.finetune = signedByte(wholeOf(c, "finetune", 0));
        o.phaseStart = lowByte(wholeOf(c, "phasestart", 0));
        o.volume = singleOf(c, "volume", 1);
        o.color = signedByte(wholeOf(c, "color", 0));
      } else if (c.name == "order") {
        const long long i = wholeOf(c, "index", -1);
        if (i < 0 || i > 255) continue;
        g.order[i] = lowByte(wholeOf(c, "pattern", 255));
      } else if (c.name == "pattern") {
        const long long i = wholeOf(c, "index", -1);
        if (i < 0 || i > 255) continue;
        g.given[i] = true;
        for (Row& r : g.patterns[i]) r = Row();
        for (size_t rk : c.kids) {
          const Node& r = nodes[rk];
          if (r.name != "row") continue;
          const long long ri = wholeOf(r, "index", -1);
          if (ri < 0 || ri > 15) continue;
          g.patterns[i][ri].note = static_cast<uint8_t>(noteOf(r.attr("note")));
          g.patterns[i][ri].volume = lowByte(wholeOf(r, "volume", 255));
        }
      }
    }
    for (Oscillator& o : g.osc) {
      const int k = o.color;
      volatile Extended w = static_cast<Extended>(k + 128) / 255;
      o.width = static_cast<float>(w);
      if (o.width == 0) {
        o.slopeA = 0;
        o.slopeB = 1;
      } else if (o.width == 1) {
        o.slopeA = 1;
        o.slopeB = 0;
      } else {
        volatile Extended wi = o.width;
        volatile Extended a = 1 / wi, b = 1 / (1 - wi);
        o.slopeA = static_cast<float>(a);
        o.slopeB = static_cast<float>(b);
      }
      o.feedback = static_cast<float>(k) / 128.0f;
    }
  }

  // The grid: who takes its input from whom, and the order in which they run.
  bool wire() {
    std::map<std::pair<int32_t, int32_t>, int> grid;
    auto right = [](int32_t v, int d) { return static_cast<int32_t>(static_cast<uint32_t>(v) + d); };
    for (size_t i = 0; i < ops_.size(); i++) {
      grid.insert({{ops_[i].x, ops_[i].y}, static_cast<int>(i)});
      if (ops_[i].kind == MIX) grid.insert({{right(ops_[i].x, 1), ops_[i].y}, static_cast<int>(i)});
    }
    for (Operator& op : ops_) {
      const int32_t above = right(op.y, -1);
      auto f = grid.find({op.x, above});
      if (f != grid.end()) op.in1 = f->second;
      if (op.kind == MIX) {
        f = grid.find({right(op.x, 1), above});
        if (f != grid.end()) op.in2 = f->second;
      }
    }
    // depth first from the master, the first input before the second
    std::vector<uint8_t> state(ops_.size(), 0);   // 0 not met, 1 on the path, 2 done
    struct Frame { int op, stage; };
    std::vector<Frame> stack{{master_, 0}};
    state[master_] = 1;
    while (!stack.empty()) {
      Frame& f = stack.back();
      if (f.stage < 2) {
        const int in = f.stage == 0 ? ops_[f.op].in1 : ops_[f.op].in2;
        f.stage++;
        if (in < 0) continue;
        if (state[in] == 1) return false;
        if (state[in] == 2) continue;
        state[in] = 1;
        stack.push_back({in, 0});
      } else {
        state[f.op] = 2;
        runOrder_.push_back(f.op);
        stack.pop_back();
      }
    }
    return true;
  }

  void ringOut() {
    double rmax = 0, emax = 0;
    for (const Operator& op : ops_) {
      if (op.gen && op.gen->release > rmax) rmax = static_cast<double>(op.gen->release);
      if (op.kind == DELAY) {
        const double g = std::fabs(static_cast<double>(op.feedback));
        const double m = g <= 0.001 ? 1 : g >= 0.999 ? 1e9 : std::log(0.001) / std::log(g);
        double echo = m * static_cast<double>(op.ring.size());
        if (echo > 1e9) echo = 1e9;
        echo = std::trunc(echo);
        if (echo > emax) emax = echo;
      }
    }
    double ring = rmax + emax + 11025;
    if (ring < 22050) ring = 22050;
    if (ring > 8.0 * kRate) ring = 8.0 * kRate;
    long long ringOut = static_cast<long long>(ring);
    if (bare_) ringOut = 0;
    end_ = songEnd_ + ringOut;
    fadeOut_ = bare_ ? 0 : (ringOut < 2 * kRate ? ringOut : 2 * kRate);
  }

  float step(int k, int f) {
    const int key = k * 256 + f;
    auto it = steps_.find(key);
    if (it != steps_.end()) return it->second;
    const float s = stepFor(k, f);
    steps_[key] = s;
    return s;
  }

  // ------------------------------------------------------------- one row

  void startRow(Generator& g) {
    Row r;
    if (!ended_ && g.order[order_] != 255 && g.given[g.order[order_]]) r = g.patterns[g.order[order_]][row_];
    if (r.note == NOTE_CUT) {
      g.env = IDLE;
      g.counter = 0;
      g.level = 0;
    } else if (r.note == NOTE_OFF) {
      if (g.env != IDLE && g.env != RELEASE) {
        g.env = RELEASE;
        g.counter = 0;
      }
    } else if (r.note != NO_NOTE) {
      for (Oscillator& o : g.osc) {
        if (o.mode == 0) continue;
        o.step = step(r.note + o.transpose - noteBase_, o.finetune);
        o.phase = static_cast<float>(o.phaseStart / 255.0);
      }
      g.volume = 1;
      g.env = ATTACK;
      g.counter = 0;
      g.level = 0;
    }
    if (r.volume <= 64) g.volume = static_cast<float>(r.volume) / 64.0f;
  }

  double envelope(Generator& g) {
    double e = 0;
    switch (g.env) {
      case IDLE:
        break;
      case ATTACK:
        e = static_cast<double>(g.counter) / static_cast<double>(g.attack);
        g.level = static_cast<float>(e);
        if (++g.counter >= g.attack) {
          g.env = DECAY;
          g.counter = 0;
        }
        break;
      case DECAY: {
        const double t = static_cast<double>(g.counter) / static_cast<double>(g.decay);
        const double depth = 1.0 - static_cast<double>(g.sustain);
        e = 1.0 - t * depth;
        g.level = static_cast<float>(e);
        if (++g.counter >= g.decay) {
          g.env = g.continuous ? SUSTAIN : RELEASE;
          g.counter = 0;
        }
        break;
      }
      case SUSTAIN:
        e = g.sustain;
        g.level = g.sustain;
        break;
      case RELEASE: {
        const double t = static_cast<double>(g.counter) / static_cast<double>(g.release);
        e = (1.0 - t) * static_cast<double>(g.level);
        if (++g.counter >= g.release) {
          g.env = IDLE;
          g.counter = 0;
        }
        break;
      }
    }
    return e;
  }

  void generate(Operator& op, const float* in) {
    Generator& g = *op.gen;
    startRow(g);
    const double twoPi = static_cast<double>(fromBits(0x40C90FDB));
    for (long long i = 0; i < rowLen_; i++) {
      float v = in ? in[i] : 0.0f;
      for (Oscillator& o : g.osc) {
        if (o.mode == 0) continue;
        double a = o.phase;
        if (o.mode == 2) a = a + v;
        if (o.mode == 6) a = a * (1.0 + v);
        const float phi = static_cast<float>(a - std::trunc(a));
        const double next = static_cast<double>(o.phase) + o.step;
        o.phase = static_cast<float>(next - std::trunc(next));
        o.step = static_cast<float>(static_cast<double>(o.step) * g.phaseFactor);
        double w;
        switch (o.waveform) {
          case 0: {
            const double arg = static_cast<double>(o.last) * o.feedback + phi;
            const double fr = arg - std::trunc(arg);
            w = std::sin(fr * twoPi);
            o.last = static_cast<float>(w);
            break;
          }
          case 1:
            if (phi < o.width) w = static_cast<double>(phi) * o.slopeA;
            else w = 1.0 - (static_cast<double>(phi) - o.width) * o.slopeB;
            w = w * 2.0;
            w = w - 1.0;
            break;
          case 2:
            w = std::signbit(static_cast<float>(static_cast<double>(phi) - o.width)) ? -1.0 : 1.0;
            break;
          case 3:
            seed_ = seed_ * 0x30091895u + 0x19850930u;
            w = static_cast<double>(fromBits((seed_ & 0x007FFFFFu) | 0x40000000u)) - 3.0;
            break;
          default:
            w = 0;
        }
        w = w * o.volume;
        if (o.mode == 3) w = v + w;
        else if (o.mode == 4) w = v - w;
        else if (o.mode == 5) w = v * w;
        else if (o.mode >= 7) w = v;
        v = static_cast<float>(w);
      }
      const double e = envelope(g);
      op.out[i] = static_cast<float>((static_cast<double>(v) * e) * g.volume);
    }
  }

  void compute(Operator& op) {
    const float* x = op.in1 >= 0 ? ops_[op.in1].out.data() : nullptr;
    const float* y = op.in2 >= 0 ? ops_[op.in2].out.data() : nullptr;
    float* out = op.out.data();
    const long long n = rowLen_;
    switch (op.kind) {
      case MASTER:
      case NOP:
        for (long long i = 0; i < n; i++) out[i] = x ? x[i] : 0.0f;
        break;
      case MIX:
        for (long long i = 0; i < n; i++) {
          const double l = static_cast<double>(x ? x[i] : 0.0f) * op.a;
          const double r = static_cast<double>(y ? y[i] : 0.0f) * op.b;
          out[i] = static_cast<float>(l + r);
        }
        break;
      case DELAY:
        for (long long i = 0; i < n; i++) {
          if (++op.ringPos >= op.ring.size()) op.ringPos = 0;
          const double v = static_cast<double>(op.ring[op.ringPos]) * op.feedback + (x ? x[i] : 0.0f);
          op.ring[op.ringPos] = out[i] = static_cast<float>(v);
        }
        break;
      case DISTORTION:
        for (long long i = 0; i < n; i++) {
          const double v = static_cast<double>(x ? x[i] : 0.0f) * op.a;
          out[i] = static_cast<float>(clip(v, -static_cast<double>(op.c), op.c));
        }
        break;
      case FILTER:
        for (long long i = 0; i < n; i++) {
          const double t = x ? x[i] : 0.0f;
          const double d1 = t - op.s1;
          const double d2 = static_cast<double>(op.s1) - op.s2;
          const double fb = d2 * op.amount;
          const double sum = d1 + fb;
          const double scaled = sum * op.cutoff;
          op.s1 = static_cast<float>(scaled + op.s1);
          const double e = static_cast<double>(op.s1) - op.s2;
          op.s2 = static_cast<float>(e * op.cutoff + op.s2);
          out[i] = op.highpass ? static_cast<float>(clip(t - op.s2, -1.0, 1.0))
                               : static_cast<float>(clip(op.s2, -1.0, 1.0));
        }
        break;
      case COMPRESSOR:
        for (long long i = 0; i < n; i++) {
          const float xi = x ? x[i] : 0.0f;
          double u = std::fabs(static_cast<double>(xi));
          if (!(op.a > u)) u = (u - op.a) / op.b + op.a;
          const float w = static_cast<float>(u * op.c);
          out[i] = std::signbit(xi) ? -std::fabs(w) : w;
        }
        break;
      case GENERATOR:
        generate(op, x);
        break;
    }
  }

  // Computes one row of the master's output, faded in, rounding toward zero.
  __attribute__((noinline)) void synthesize(float* dst) {
    for (int i : runOrder_) compute(ops_[i]);
    const float* m = ops_[master_].out.data();
    for (long long i = 0; i < rowLen_; i++) {
      float v = m[i];
      if (fadeCount_ < fadeIn_) {
        const double part = static_cast<double>(fadeCount_) / static_cast<double>(fadeIn_);
        v = static_cast<float>(static_cast<double>(v) * part);
        fadeCount_++;
      }
      dst[i] = v;
    }
  }

  // Appends the next row to the stream.
  void nextRow() {
    if (!released_ && rowStart_ >= songEnd_) {
      released_ = true;
      if (!bare_)
        for (Operator& op : ops_)
          if (op.gen && op.gen->env != IDLE && op.gen->env != RELEASE) {
            op.gen->env = RELEASE;
            op.gen->counter = 0;
          }
    }
    // keep the last 32 samples for the resampler
    const long long have = static_cast<long long>(buf_.size());
    const long long keep = have < 32 ? have : 32;
    std::vector<float> nb(keep + rowLen_);
    std::copy(buf_.end() - keep, buf_.end(), nb.begin());
    bufStart_ = bufStart_ + have - keep;
    float* dst = nb.data() + keep;
    const int saved = std::fegetround();
    std::fesetround(FE_TOWARDZERO);
    synthesize(dst);
    std::fesetround(saved);
    if (!ended_) {
      if (++row_ == kRows) {
        row_ = 0;
        if (++order_ >= orders_) {
          order_ = 0;
          ended_ = true;
        }
      }
    }
    long long valid = rowLen_;
    for (long long i = 0; i < rowLen_; i++) {
      const long long left = end_ - (rowStart_ + i);
      if (left <= 0) {
        dst[i] = 0;
        if (i < valid) valid = i;
      } else if (left < fadeOut_) {
        dst[i] = dst[i] * (static_cast<float>(left) / static_cast<float>(fadeOut_));
      }
    }
    if (stream_ && valid > 0) std::fwrite(dst, sizeof(float), static_cast<size_t>(valid), stream_);
    buf_.swap(nb);
    rowStart_ += rowLen_;
  }

  float sample(long long pos) {
    if (pos < 0 || pos >= end_) return 0;
    while (pos >= rowStart_) nextRow();
    return buf_[pos - bufStart_];
  }

  void makeTable() {
    const double pi = 3.141592653589793;
    const double ratio = static_cast<double>(rate_) / kRate;
    const double kappa = rate_ < kRate ? ratio * 0.95 : 0.95;
    table_.assign(513 * 32, 0.0f);
    for (int p = 0; p <= 512; p++) {
      double taps[32], sum = 0;
      for (int t = 0; t < 32; t++) {
        const double z = static_cast<double>(t - 15) - p / 512.0;
        const double window = 0.5 + 0.5 * std::cos(pi * z / 16);
        double sinc = 1;
        if (z != 0) {
          const double arg = pi * z * kappa;
          sinc = std::sin(arg) / arg;
        }
        taps[t] = std::fabs(z) >= 16 ? 0 : sinc * window;
        sum += taps[t];
      }
      for (int t = 0; t < 32; t++) table_[p * 32 + t] = static_cast<float>(taps[t] / sum);
    }
  }

  float frameAt(long long j) {
    if (rate_ == kRate) return sample(j);
    const long long q = j * kRate;
    const long long whole = q / rate_, rem = q % rate_;
    const double u = (static_cast<double>(rem) / rate_) * 512;
    const int p = static_cast<int>(u);
    const float mu = static_cast<float>(u - p);
    const float* r0 = &table_[p * 32];
    const float* r1 = &table_[(p + 1) * 32];
    sample(whole + 16 < end_ ? whole + 16 : end_ - 1);   // the rows the taps need
    float s = 0;
    for (int t = 0; t < 32; t++) {
      const float c = r0[t] + (r1[t] - r0[t]) * mu;
      s = s + sample(whole - 15 + t) * c;
    }
    return s;
  }
};

}  // namespace

REGISTER_ENGINE("picatune", Picatune)
