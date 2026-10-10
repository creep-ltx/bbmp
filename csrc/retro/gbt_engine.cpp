// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-retro's engine "mgb": Gameboy Tracker songs (.mgb), the tracker for the Game Boy by
// Paragon 5 (Stephane Hockenhull).
//
// Written for this player in a clean room, from a description of the format only.
//
// A song file is four kinds of block, each with a signature, a version and a magic text:
// the song (title, order list), its patterns (rows of four cells: two pulse channels, the
// wave channel and the noise channel), its instruments (small scripts of text lines, one
// line per frame) and its waves (32 hexadecimal digits). Most texts are enciphered with a
// running key. The engine reads the file, compiles the instrument scripts and the patterns
// the order list names into the byte streams that the SoundSystem replay driver (MIT,
// assembled at build time into mgb_driver.h) plays, puts driver and streams into a GBS
// image in memory and has Game Music Emu play it.
//
// A song that repeats is played once through and faded over 4 seconds; a song that ends
// rings out for a second and fades over one more.
//
// For comparisons: with BBMP_RETRO_MGB_DUMP=<file> the GBS image is written to <file>.

#include <gme/gme.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "engine.h"
#include "mgb_driver.h"

using namespace retro;

namespace {

const uint32_t kCipherA = 0xA8C7A6C7, kCipherB = 0xEBD876CC, kCipherC = 0xD5BFB20C;
const uint32_t kCipherD = 0xD5BFB20C;   // the same number, but the key rotates

const uint32_t kSongSig = 0x00010000, kInstrumentSig = 0x00010001;
const uint32_t kPatternSig = 0x00010002, kWaveSig = 0x00010003;
const char kSongMagic[] = "GameBoy Music Module";
const char kPatternMagic[] = "GameBoy Music Pattern";
const char kInstrumentMagic[] = "GameBoy Instrument";
const char kWaveMagic[] = "GameBoy Audio Wave";

const size_t kBank = 0x4000;
const size_t kDriverSize = 0x3C00;   // the driver from $0400 to $3FFF
const size_t kHeader = 0x70;
const int kMostBanks = 31;

// The 72 note frequencies, c-0 to b-5.
const uint16_t kNotes[72] = {
    0x020, 0x091, 0x0FC, 0x160, 0x1C0, 0x219, 0x26E, 0x2BE, 0x30A, 0x351, 0x394, 0x3D4,
    0x410, 0x448, 0x47E, 0x4B0, 0x4E0, 0x50D, 0x537, 0x55F, 0x585, 0x5A8, 0x5CA, 0x5EA,
    0x608, 0x624, 0x63F, 0x658, 0x670, 0x686, 0x69C, 0x6B0, 0x6C2, 0x6D4, 0x6E5, 0x6F5,
    0x704, 0x712, 0x71F, 0x72C, 0x738, 0x743, 0x74E, 0x758, 0x761, 0x76A, 0x773, 0x77A,
    0x782, 0x789, 0x790, 0x796, 0x79C, 0x7A2, 0x7A7, 0x7AC, 0x7B1, 0x7B5, 0x7B9, 0x7BD,
    0x7C1, 0x7C5, 0x7C8, 0x7CB, 0x7CE, 0x7D1, 0x7D3, 0x7D6, 0x7D8, 0x7DB, 0x7DD, 0x7DF};
const char* const kNoteNames[12] = {"c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b"};
// The wave channel's output levels.
const uint8_t kWaveLevel[4] = {0x00, 0x60, 0x40, 0x20};

// ------------------------------------------------------------------- reading the file

struct Failure {
  std::string why;
};
[[noreturn]] void refuse(const std::string& why) { throw Failure{why}; }

class Reader {
 public:
  explicit Reader(const std::vector<uint8_t>& f) : f_(f) {}
  uint32_t word() {
    if (f_.size() - pos_ < 4) refuse("the file ends too early");
    const uint32_t w = le32(&f_[pos_]);
    pos_ += 4;
    return w;
  }
  void key() { key_ = word(); }
  std::string plain() {
    const uint32_t n = word();
    if (n > f_.size() - pos_) refuse("the file ends too early");
    std::string s(reinterpret_cast<const char*>(&f_[pos_]), n);
    pos_ += n;
    return s;
  }
  // An enciphered text: each byte against the running key, which moves on a step a byte
  // (shifted left, or with <rotate> rotated left) and keeps going into the next text.
  std::string secret(uint32_t constant, bool rotate = false) {
    std::string s = plain();
    for (size_t i = 0; i < s.size(); i++) {
      const uint32_t at = static_cast<uint32_t>(i);
      s[i] = static_cast<char>(static_cast<uint8_t>(s[i]) ^ ((key_ ^ at) & 0xFF));
      const uint32_t moved = rotate ? (key_ << 1 | key_ >> 31) : key_ << 1;
      key_ = moved ^ (at + constant);
    }
    return s;
  }

 private:
  const std::vector<uint8_t>& f_;
  size_t pos_ = 0;
  uint32_t key_ = 0;
};

// ------------------------------------------------------------------- texts and numbers

bool trailingBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Up to the first NUL, trailing blanks off, as UTF-8.
std::string shown(const std::string& raw) {
  size_t n = 0;
  while (n < raw.size() && raw[n]) n++;
  while (n > 0 && trailingBlank(raw[n - 1])) n--;
  return from_latin1(raw.substr(0, n));
}

// A decimal number as the C library's strtol reads it, in 64 bits, from <p> up to <end>
// (or the first character that isn't part of it). Beyond the range it sticks at the ends.
long long decimal(const char* p, const char* end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\v' || *p == '\f' || *p == '\r'))
    p++;
  bool minus = false;
  if (p < end && (*p == '+' || *p == '-')) minus = *p++ == '-';
  unsigned long long v = 0;
  bool over = false;
  const unsigned long long most = minus ? 9223372036854775808ULL : 9223372036854775807ULL;
  for (; p < end && *p >= '0' && *p <= '9'; p++) {
    const unsigned d = static_cast<unsigned>(*p - '0');
    if (over || v > (most - d) / 10) {
      over = true;
      continue;
    }
    v = v * 10 + d;
  }
  if (over) v = most;
  return minus ? static_cast<long long>(0ULL - v) : static_cast<long long>(v);
}
long long decimalAt(const std::string& s, size_t at) {
  if (at >= s.size()) return 0;
  return decimal(s.data() + at, s.data() + s.size());
}

// The number written in front of an instrument's or a wave's name ("5:bass", "1F:lead").
int nameDigit(unsigned char c) {
  if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + 32);
  const int v = static_cast<int>(c) - 48;
  return v < 10 ? v : static_cast<int>(c) - 87;
}
bool nameNumber(const std::string& name, int& number) {
  if (name.size() < 2) return false;
  if (name[1] == ':') {
    number = nameDigit(static_cast<unsigned char>(name[0]));
    return true;
  }
  if (name.size() > 2 && name[2] == ':') {
    number = (nameDigit(static_cast<unsigned char>(name[0])) * 16) |
             nameDigit(static_cast<unsigned char>(name[1]));
    return true;
  }
  return false;
}

// ------------------------------------------------------------------- the song as read

struct Pattern {
  int number = 0, speed = 6;
  bool hasRows = false;
  std::string cells;   // rows of 32 bytes
};

struct Instrument {
  std::string name;
  std::vector<std::string> lines[4];
};

struct Wave {
  std::string name, samples;
  int number = 0;
  bool numbered = false;
};

struct Song {
  std::string title, order;
  std::vector<Pattern> patterns;
  std::vector<Instrument> instruments;
  std::vector<Wave> waves;
};

void readSong(const std::vector<uint8_t>& file, Song& song) {
  Reader r(file);
  if (r.word() != kSongSig) refuse("not a Gameboy Tracker module");
  const uint32_t version = r.word();
  if (version != 0x00010010 && version != 0x00010011 && version != 0x00020000 && version != 0x00020001)
    refuse("unknown version of the module");
  if (r.plain() != kSongMagic) refuse("not a Gameboy Tracker module");
  r.key();
  song.title = r.secret(kCipherA);
  r.secret(kCipherA);
  if (version > 0x00010010) r.secret(kCipherA);
  r.plain();
  r.secret(kCipherA);
  song.order = r.secret(kCipherA);
  for (int i = 0; i < 3; i++) r.secret(kCipherA);
  const uint32_t patterns = r.word(), instruments = r.word(), waves = r.word();
  if (patterns > 4096 || instruments > 4096 || waves > 4096) refuse("too many patterns or instruments");

  for (uint32_t n = 0; n < patterns; n++) {
    Pattern p;
    if (r.word() != kPatternSig) refuse("a pattern was expected");
    const uint32_t v = r.word();
    if (v != 0x00010000 && v != 0x00010001 && v != 0x00010002 && v != 0x00020000)
      refuse("unknown version of a pattern");
    std::string magic;
    if (v <= 0x00010001) {
      magic = r.plain();
    } else {
      r.key();
      magic = r.secret(kCipherB);
    }
    if (magic != kPatternMagic) refuse("a pattern was expected");
    r.key();
    p.number = static_cast<int>(n);
    if (v > 0x00010000) p.number = static_cast<int32_t>(r.word());
    int32_t channels = 4;
    if (v == 0x00020000) {
      r.secret(kCipherB);
      r.secret(kCipherB);
      channels = static_cast<int32_t>(r.word());
    }
    if (v > 0x00010000) p.speed = static_cast<int32_t>(r.word());
    if (v == 0x00010002 || v == 0x00020000) {
      r.key();
      p.cells = r.secret(kCipherD, true);
    } else {
      p.cells = r.secret(kCipherC);
    }
    if (channels != 4) refuse("not a Game Boy song: a pattern with " + std::to_string(channels) + " channels");
    if (p.cells.size() % 32 != 0) p.cells.clear();
    p.hasRows = !p.cells.empty();
    song.patterns.push_back(std::move(p));
  }

  for (uint32_t n = 0; n < instruments; n++) {
    Instrument ins;
    if (r.word() != kInstrumentSig) refuse("an instrument was expected");
    const uint32_t v = r.word();
    if (v != 0x00010011 && v != 0x00010012 && v != 0x00020000) refuse("unknown version of an instrument");
    std::string magic;
    if (v == 0x00010011) {
      magic = r.plain();
    } else {
      r.key();
      magic = r.secret(kCipherA);
    }
    if (magic != kInstrumentMagic) refuse("an instrument was expected");
    r.key();
    r.secret(kCipherA);   // a file name
    ins.name = r.secret(kCipherA);
    const uint32_t channels = r.word();
    if (channels > 64) refuse("an instrument with too many channels");
    for (uint32_t ch = 0; ch < channels; ch++) {
      const uint32_t lines = r.word();
      if (lines > 100000) refuse("an instrument with too many lines");
      for (uint32_t l = 0; l < lines; l++) {
        std::string line = r.secret(kCipherA);
        if (ch < 4) ins.lines[ch].push_back(std::move(line));
      }
    }
    if (v == 0x00020000) r.word();
    song.instruments.push_back(std::move(ins));
  }

  for (uint32_t n = 0; n < waves; n++) {
    Wave w;
    if (r.word() != kWaveSig) refuse("a wave was expected");
    const uint32_t v = r.word();
    if (v != 0x00010010 && v != 0x00010020 && v != 0x00020000 && v != 0x00020010)
      refuse("unknown version of a wave");
    std::string magic;
    if (v == 0x00010010) {
      magic = r.plain();
    } else {
      r.key();
      magic = r.secret(kCipherB);
    }
    if (magic != kWaveMagic) refuse("a wave was expected");
    r.key();
    w.name = r.secret(kCipherA);
    w.samples = r.secret(kCipherA);
    if (v >= 0x00020000) {
      r.secret(kCipherA);
      for (int i = 0; i < 5; i++) r.word();
      if (v == 0x00020010) r.word();
      std::string half;
      for (size_t i = 0; i < w.samples.size(); i += 2) half += w.samples[i];
      w.samples = half;
    }
    w.numbered = nameNumber(w.name, w.number);
    song.waves.push_back(std::move(w));
  }
}

// ------------------------------------------------------------------- instrument scripts

typedef std::vector<uint8_t> Bytes;

// A wait of n frames (n at least 1): the low byte, then a pair of zeros per 256 frames.
void putWait(Bytes& out, long long n) {
  out.push_back(0);
  out.push_back(static_cast<uint8_t>(n & 0xFF));
  for (long long i = 0; i < n / 256; i++) {
    out.push_back(0);
    out.push_back(0);
  }
}

// Control and high bytes to spaces, upper case to lower, runs of spaces to one, no blanks
// at either end.
std::string tidy(const std::string& raw) {
  std::string out;
  for (unsigned char c : raw) {
    if (c < 0x20 || c >= 0x80) c = ' ';
    if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + 32);
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out += static_cast<char>(c);
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

int waveDigit(unsigned char c) {
  const int v = static_cast<int>(c) - 48;
  return v < 10 ? v : static_cast<int>(c) - 97;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return 0;
}

Bytes waveBytes(std::string text) {
  for (char& c : text)
    if (c >= ':' && c <= '@') c = static_cast<char>(c + 7);
  Bytes out;
  for (size_t i = 0; i + 1 < text.size(); i += 2)
    out.push_back(static_cast<uint8_t>(hexNibble(text[i]) << 4 | hexNibble(text[i + 1])));
  return out;
}

class ScriptCompiler {
 public:
  ScriptCompiler(const std::vector<Wave>& waves, int channel) : waves_(waves), ch_(channel) {}

  // The script's bytes, or false when the tracker couldn't compile it.
  bool compile(const std::vector<std::string>& lines, Bytes& out) {
    long long previous = 0;
    bool ended = false;
    for (size_t i = 0; i < lines.size(); i++) {
      line_ = tidy(lines[i]);
      if (line_.empty()) continue;
      const long long at = static_cast<long long>(i);
      if (at != previous) putWait(out, at - previous);
      size_t s = 0;
      while (s < line_.size()) {
        size_t e = line_.find(' ', s);
        if (e == std::string::npos) e = line_.size();
        if (!token(s, e, out, ended)) return false;
        s = e + 1;
      }
      previous = at;
    }
    return ended;
  }

 private:
  unsigned char c(size_t at) const { return at < line_.size() ? static_cast<unsigned char>(line_[at]) : 0; }

  // A note written from <at>: the letter, maybe '#', then the octave digit.
  bool note(size_t at, unsigned& lo, unsigned& hi) const {
    std::string name(1, static_cast<char>(c(at)));
    size_t octaveAt = at + 1;
    if (c(at + 1) == '#') {
      name += '#';
      octaveAt = at + 2;
    }
    int index = 0;
    for (int k = 0; k < 12; k++)
      if (name == kNoteNames[k]) index = k;
    const int octave = static_cast<int>(c(octaveAt)) - 48;
    if (octave < 0 || octave > 5) return false;
    const unsigned f = kNotes[index + 12 * octave];
    lo = f & 0xFF;
    hi = (f >> 8) & 7;
    return true;
  }

  const Wave* findWave(int id) const {
    for (const Wave& w : waves_)
      if (w.numbered && w.number == id) return &w;
    return nullptr;
  }

  bool token(size_t s, size_t e, Bytes& out, bool& ended) {
    int colons = 0;
    for (size_t i = s; i < e; i++) colons += line_[i] == ':';
    const unsigned char letter = c(s);
    const bool pulse = ch_ == 0 || ch_ == 1;
    auto put = [&out](unsigned b) { out.push_back(static_cast<uint8_t>(b)); };

    if (letter == 'e' && colons == 0) {   // the end of the script
      put(0x02);
      ended = true;
      return true;
    }
    if (letter == 'e' && colons == 2 && ch_ != 2) {
      put(0x03);
      put((c(s + 2) == 'u' ? 8 : 0) | (c(s + 4) & 7));
      return true;
    }
    if (letter == 'e' && colons == 3 && ch_ != 2) {   // envelope
      const unsigned v = c(s + 2);
      const unsigned high = (v > 0x60 ? v - 0x57 : v) & 15;
      put(0x05);
      put(high * 16 + (c(s + 4) == 'u' ? 8 : 0) + (c(s + 6) & 7));
      return true;
    }
    if (letter == 'g' && colons == 0) {
      put(0x01);
      put(0x80);
      return true;
    }
    if (letter == 'g' && colons == 1 && ch_ != 3) {
      unsigned lo, hi;
      if (!note(s + 2, lo, hi)) return false;
      put(0x04);
      put(lo);
      put(0x80 | hi);
      return true;
    }
    if (letter == 's' && colons == 1) {
      if (c(s + 2) != 'c' && c(s + 2) != 'l') return false;
      put(0x01);
      put(c(s + 2) == 'c' ? 0x80 : 0xC0);
      return true;
    }
    if (letter == 's' && colons == 2 && ch_ != 3) {
      unsigned lo, hi;
      if (!note(s + 4, lo, hi)) return false;
      if (c(s + 2) != 'c' && c(s + 2) != 'l') return false;
      put(0x04);
      put(lo);
      put((c(s + 2) == 'c' ? 0x80 : 0xC0) | hi);
      return true;
    }
    if (letter == 'p' && colons == 1) {   // panning
      const unsigned char w = c(s + 2);
      if (w == 'm') put(0x07);
      else if (w == 'r') put(0x08);
      else if (w == 'l') put(0x09);
      else return false;
      return true;
    }
    if (letter == 'p' && colons == 3 && ch_ == 3) {   // the noise's polynomial counter
      const long long ratio = decimalAt(line_, s + 2);
      if (ratio < 0 || ratio > 13) return false;
      const size_t q = line_.find(':', s + 2);
      if (q == std::string::npos) return false;
      const unsigned char steps = c(q + 1);
      const long long divider = decimalAt(line_, q + 3);
      if (divider < 0 || divider > 7) return false;
      if (steps != '0' && steps != '1') return false;
      put(0x0A);
      put(static_cast<unsigned>(ratio * 16 + divider + (steps == '1' ? 8 : 0)));
      return true;
    }
    if ((letter == 'l' || letter == 'r') && (colons == 1 || colons == 2) && pulse) {   // duty, length
      const int v = static_cast<int>(c(s + 2)) - 48;
      if (v < 0 || v > 3) return false;
      long long length = 0;
      if (colons == 2) {
        length = decimalAt(line_, s + 4);
        if (length < 0 || length > 63) return false;
      }
      put(0x0A);
      put(static_cast<unsigned>(v * 64 + length));
      return true;
    }
    if (letter == 'l' && colons == 1 && ch_ == 3) {
      const long long length = decimalAt(line_, s + 2);
      if (length < 0 || length > 63) return false;
      put(0x0B);
      put(static_cast<unsigned>(length));
      return true;
    }
    if (letter == 'f' && colons == 3 && pulse) {   // frequency sweep
      const int speed = static_cast<int>(c(s + 2)) - 48;
      const int shift = static_cast<int>(c(s + 6)) - 48;
      const unsigned char way = c(s + 4);
      if (speed < 0 || speed > 7 || shift < 0 || shift > 7) return false;
      if (way != 'd' && way != 'u') return false;
      put(0x0B);
      put(static_cast<unsigned>(speed * 16 + shift + (way == 'd' ? 8 : 0)));
      return true;
    }
    if (letter == 'v' && colons == 1 && ch_ == 2) {   // the wave channel's level
      const long long level = decimalAt(line_, s + 2);
      if (level < 0 || level > 3) return false;
      put(0x05);
      put(kWaveLevel[level]);
      return true;
    }
    if (letter == 'w' && colons == 1 && ch_ == 2) {   // a wave
      int id = waveDigit(c(s + 2));
      if (c(s + 3) != ' ') id = id * 16 + waveDigit(c(s + 3));
      if (id < 0 || id > 255) return false;
      const Wave* w = findWave(id);
      if (!w || w->samples.size() != 32) return false;
      put(0x0A);
      put(static_cast<unsigned>(id));
      for (uint8_t b : waveBytes(w->samples)) put(b);
      return true;
    }
    if (letter == 'x' && colons == 2 && ch_ == 2) {   // a long wave, 32 digits at a time
      int id = waveDigit(c(s + 2));
      long long pause;
      if (c(s + 3) == ':') {
        pause = decimalAt(line_, s + 4);
      } else {
        id = id * 16 + waveDigit(c(s + 3));
        pause = decimalAt(line_, s + 5);
      }
      if (id < 0 || id > 255) return false;
      const Wave* w = findWave(id);
      if (!w || w->samples.empty() || w->samples.size() % 32 != 0) return false;
      const unsigned pauseByte = static_cast<unsigned>((static_cast<unsigned long long>(pause) + 1) & 0xFF);
      for (size_t at = 0; at < w->samples.size(); at += 32) {
        put(0x0A);
        put(0xFF);
        for (uint8_t b : waveBytes(w->samples.substr(at, 32))) put(b);
        put(0x01);
        put(0x80);
        put(0x00);
        put(pauseByte);
      }
      return true;
    }
    return false;
  }

  const std::vector<Wave>& waves_;
  const int ch_;
  std::string line_;
};

// ------------------------------------------------------------------- patterns

// The low byte of a 32-bit signed number, and 32-bit sums that wrap instead of overflowing.
int32_t wrapAdd(int32_t a, int32_t b) {
  return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

// A wait written the long way, and the frames the driver takes for it.
long long longWait(Bytes& out, int32_t w) {
  if (w < 0) {
    out.push_back(0);
    out.push_back(static_cast<uint8_t>(w & 0xFF));
    return (w & 0xFF) ? (w & 0xFF) : 256;
  }
  putWait(out, w);
  return ((w % 256) ? (w % 256) : 256) + 256LL * (w / 256);
}

// Compiles one pattern; <frames> gets its length in frames.
Bytes compilePattern(const Pattern& p, long long& frames) {
  const int32_t S = p.speed;
  Bytes out;
  out.push_back(0x0A);
  out.push_back(static_cast<uint8_t>(S & 0xFF));
  frames = 0;
  int32_t W = 0;
  const std::string& cells = p.cells;
  const std::string who = std::to_string(p.number);
  for (size_t row = 0; row + 32 <= cells.size(); row += 32) {
    const uint8_t* r = reinterpret_cast<const uint8_t*>(cells.data()) + row;
    bool stop = false, empty = true;
    for (int ch = 0; ch < 4; ch++) {
      const uint8_t* cell = r + 8 * ch;
      for (int k = 0; k < 3; k++) {
        if (cell[2 + k] == 'C') stop = true;
        if (cell[2 + k] != 0xFF) empty = false;
      }
      if (cell[0] != 0xFF || cell[1] != 0xFF) empty = false;
    }
    if (stop) break;
    if (empty) {
      W = wrapAdd(W, S);
      continue;
    }
    if (W != 0) {
      int k = 0;
      for (int m = 1; m <= 4 && !k; m++)
        if (static_cast<long long>(W) == static_cast<long long>(m) * S) k = m;
      if (k) {
        out.push_back(static_cast<uint8_t>(0x0A + k));
        const unsigned n = (k * (S & 0xFF)) & 0xFF;
        frames += n ? n : 256;
      } else {
        frames += longWait(out, W);
      }
    }
    int32_t R = S;
    for (int ch = 0; ch < 4; ch++) {
      const uint8_t* cell = r + 8 * ch;
      for (int k = 0; k < 3; k++) {   // effects that go before the note
        const uint8_t x = cell[2 + k], v = cell[5 + k];
        const unsigned evenOdd = ((v * 16) & 0xFF) | ((v & 0x30) >> 2) | ch;
        switch (x) {
          case 'L': out.insert(out.end(), {0x11, v, static_cast<uint8_t>(ch)}); break;
          case 'E': out.insert(out.end(), {0x10, static_cast<uint8_t>(evenOdd)}); break;
          case 'F': out.insert(out.end(), {0x0F, static_cast<uint8_t>(evenOdd)}); break;
          case 'H': out.insert(out.end(), {0x04, static_cast<uint8_t>(((v * 16) & 0xFF) | ch)}); break;
          case 'P': out.insert(out.end(), {0x12, static_cast<uint8_t>(((v * 64) & 0xFF) | ch)}); break;
          case 'V':
            if (v <= 0x0F)
              out.insert(out.end(), {0x03, static_cast<uint8_t>((v * 16) | ch)});
            else if (v >= 0x30 && v <= 0x3F)
              out.insert(out.end(), {0x12, static_cast<uint8_t>(kWaveLevel[v & 3] | ch)});
            else
              refuse("pattern " + who + " has a volume the tracker refuses");
            break;
          case 'A': case 'O': case 'Z': case 0xFF: break;
          default: refuse("pattern " + who + " has a command the tracker refuses");
        }
      }
      const uint8_t n = cell[0], i = cell[1];   // the note and the instrument
      if (n == 0xFE) {
        out.insert(out.end(), {0x02, static_cast<uint8_t>(ch)});
      } else if (i != 0xFF) {
        if (ch == 3 || n == 0xFF) out.push_back(0x02);
        else out.insert(out.end(), {0x01, n});
        out.push_back(static_cast<uint8_t>(4 * (i + 1) + ch));
      }
      for (int k = 0; k < 3; k++) {   // effects that go after it
        const uint8_t x = cell[2 + k], v = cell[5 + k];
        if (x == 'O') out.insert(out.end(), {0x05, static_cast<uint8_t>(ch)});
        else if (x == 'Z') out.insert(out.end(), {0x06, v});
        else if (x == 'A') R = v;
      }
    }
    W = R;
  }
  if (W > 1) frames += longWait(out, W - 1);
  out.push_back(0x07);
  frames += 1;
  return out;
}

// ------------------------------------------------------------------- the image

class Banks {
 public:
  // Places a stream; its address and bank.
  void place(const Bytes& s, unsigned& address, unsigned& bank) {
    if (s.size() > kBank) refuse("a pattern doesn't fit into a bank");
    if (banks_.empty() || used_ + s.size() > kBank) {
      banks_.emplace_back(kBank, 0xFF);
      used_ = 0;
    }
    std::memcpy(banks_.back().data() + used_, s.data(), s.size());
    address = static_cast<unsigned>(0x4000 + used_);
    bank = static_cast<unsigned>(banks_.size() + 1);
    used_ += s.size();
  }
  const std::vector<Bytes>& all() const { return banks_; }

 private:
  std::vector<Bytes> banks_;
  size_t used_ = 0;
};

void put16(Bytes& b, size_t at, unsigned v) {
  b[at] = static_cast<uint8_t>(v & 0xFF);
  b[at + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

// Builds the GBS image of a song; <frames> gets its length once through, <repeats> whether
// it starts over, <failed> how many instruments couldn't be compiled.
Bytes buildImage(const Song& song, long long& frames, bool& repeats, int& failed) {
  // the order list: items between commas, entries, and where it starts over
  const std::string& o = song.order;
  std::vector<size_t> items;
  if (!o.empty()) {
    items.push_back(0);
    for (size_t i = 0; i + 1 < o.size(); i++)
      if (o[i] == ',') items.push_back(i + 1);
  }
  long long restart = -1;
  for (size_t i = o.size(); i-- > 1;) {
    if (o[i] == 'x' || o[i] == 'X') {
      restart = decimal(o.data() + i + 1, o.data() + o.size());
      break;
    }
  }
  std::vector<int32_t> entries;
  for (size_t at : items) {
    if (at < o.size() && (o[at] == 'x' || o[at] == 'X')) continue;
    entries.push_back(static_cast<int32_t>(static_cast<uint32_t>(
        static_cast<unsigned long long>(decimal(o.data() + at, o.data() + o.size())))));
  }
  if (entries.empty()) refuse("the song has no order list");

  // the instruments: the last of each number from 0 to 255
  std::map<int, const Instrument*> byNumber;
  int highest = -1;
  for (const Instrument& ins : song.instruments) {
    int k;
    if (!nameNumber(ins.name, k) || k < 0 || k > 255) continue;
    byNumber[k] = &ins;
    if (k > highest) highest = k;
  }
  const size_t table = 8, silence = table + 2 * (highest + 2), emptyScript = silence + 5;
  Bytes bank1(kBank, 0xFF);
  std::vector<unsigned> addresses(highest + 1, 0x4000 + emptyScript);
  Bytes scripts;
  failed = 0;
  for (int k = 0; k <= highest; k++) {
    auto it = byNumber.find(k);
    if (it == byNumber.end()) continue;
    for (int ch = 0; ch < 4; ch++) {
      if (it->second->lines[ch].empty()) continue;
      Bytes s;
      ScriptCompiler sc(song.waves, ch);
      if (sc.compile(it->second->lines[ch], s)) {
        addresses[k] = static_cast<unsigned>(0x4000 + emptyScript + 1 + scripts.size());
        scripts.insert(scripts.end(), s.begin(), s.end());
      } else {
        failed++;
        if (debug_on()) std::fprintf(stderr, "mgb: instrument %d (channel %d) can't be compiled\n", k, ch);
      }
      break;
    }
  }
  const size_t orderTable = emptyScript + 1 + scripts.size();
  if (orderTable + 4 * (entries.size() + 1) > kBank) refuse("the instruments don't fit into a bank");
  put16(bank1, 0, 0x4000 + table);
  put16(bank1, 2, 1);
  put16(bank1, 4, static_cast<unsigned>(0x4000 + orderTable));
  put16(bank1, 6, 1);
  put16(bank1, table, 0x4000 + silence);
  for (int k = 0; k <= highest; k++) put16(bank1, table + 2 * (k + 1), addresses[k]);
  const uint8_t silencing[5] = {0x05, 0x00, 0x01, 0x80, 0x02};
  std::memcpy(&bank1[silence], silencing, 5);
  bank1[emptyScript] = 0x02;
  if (!scripts.empty()) std::memcpy(&bank1[emptyScript + 1], scripts.data(), scripts.size());

  // the patterns, by number: the last one with rows
  std::map<int32_t, const Pattern*> patterns;
  for (const Pattern& p : song.patterns)
    if (p.hasRows && p.number >= 0) patterns[p.number] = &p;

  struct Placed {
    unsigned address, bank;
    long long frames;
  };
  std::map<int32_t, Placed> placed;
  Banks banks;
  frames = 0;
  for (size_t e = 0; e < entries.size(); e++) {
    const int32_t number = entries[e];
    auto done = placed.find(number);
    if (done == placed.end()) {
      auto it = patterns.find(number);
      if (it == patterns.end())
        refuse("the order list names pattern " + std::to_string(number) + ", which isn't there");
      Placed pl;
      const Bytes s = compilePattern(*it->second, pl.frames);
      banks.place(s, pl.address, pl.bank);
      done = placed.emplace(number, pl).first;
    }
    put16(bank1, orderTable + 4 * e, done->second.address);
    put16(bank1, orderTable + 4 * e + 2, done->second.bank);
    frames += done->second.frames;
  }

  // the ending: a jump back, or the end of the song
  Bytes ending;
  repeats = restart >= 0;
  if (repeats) {
    const unsigned long long I = items.size(), E = entries.size() + 1;
    const long long b = static_cast<long long>((static_cast<unsigned long long>(restart) - I) * 4);
    const long long t = static_cast<long long>(4 * E + static_cast<unsigned long long>(b));
    if (t < 0 || t >= static_cast<long long>(4 * E)) refuse("the order list repeats from a place that isn't there");
    ending = {0x08, static_cast<uint8_t>(b & 0xFF), static_cast<uint8_t>((b >> 8) & 0xFF), 0x07};
    frames += 1;
  } else {
    ending = {0x09};
  }
  unsigned address, bank;
  banks.place(ending, address, bank);
  put16(bank1, orderTable + 4 * entries.size(), address);
  put16(bank1, orderTable + 4 * entries.size() + 2, bank);
  if (static_cast<int>(banks.all().size()) + 2 > kMostBanks) refuse("the song is too big");

  // header, driver, bank 1, the pattern banks
  Bytes image(kHeader + kDriverSize, 0);
  std::memcpy(image.data(), "GBS", 3);
  image[3] = 1;
  image[4] = 1;
  image[5] = 1;
  put16(image, 6, 0x0400);
  put16(image, 8, 0x0400);
  put16(image, 10, 0x0403);
  put16(image, 12, 0xDFFF);
  static_assert(sizeof(mgb_driver) <= kDriverSize, "the driver is bigger than its room");
  std::memcpy(image.data() + kHeader, mgb_driver, sizeof(mgb_driver));
  image.insert(image.end(), bank1.begin(), bank1.end());
  for (const Bytes& b : banks.all()) image.insert(image.end(), b.begin(), b.end());
  return image;
}

// ------------------------------------------------------------------- the engine

class GbtEngine : public Engine {
 public:
  ~GbtEngine() override {
    if (emu_) gme_delete(emu_);
  }

  bool open(const std::vector<uint8_t>& file, const std::string&, int, int rate, SongInfo& info,
            std::string& error) override {
    if (file.size() < 36 || std::memcmp(&file[12], kSongMagic, 20) != 0 || le32(&file[0]) != kSongSig) {
      error = "not a Gameboy Tracker module";
      return false;
    }
    Song song;
    long long frames = 0;
    bool repeats = false;
    int failed = 0;
    try {
      readSong(file, song);
      image_ = buildImage(song, frames, repeats, failed);
    } catch (const Failure& f) {
      error = f.why;
      return false;
    }

    if (const char* dump = std::getenv("BBMP_RETRO_MGB_DUMP")) {
      if (FILE* f = std::fopen(dump, "wb")) {
        std::fwrite(image_.data(), 1, image_.size(), f);
        std::fclose(f);
      }
    }
    if (gme_err_t err = gme_open_data(image_.data(), static_cast<long>(image_.size()), &emu_, rate)) {
      error = std::string("Game Music Emu: ") + err;
      return false;
    }
    gme_ignore_silence(emu_, 1);
    if (gme_err_t err = gme_start_track(emu_, 0)) {
      error = std::string("Game Music Emu: ") + err;
      return false;
    }

    const double frameRate = 4194304.0 / 70224.0;
    const long long once = static_cast<long long>(static_cast<double>(frames) / frameRate * rate);
    if (repeats) {
      fadeLength_ = 4LL * rate;
      fadeStart_ = once;
    } else {
      fadeLength_ = rate;
      fadeStart_ = once + rate;
    }
    end_ = fadeStart_ + fadeLength_;

    info.tracks = 1;
    info.title = shown(song.title);
    info.author.clear();
    info.type = kSongMagic;
    info.duration_ms = end_ * 1000 / rate;
    if (failed == 1)
      info.comment = "1 instrument has a script the tracker can't compile and stay silent";
    else if (failed > 1)
      info.comment = std::to_string(failed) + " instruments have a script the tracker can't compile and stay silent";
    for (const Instrument& ins : song.instruments) {
      const size_t colon = ins.name.find(':');
      info.names.push_back(shown(colon != std::string::npos && colon <= 2 ? ins.name.substr(colon + 1) : ins.name));
    }
    if (debug_on())
      std::fprintf(stderr, "mgb: %lld frames, %s, image of %zu bytes\n", frames, repeats ? "repeats" : "ends",
                   image_.size());
    return true;
  }

  size_t render(int16_t* out, size_t frames) override {
    if (!emu_ || done_ >= end_) return 0;
    size_t n = frames;
    if (static_cast<long long>(n) > end_ - done_) n = static_cast<size_t>(end_ - done_);
    if (gme_play(emu_, static_cast<int>(n * 2), out)) return 0;
    for (size_t i = 0; i < n; i++) {
      const long long t = done_ + static_cast<long long>(i);
      if (t < fadeStart_) continue;
      const double g = 1.0 - static_cast<double>(t - fadeStart_) / static_cast<double>(fadeLength_);
      out[2 * i] = static_cast<int16_t>(out[2 * i] * g);
      out[2 * i + 1] = static_cast<int16_t>(out[2 * i + 1] * g);
    }
    done_ += static_cast<long long>(n);
    return n;
  }

 private:
  Music_Emu* emu_ = nullptr;
  Bytes image_;
  long long fadeStart_ = 0, fadeLength_ = 0, end_ = 0, done_ = 0;
};

}  // namespace

REGISTER_ENGINE("mgb", GbtEngine)
