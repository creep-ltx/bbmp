// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-misc's engine "soundclub": the songs of Sound Club, BlueMoon Software's tracker
// (Sound Club for DOS: .sn, "Sound Club"; Sound Club for Windows: .sn2, "Sound Club 2").
//
// Written for this player in a clean room, from a description of the format only.
//
// A Sound Club 1 song is one bit stream: a header, the sequence, patterns whose voices are
// coded as short bit codes (skips, notes moving up or down from the last one, volume and
// balance changes, slides), then the instruments. A Sound Club 2 song is made of chunks
// (title, sequence, patterns with tempo maps and voices of byte-coded events, instruments).
// Instruments are samples, stored plain or Huffman-coded with up to three running sums, or
// FM instruments (two operators), which are rendered once into a sample when the song is
// loaded. Songs may also name instruments of Sound Club's instrument bank.
//
// Two files of Sound Club for Windows 2.03 are needed for some songs: the instrument bank
// (sclub.bnk) and the waveforms of its FM synthesizer (scwaves.bin). They are BlueMoon's
// and are not part of this player; the player downloads Sound Club's package from its
// publisher and takes them out (`bbmp download soundclub`). They are looked for in
// $BBMP_SOUNDCLUB_DATA, else in share/soundclub beside the helper's bin folder, and read
// only when a song needs them.
//
// The song is mixed in blocks of 8192 frames: every note takes a place (at most 4096 at
// once), samples are stepped at their pitch with a short interpolation for low notes, and
// volume and balance come from the voice.
//
// Test mode: with BBMP_SOUNDCLUB_DUMP=<prefix>, the instruments after loading are listed in
// <prefix>.inst.txt (kind, rate, length, loop, a hash of the sound, name) and the song's
// sequence, patterns and events in <prefix>.events.txt.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "common.h"

namespace misc {
namespace {

// ---------------------------------------------------------------------------------------
// Tables

// The pitch of each note (0 = C1, 45 = A4) in Hz, 16.16
const uint32_t kPitch[84] = {
  2143237, 2270680, 2405702, 2548752, 2700309, 2860878, 3030994, 3211227,
  3402176, 3604480, 3818814, 4045892, 4286473, 4541360, 4811404, 5097505,
  5400618, 5721755, 6061989, 6422453, 6804352, 7208960, 7637627, 8091784,
  8572947, 9082720, 9622807, 10195009, 10801236, 11443511, 12123977, 12844906,
  13608704, 14417920, 15275254, 16183568, 17145893, 18165441, 19245614, 20390018,
  21602472, 22887021, 24247954, 25689813, 27217409, 28835840, 30550508, 32367136,
  34291786, 36330882, 38491228, 40780036, 43204943, 45774043, 48495909, 51379626,
  54434817, 57671680, 61101017, 64734272, 68583572, 72661764, 76982457, 81560072,
  86409886, 91548086, 96991818, 102759252, 108869635, 115343360, 122202033, 129468544,
  137167144, 145323527, 153964914, 163120144, 172819773, 183096171, 193983636, 205518503,
  217739269, 230686720, 244404066, 258937088,
};

// The gain of each volume 0..30 (256 = full)
const int kVolumeGain[31] = {
  0, 16, 18, 19, 21, 23, 26, 28,
  31, 34, 38, 42, 46, 50, 55, 61,
  67, 74, 81, 89, 98, 108, 119, 131,
  144, 159, 175, 192, 211, 233, 256,
};

// An FM operator's level setting as a divisor (65535: the operator is silent)
const uint32_t kFmLevel[64] = {
  256, 279, 304, 331, 362, 394, 430, 469,
  512, 558, 608, 663, 724, 789, 861, 939,
  1024, 1116, 1217, 1327, 1448, 1579, 1722, 1878,
  2048, 2233, 2435, 2655, 2896, 3158, 3444, 3756,
  4096, 4466, 4870, 5311, 5792, 6316, 6888, 7512,
  8192, 8933, 9741, 10623, 11585, 12633, 13777, 15024,
  16384, 17866, 19483, 21247, 23170, 25267, 27554, 30048,
  32768, 35733, 38967, 42494, 46340, 50535, 55108, 65535,
};

// An FM operator's sustain level
const uint32_t kFmSustain[16] = {
  65535, 46340, 32767, 23170, 16383, 11585, 8191, 5792,
  4095, 2896, 2047, 1448, 1023, 724, 511, 362,
};

// Code page 775 (the DOS version's texts), bytes $80..$FF
const uint16_t kCp775[128] = {
  0x0106, 0x00FC, 0x00E9, 0x0101, 0x00E4, 0x0123, 0x00E5, 0x0107,
  0x0142, 0x0113, 0x0156, 0x0157, 0x012B, 0x0179, 0x00C4, 0x00C5,
  0x00C9, 0x00E6, 0x00C6, 0x014D, 0x00F6, 0x0122, 0x00A2, 0x015A,
  0x015B, 0x00D6, 0x00DC, 0x00F8, 0x00A3, 0x00D8, 0x00D7, 0x00A4,
  0x0100, 0x012A, 0x00F3, 0x017B, 0x017C, 0x017A, 0x201D, 0x00A6,
  0x00A9, 0x00AE, 0x00AC, 0x00BD, 0x00BC, 0x0141, 0x00AB, 0x00BB,
  0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x0104, 0x010C, 0x0118,
  0x0116, 0x2563, 0x2551, 0x2557, 0x255D, 0x012E, 0x0160, 0x2510,
  0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x0172, 0x016A,
  0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x017D,
  0x0105, 0x010D, 0x0119, 0x0117, 0x012F, 0x0161, 0x0173, 0x016B,
  0x017E, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
  0x00D3, 0x00DF, 0x014C, 0x0143, 0x00F5, 0x00D5, 0x00B5, 0x0144,
  0x0136, 0x0137, 0x013B, 0x013C, 0x0146, 0x0112, 0x0145, 0x2019,
  0x00AD, 0x00B1, 0x201C, 0x00BE, 0x00B6, 0x00A7, 0x00F7, 0x201E,
  0x00B0, 0x2219, 0x00B7, 0x00B9, 0x00B3, 0x00B2, 0x25A0, 0x00A0,
};

// Code page 1252 (the Windows version's texts), bytes $80..$FF
const uint16_t kCp1252[128] = {
  0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
  0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
  0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
  0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
  0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
  0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
  0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
  0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
  0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
  0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
  0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7,
  0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
  0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
  0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
  0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
  0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,
};

const int kNotes = 84;
const uint32_t kFmRate = 33488;
const int kFmLongest = 65536;
const int kBlock = 8192;               // frames mixed at a time
const size_t kMostPlaces = 4096;
const uint64_t kLongestSong = 1u << 24;  // steps
const size_t kDataFileMax = (64u << 20) - 1;

// A refusal: the message is the first failure met.
struct Refusal {
  std::string why;
};
[[noreturn]] void refuse(const std::string& why) { throw Refusal{why}; }

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

// ---------------------------------------------------------------------------------------
// Bits, most significant first; reads past the end give 0 and mark the stream overrun.

class Bits {
 public:
  Bits(const uint8_t* data, size_t size, size_t start_byte = 0)
      : d_(data), bits_(static_cast<uint64_t>(size) * 8), pos_(std::min<uint64_t>(start_byte * 8ull, bits_)) {}

  uint32_t read(int n) {
    if (n <= 0) return 0;
    if (pos_ + n > bits_) {
      overrun_ = true;
      pos_ = bits_;
      return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < n; ++i, ++pos_) v = v << 1 | ((d_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1);
    return v;
  }
  uint32_t byte() { return read(8); }
  // The next 16 bits without moving on (0 when fewer are left).
  uint32_t peek16() const {
    if (pos_ + 16 > bits_) return 0;
    uint32_t v = 0;
    for (uint64_t p = pos_; p < pos_ + 16; ++p) v = v << 1 | ((d_[p >> 3] >> (7 - (p & 7))) & 1);
    return v;
  }
  void align() { pos_ = std::min<uint64_t>((pos_ + 7) & ~7ull, bits_); }
  // A text ended by a zero byte, at most `most` characters; false when bad.
  bool string(int most, std::string& out) {
    out.clear();
    bool ended = false;
    for (int i = 0; i <= most; ++i) {
      const uint32_t c = byte();
      if (c == 0) {
        ended = true;
        break;
      }
      out += static_cast<char>(c);
    }
    return ended && !overrun_;
  }
  bool overrun() const { return overrun_; }
  uint64_t left() const { return bits_ - pos_; }
  size_t byte_pos() const { return static_cast<size_t>(pos_ >> 3); }
  size_t size() const { return static_cast<size_t>(bits_ >> 3); }
  const uint8_t* data() const { return d_; }

 private:
  const uint8_t* d_;
  uint64_t bits_, pos_;
  bool overrun_ = false;
};

// ---------------------------------------------------------------------------------------
// Texts

void put_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | cp >> 6);
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xE0 | cp >> 12);
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

std::string to_utf8(const std::string& s, const uint16_t* page) {
  std::string out;
  for (unsigned char c : s) {
    if (c == 0) break;
    if (c == '\r') continue;
    if (c >= 0x80) put_utf8(out, page[c - 0x80]);
    else if (c < 0x20 && c != '\n' && c != '\t') out += ' ';
    else out += static_cast<char>(c);
  }
  return out;
}

std::string trim(const std::string& s) {
  auto blank = [](char c) { return c == ' ' || c == '\n' || c == '\t'; };
  size_t e = s.size();
  while (e > 0 && blank(s[e - 1])) e--;
  size_t b = 0;
  while (b < e && blank(s[b])) b++;
  return s.substr(b, e - b);
}

std::string instrument_name(const std::string& raw, const uint16_t* page) {
  std::string s = to_utf8(raw, page);
  for (char& c : s)
    if (c == '\n' || c == '\t') c = ' ';
  return trim(s);
}

// ---------------------------------------------------------------------------------------
// The song

enum EventType : uint8_t { kNoteOn = 1, kRelease = 2, kNoteOff = 3, kVolume = 4, kBalance = 5, kSlide = 6 };

struct Event {
  uint32_t step;
  uint8_t type, a, b, c;
};

struct Voice {
  uint32_t instrument = 0;     // as the file has it; a number that isn't an instrument is "none"
  std::vector<Event> events;
};

struct TempoEntry {
  uint32_t step;
  int tempo;
};

struct Pattern {
  uint32_t length = 0;
  std::vector<Voice> voices;
  std::vector<TempoEntry> tempo;   // in the file's order
  bool has_tempo = false;
};

struct Instrument {
  std::string name;
  bool fm = false, bank = false, bits8 = false;
  uint32_t rate = 0, length = 0;
  int64_t loop = -1;
  std::vector<int16_t> sound;      // length + 1 values after loading
  bool valid() const { return length > 0; }
};

struct Song {
  int version = 1;
  std::string title, comment;
  int global_tempo = 0;
  uint32_t restart = 0;
  std::vector<uint32_t> sequence;
  std::vector<Pattern> patterns;
  std::vector<Instrument> instruments;
};

int event_class(uint8_t type) { return type == kNoteOff ? 0 : type == kSlide ? 2 : 1; }

void sort_events(std::vector<Event>& e) {
  std::stable_sort(e.begin(), e.end(), [](const Event& x, const Event& y) {
    if (x.step != y.step) return x.step < y.step;
    return event_class(x.type) < event_class(y.type);
  });
}

// The tempo in force at `step`, continuing a scan of the map at `k` with `tempo`.
void tempo_at(const Pattern& p, uint32_t step, size_t& k, int& tempo) {
  while (k < p.tempo.size() && p.tempo[k].step <= step) tempo = p.tempo[k++].tempo;
}

int tempo_from_start(const Pattern& p, uint32_t step) {
  size_t k = 0;
  int tempo = 20;
  tempo_at(p, step, k, tempo);
  return tempo;
}

// ---------------------------------------------------------------------------------------
// The data files

class DataFiles {
 public:
  DataFiles() {
    if (const char* d = std::getenv("BBMP_SOUNDCLUB_DATA")) {
      dir_ = d;
    } else {
      char buf[4096];
      const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
      if (n > 0) {
        std::string self(buf, n);
        size_t slash = self.rfind('/');
        std::string bin = slash == std::string::npos ? std::string() : self.substr(0, slash);
        slash = bin.rfind('/');
        dir_ = (slash == std::string::npos ? std::string(".") : bin.substr(0, slash)) + "/share/soundclub";
      } else {
        dir_ = "share/soundclub";
      }
    }
  }

  const std::vector<uint8_t>& bank() {
    if (!bank_tried_) {
      bank_tried_ = true;
      bank_ok_ = load("sclub.bnk", bank_) && bank_.size() >= 0x43 && std::memcmp(bank_.data(), "BNK", 3) == 0;
    }
    if (!bank_ok_) refuse("the song needs Sound Club's instrument bank (sclub.bnk)");
    return bank_;
  }

  // Waveform w (10..31) of the FM synthesizer.
  const int16_t* wave(int w) {
    if (!waves_tried_) {
      waves_tried_ = true;
      std::vector<uint8_t> raw;
      waves_ok_ = load("scwaves.bin", raw) && raw.size() >= 22 * 4096;
      if (waves_ok_) {
        waves_.resize(22 * 2048);
        for (size_t i = 0; i < waves_.size(); ++i) waves_[i] = static_cast<int16_t>(le16(&raw[2 * i]));
      }
    }
    if (!waves_ok_) refuse("the song needs the waveforms of Sound Club's FM synthesizer (scwaves.bin)");
    return &waves_[(w - 10) * 2048];
  }

 private:
  bool load(const char* name, std::vector<uint8_t>& out) {
    FILE* f = std::fopen((dir_ + "/" + name).c_str(), "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[65536];
    size_t n;
    bool ok = true;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
      out.insert(out.end(), buf, buf + n);
      if (out.size() > kDataFileMax) {
        ok = false;
        break;
      }
    }
    std::fclose(f);
    return ok && !out.empty();
  }

  std::string dir_;
  std::vector<uint8_t> bank_;
  std::vector<int16_t> waves_;
  bool bank_tried_ = false, bank_ok_ = false, waves_tried_ = false, waves_ok_ = false;
};

// ---------------------------------------------------------------------------------------
// The FM synthesizer

struct FmTables {
  int16_t sine[2048];
  uint8_t trem[128];
  FmTables() {
    int32_t v = 32760, vel = 0;
    for (int i = 0; i < 2048; ++i) {
      sine[(i + 512) & 2047] = static_cast<int16_t>(v);
      vel += v / -350;
      v += vel / 300;
    }
    for (int k = 0; k < 128; ++k) trem[k] = static_cast<uint8_t>(32 - (sine[16 * k] + 32768) / 6553);
  }
};

const FmTables& fm_tables() {
  static const FmTables t;
  return t;
}

struct Operator {
  uint32_t vib_phase = 0, vib_step = 0, trem_phase = 0, trem_step = 0;
  uint32_t env = 0, div = 0, trem_value = 32, amp = 0, attacking = 1;
  uint32_t attack = 0, decay = 0, release = 0, sustain = 0;
  uint32_t phase = 0, mult = 0, step = 0, threshold = 0;
  std::vector<int16_t> wave;     // empty: noise

  void setup(const uint8_t* q, uint32_t quiet, bool modulator, DataFiles& files) {
    const FmTables& t = fm_tables();
    attack = 24u << (q[3] >> 4);
    decay = (q[3] & 15) ? (0x2678u >> (q[3] & 15)) + 1 : 0;
    release = (q[4] & 15) ? (0x2678u >> (q[4] & 15)) + 1 : 0;
    const uint32_t level = kFmLevel[q[0] & 63];
    if (level == 65535) {
      attack = 49152;
      release = 1;
      decay = 1;
    }
    div = level >> 1;
    if (modulator) div *= 4;
    if (div >= 65535) div = 0xFFFFFFFFu;
    sustain = kFmSustain[q[4] >> 4];
    mult = (q[2] & 31) * 8 + 8;
    const int vib = q[0] >> 6;
    vib_step = vib ? 24u << (vib - 1) : 0;
    trem_step = q[1] >> 6;
    if (trem_step == 3) trem_step = 4;
    const int w = q[1] & 63;
    wave.assign(2048, 0);
    switch (w) {
      case 0: std::copy(t.sine, t.sine + 2048, wave.begin()); break;
      case 1: std::copy(t.sine, t.sine + 1024, wave.begin()); break;
      case 2:
        std::copy(t.sine, t.sine + 1024, wave.begin());
        std::copy(t.sine, t.sine + 1024, wave.begin() + 1024);
        break;
      case 3:
        std::copy(t.sine, t.sine + 512, wave.begin());
        std::copy(t.sine, t.sine + 512, wave.begin() + 1024);
        break;
      case 4:
        for (int i = 0; i < 2048; ++i) wave[i] = i < 1024 ? 32767 : -32768;
        break;
      case 5:
        for (int i = 0; i < 2048; ++i) wave[i] = i < 512 ? 32767 : -32768;
        break;
      case 6:
        for (int i = 0; i < 2048; ++i) wave[i] = static_cast<int16_t>(static_cast<uint16_t>(i * 32));
        break;
      case 7:
      case 8:
        for (int i = 0; i < 512; ++i) {
          int a = 64 * i, b = 32767 - a;
          if (w == 8) {
            a = std::min(128 * i, 32767);
            b = std::min(2 * (32767 - 64 * i), 32767);
          }
          wave[i] = static_cast<int16_t>(a);
          wave[i + 512] = static_cast<int16_t>(b);
          wave[i + 1024] = static_cast<int16_t>(-a);
          wave[i + 1536] = static_cast<int16_t>(-b);
        }
        break;
      case 9: wave.clear(); break;
      default:
        if (w >= 32) refuse("an FM instrument can't be rendered");
        {
          const int16_t* src = files.wave(w);
          std::copy(src, src + 2048, wave.begin());
        }
    }
    attacking = 1;
    trem_value = 32;
    step = mult << 16;
    env = phase = vib_phase = trem_phase = 0;
    amp = 0;
    threshold = div >= 65535 ? 65536 : ((quiet * div - 1) | 0xF80) >> 7;
  }

  void amplitude() { amp = (((env & ~31u) * trem_value) << 1) / div; }

  void lfo() {
    const FmTables& t = fm_tables();
    if (vib_step) {
      vib_phase = (vib_phase + vib_step) & 2047;
      step = static_cast<uint32_t>(t.sine[vib_phase] / 62 + 65536) * mult;
    }
    if (trem_step) {
      trem_phase = (trem_phase + trem_step) & 127;
      trem_value = t.trem[trem_phase];
    }
  }

  void envelope() {
    if (attacking) {
      env += attack;
      if (env > 65535) {
        env = 65535;
        attacking = 0;
      }
    } else {
      uint32_t v = env;
      if (decay) v = v > decay ? v - v / decay : v - 2;
      env = v;
      if (static_cast<int32_t>(v) < 0) {
        v = 0;
        decay = 0;
        env = 0;
      }
      if (v < sustain) {
        env = sustain;
        decay = release;
        sustain = 0;
      }
    }
    amplitude();
  }

  int32_t read(uint32_t at, uint32_t& noise) const {
    int32_t s;
    if (!wave.empty()) {
      s = wave[at & 2047];
    } else {
      noise = (noise * 805 + 1) & 65535;
      s = static_cast<int16_t>(noise);
    }
    return (static_cast<int32_t>(amp) * s) >> 15;
  }
};

// Renders the FM instrument of patch p into `ins`.
void render_fm(const uint8_t* p, Instrument& ins, DataFiles& files) {
  const int fb = p[3] >> 5;
  int fbshift = fb ? 10 - fb : 16;
  const bool fm = p[0] != 0;
  if (fm && fbshift < 16) fbshift -= 2;
  Operator op1, op2;
  op1.setup(p + 1, fm ? 6554 : 4915, fm, files);
  op2.setup(p + 6, 4915, false, files);
  int32_t out1 = 0;
  uint32_t noise = 0;

  bool looped = false;
  uint32_t loop_start = 0, rem_noise = 0, rem_ph[2] = {0, 0}, rem_vp[2] = {0, 0}, rem_tp[2] = {0, 0};
  int32_t rem_out1 = 0;
  std::vector<int16_t> out;
  out.reserve(kFmLongest + 1);
  ins.length = kFmLongest;
  ins.loop = -1;
  auto near = [&](const Operator& o, int i, uint32_t since) {
    const int32_t d = static_cast<int32_t>((rem_ph[i] - o.phase) & 0x7FFFFFF);
    if (d > static_cast<int32_t>(since << 4)) return false;
    const int32_t dv = static_cast<int32_t>(rem_vp[i] - o.vib_phase), lv = static_cast<int32_t>(since >> 10);
    if (dv < -lv || dv > lv) return false;
    const int32_t dt = static_cast<int32_t>(rem_tp[i] - o.trem_phase), lt = static_cast<int32_t>(since >> 16);
    return dt >= -lt && dt <= lt;
  };
  for (uint32_t n = 0; n < static_cast<uint32_t>(kFmLongest); ++n) {
    if ((n & 15) == 0) {
      if (!looped && !op1.attacking && !op2.attacking && op1.decay == 0 && op2.decay == 0) {
        looped = true;
        loop_start = n;
        rem_noise = noise;
        rem_out1 = out1;
        const Operator* ops[2] = {&op1, &op2};
        for (int i = 0; i < 2; ++i) {
          rem_ph[i] = ops[i]->phase;
          rem_vp[i] = ops[i]->vib_phase;
          rem_tp[i] = ops[i]->trem_phase;
        }
      }
      if ((n & 127) == 0) {
        op1.lfo();
        op2.lfo();
      }
      op1.envelope();
      op2.envelope();
    }
    if (!looped) {
      if ((n & 255) == 0 && op2.env <= op2.threshold && !op2.attacking &&
          (fm || (op1.env <= op1.threshold && !op1.attacking))) {
        ins.length = n;
        ins.loop = -1;
        break;
      }
    } else if (n != loop_start) {
      const uint32_t since = n - loop_start;
      if (((static_cast<uint32_t>(rem_out1) - static_cast<uint32_t>(out1)) & 0xFE00) == 0 && near(op1, 0, since) &&
          near(op2, 1, since) && noise == rem_noise) {
        ins.length = n;
        ins.loop = loop_start;
        break;
      }
    }
    uint32_t at = op1.phase >> 16;
    if (fbshift < 16)
      at += (static_cast<uint32_t>(out1) >> fbshift) + ((static_cast<uint32_t>(out1) >> (fbshift - 1)) & 1);
    out1 = op1.read(at, noise);
    at = op2.phase >> 16;
    if (fm) at += static_cast<uint32_t>(out1);
    int32_t v = op2.read(at, noise);
    if (!fm) v = std::clamp(v + out1, -32768, 32767);
    out.push_back(static_cast<int16_t>(v));
    op1.phase += op1.step;
    op2.phase += op2.step;
  }
  out.resize(ins.length);
  ins.sound = std::move(out);
}

void fm_instrument(const uint8_t* p, Instrument& ins, DataFiles& files) {
  const bool bad = p[0] >= 2 || (p[8] & 0xE0) || (p[3] & 0x1F) == 0x1F || (p[2] & 0x3F) >= 32 ||
                   (p[4] & 0xF0) > 0xB0 || (p[4] & 15) > 13 || (p[5] & 15) > 13 || (p[8] & 0x1F) == 0x1F ||
                   (p[7] & 0x3F) >= 32 || (p[9] & 0xF0) > 0xB0 || (p[9] & 15) > 13 || (p[10] & 15) > 13;
  if (bad) refuse("an FM instrument is not valid");
  if ((p[2] & 63) >= 10) files.wave(p[2] & 63);
  if ((p[7] & 63) >= 10) files.wave(p[7] & 63);
  ins.fm = true;
  ins.rate = kFmRate;
  ins.bits8 = false;
  render_fm(p, ins, files);
}

// ---------------------------------------------------------------------------------------
// Samples

// The order of two items of the Huffman heap: by frequency, then by number.
struct HuffItem {
  uint32_t freq;
  int id;
  bool operator<(const HuffItem& o) const { return freq != o.freq ? freq < o.freq : id < o.id; }
};

void sift_down(std::vector<HuffItem>& h, size_t i, size_t n) {
  const HuffItem x = h[i];
  while (2 * i <= n) {
    size_t c = 2 * i;
    if (c != n && !(h[c] < h[c + 1])) c++;
    if (x < h[c]) break;
    h[i] = h[c];
    h[c] = x;
    i = c;
  }
}

// Unpacks a sample (packings 0..4) into its byte values.
void unpack(Bits& bits, int packing, uint32_t length, std::vector<uint8_t>& values) {
  values.clear();
  if (length == 0) {
    bits.align();
    return;
  }
  values.reserve(length);
  if (packing == 0) {
    for (uint32_t i = 0; i < length; ++i) values.push_back(static_cast<uint8_t>(bits.byte()));
  } else {
    std::vector<uint8_t> symbols;
    if (bits.peek16() >= 0xFF00) {
      symbols.assign(length, static_cast<uint8_t>(bits.read(16)));
    } else {
      uint32_t freq[256] = {0};
      for (int half = 0; half < 2; ++half) {
        int left = 128, w = 0, sym = 0;
        while (left > 0) {
          const uint32_t k = bits.byte();
          for (uint32_t j = 0; j < k && left > 0; ++j, --left, ++sym)
            freq[half ? 255 - sym : sym] = bits.read(w);
          ++w;
          if (w > 24 || bits.overrun()) refuse("a sample has a bad table of frequencies");
        }
      }
      std::vector<HuffItem> heap(1);
      std::vector<uint32_t> node_freq(512, 0);
      for (int s = 0; s < 256; ++s)
        if (freq[s]) heap.push_back({freq[s], s});
      size_t n = heap.size() - 1;
      if (n < 2) refuse("a sample has a bad Huffman code");
      for (size_t i = n / 2; i >= 1; --i) sift_down(heap, i, n);
      std::vector<int> child0(512, 0), child1(512, 0);
      int next = 256, root = 0;
      while (n > 1) {
        const HuffItem first = heap[1];
        heap[1] = heap[n];
        --n;
        sift_down(heap, 1, n);
        const HuffItem second = heap[1];
        const uint32_t sum = first.freq + second.freq;
        if (sum > 65535) refuse("a sample has a bad Huffman code");
        child0[next] = first.id;
        child1[next] = second.id;
        heap[1] = {sum, next};
        sift_down(heap, 1, n);
        root = next++;
      }
      symbols.reserve(length);
      for (uint32_t i = 0; i < length; ++i) {
        int node = root;
        while (node >= 256) node = bits.read(1) ? child1[node] : child0[node];
        if (bits.overrun()) refuse("a sample is cut off");
        symbols.push_back(static_cast<uint8_t>(node));
      }
    }
    uint8_t a1 = 128, a2 = 128, a3 = 128;
    for (uint8_t s : symbols) {
      uint8_t v = s;
      if (packing >= 2) {
        a1 = static_cast<uint8_t>(a1 + v + 128);
        v = a1;
      }
      if (packing >= 3) {
        a2 = static_cast<uint8_t>(a2 + v + 128);
        v = a2;
      }
      if (packing == 4) {
        a3 = static_cast<uint8_t>(a3 + v + 128);
        v = a3;
      }
      values.push_back(v);
    }
  }
  bits.align();
  if (bits.overrun()) refuse("a sample is cut off");
}

// A sample's sound, from the bit stream's position (stored in a song, or in the bank).
void sample_sound(Bits& bits, Instrument& ins) {
  const int packing = static_cast<int>(bits.byte());
  if (bits.overrun()) refuse("an instrument is cut off");
  if (packing == 5) {
    bits.align();
    const uint8_t* d = bits.data();
    const size_t end = bits.size();
    size_t p = bits.byte_pos();
    for (;;) {
      if (end - p < 7) refuse("an instrument has no sound");
      const uint32_t n = le32(d + p + 3);
      if (n > end - p - 7) refuse("an instrument is cut off");
      if (std::memcmp(d + p, "dsa", 3) != 0) {
        p += 7 + n;
        continue;
      }
      const uint8_t* b = d + p + 7;
      if (n < 11) refuse("an instrument has a bad header");
      const int bps = b[0];
      const uint32_t rate = le16(b + 1), length = le32(b + 3);
      const int32_t loop = static_cast<int32_t>(le32(b + 7));
      if ((bps != 1 && bps != 2) || rate == 0 || length > (bps == 1 ? 0x7FFFFFFFu : 0x3FFFFFFFu) ||
          static_cast<uint64_t>(length) * bps > n - 11 || (loop != -1 && !(loop < static_cast<int64_t>(length))))
        refuse("an instrument has a bad header");
      ins.rate = rate;
      ins.length = length;
      ins.loop = loop;
      ins.bits8 = bps == 1;
      ins.sound.resize(length);
      for (uint32_t i = 0; i < length; ++i)
        ins.sound[i] = bps == 2 ? static_cast<int16_t>(le16(b + 11 + 2 * i))
                                : static_cast<int16_t>((b[11 + i] - 128) * 256);
      return;
    }
  }
  uint8_t h[16];
  for (int i = 0; i < 16; ++i) h[i] = static_cast<uint8_t>(bits.byte());
  const uint32_t length = le32(h);
  const int32_t loop = static_cast<int32_t>(le32(h + 4));
  const uint16_t checksum = le16(h + 12), rate = le16(h + 14);
  if (bits.overrun() || rate == 0 || packing > 4 || le32(h + 8) != 0xFFFFFFFFu || length > (64u << 20) ||
      (loop != -1 && !(loop < static_cast<int64_t>(length))))
    refuse("an instrument has a bad header");
  const uint64_t need = packing == 0 ? static_cast<uint64_t>(length) * 8 : length;
  if (bits.left() < need) refuse("a sample is cut off");
  std::vector<uint8_t> values;
  unpack(bits, packing, length, values);
  if (length) {
    uint16_t sum = 0;
    for (uint8_t v : values) {
      sum = static_cast<uint16_t>((sum & 0xFF00) | ((sum + v) & 0xFF));
      sum = static_cast<uint16_t>(sum << 1 | sum >> 15);
    }
    if (sum != checksum) refuse("a sample has a wrong checksum");
  }
  ins.rate = rate;
  ins.length = length;
  ins.loop = loop;
  ins.bits8 = true;
  ins.sound.resize(length);
  for (uint32_t i = 0; i < length; ++i) ins.sound[i] = static_cast<int16_t>((values[i] - 128) * 256);
}

// An instrument stored in the song, from the bit stream's position.
void stored_instrument(Bits& bits, const uint16_t* page, Instrument& ins, DataFiles& files) {
  char tag[3];
  for (char& c : tag) c = static_cast<char>(bits.byte());
  const bool syn = std::memcmp(tag, "SYN", 3) == 0, smp = std::memcmp(tag, "SMP", 3) == 0;
  if (bits.overrun() || (!syn && !smp)) refuse("an instrument is of an unknown kind");
  std::string name;
  if (!bits.string(16, name)) refuse("an instrument has a bad name");
  const uint32_t group = bits.byte();
  if (bits.overrun() || (group >= 30 && group != 255)) refuse("an instrument has a bad group");
  ins.name = instrument_name(name, page);
  if (syn) {
    uint8_t p[11];
    for (uint8_t& b : p) b = static_cast<uint8_t>(bits.byte());
    if (bits.overrun()) refuse("an instrument is cut off");
    fm_instrument(p, ins, files);
  } else {
    sample_sound(bits, ins);
  }
}

// An instrument of the bank, by its name.
void bank_instrument(const std::string& raw, const uint16_t* page, Instrument& ins, DataFiles& files) {
  ins.name = instrument_name(raw, page);
  ins.bank = true;
  const std::vector<uint8_t>& bank = files.bank();
  char key[16];
  std::memset(key, ' ', 16);
  std::memcpy(key, raw.data(), std::min<size_t>(raw.size(), 16));
  const unsigned count = le16(&bank[3]);
  for (unsigned i = 0; i < count; ++i) {
    const size_t at = 0x43 + 21 * static_cast<size_t>(i);
    if (at + 21 > bank.size()) break;
    if (std::memcmp(&bank[at], key, 16) != 0) continue;
    const uint32_t o = le32(&bank[at + 17]);
    if (static_cast<uint64_t>(o) + 12 > bank.size()) break;
    if (bank[o] == 0) {
      fm_instrument(&bank[o + 1], ins, files);
    } else {
      Bits bits(bank.data(), bank.size(), o + 1);
      sample_sound(bits, ins);
    }
    return;
  }
  refuse("the instrument \"" + raw.substr(0, 16) + "\" is not in Sound Club's bank");
}

// ---------------------------------------------------------------------------------------
// Sound Club 1

// The codes of a skip, walked one bit at a time.
const uint8_t kSkipTree[16] = {0xF0, 2, 0xF1, 4, 6, 8, 0xF2, 0xF3, 0xF5, 10, 12, 14, 0xF4, 0xF6, 0xF7, 0xFF};

void read_voice1(Bits& bits, uint32_t length, std::vector<Event>& ev) {
  const uint8_t kNone = 0xFF;
  uint8_t cur = kNone, prev = kNone, pending_note = kNone;
  bool pending = false;
  uint32_t slide_start = 0, pos = 0;
  uint32_t vol = 30, pvol = 30, bal = 15, pbal = 15;
  auto add = [&](uint32_t step, uint8_t type, uint8_t a, uint8_t b = 0, uint8_t c = 0) {
    ev.push_back({step, type, a, b, c});
  };
  for (;;) {
    uint32_t node = kSkipTree[bits.read(1)];
    while (node < 0xF0) node = kSkipTree[node + bits.read(1)];
    uint32_t skip;
    if (node == 0xFF) {
      uint32_t k = bits.read(6);
      if (k == 63) k = bits.read(14);
      skip = k + 8;
    } else {
      skip = node - 0xF0;
    }
    if (bits.overrun()) refuse("a pattern is cut off");
    if (skip > 0) {
      skip = std::min(skip, length - pos);
      pos += skip;
      if (pos == length) break;
    }
    uint32_t flags = 0, code;
    if (bits.read(1) == 0) {
      code = bits.read(1) + 1;
    } else {
      for (;;) {
        code = bits.read(5);
        if (code == 2) {
          code = bits.read(7);
          break;
        }
        if (code != 1) break;
        flags = bits.read(3);
        if (bits.read(1)) {
          if (bits.overrun()) refuse("a pattern is cut off");
          continue;
        }
        code = bits.read(1) + 1;
        break;
      }
    }
    if (code != 0) {
      if (cur == kNone) cur = 48;
      if (code == 1) {
        cur = kNone;
      } else {
        const uint32_t d = code - 1;
        if (d & 1) {
          cur = static_cast<uint8_t>(cur + d / 2);
          if (cur >= 84) cur = static_cast<uint8_t>(cur + 172);
        } else {
          cur = static_cast<uint8_t>(cur - d / 2);
          if (cur & 0x80) cur = static_cast<uint8_t>(cur + 84);
        }
      }
      cur |= 0x80;
    }
    if (flags & 2) vol = (vol + bits.read(5)) & 31;
    if (flags & 4) bal = (bal + bits.read(5)) & 31;
    if (bits.overrun()) refuse("a pattern is cut off");
    if (cur != prev && prev != kNone && !pending) add(pos, kNoteOff, prev);
    if (pending && cur != kNone) {
      cur &= 0x7F;
      if (pending_note != cur) add(slide_start, kSlide, pending_note, cur, static_cast<uint8_t>(pos - slide_start));
      pending = false;
    }
    if (vol != pvol) add(pos, kVolume, static_cast<uint8_t>(vol));
    if (bal != pbal) add(pos, kBalance, static_cast<uint8_t>(bal));
    if (cur != kNone && (cur & 0x80)) {
      cur &= 0x7F;
      add(pos, kNoteOn, cur);
    }
    if (flags & 1) {
      pending = true;
      pending_note = cur & 127;
      slide_start = pos;
    }
    pvol = vol;
    pbal = bal;
    prev = cur;
    if (++pos >= length) break;
  }
  if (prev != kNone) add(pos, kNoteOff, prev);
}

void read_pattern1(Bits& bits, size_t ninst, Pattern& p) {
  const uint32_t nv = bits.byte();
  uint32_t length = bits.byte();
  length |= bits.byte() << 8;
  if (bits.overrun()) refuse("a pattern is cut off");
  p.length = length;
  p.voices.resize(nv);
  for (Voice& v : p.voices) {
    v.instrument = bits.read(6);
    if (v.instrument > ninst) refuse("a voice has an instrument that isn't there");
  }
  int previous = 20;
  for (uint32_t step = 0; step < length;) {
    const int t = static_cast<int>(bits.read(5));
    if (bits.overrun() || t > 30) refuse("a pattern has a bad tempo");
    if (t != previous) p.tempo.push_back({step, t});
    previous = t;
    uint32_t run = bits.read(8);
    step += run ? run : 256;
  }
  for (uint32_t step = 0; step < length;) {
    uint32_t run = 1;
    if (bits.byte() == 0x20) {
      run = bits.byte();
      if (!run) run = 256;
    }
    if (bits.overrun()) refuse("a pattern is cut off");
    step += run;
  }
  for (Voice& v : p.voices) {
    if (length == 0) continue;     // a pattern of no steps has voices without events
    read_voice1(bits, length, v.events);
    sort_events(v.events);
    if (bits.overrun()) refuse("a pattern is cut off");
  }
  bits.align();
}

void load_sc1(const std::vector<uint8_t>& f, Song& song, DataFiles& files) {
  song.version = 1;
  Bits bits(f.data(), f.size());
  uint8_t h[15];
  for (uint8_t& b : h) b = static_cast<uint8_t>(bits.byte());
  if (bits.overrun() || std::memcmp(h, "SNGs", 4) != 0) refuse("not a song of Sound Club");
  std::string title;
  if (!bits.string(25, title)) refuse("the song has a bad title");
  const int nsmp = h[4], nbank = h[5], npat = h[6], nseq = h[7];
  if (!(h[10] < nseq && h[10] <= 200 && nsmp + nbank <= 64 && npat >= 1 && npat <= 200 && h[11] <= 30 &&
        h[12] <= 120 && h[13] >= 1 && h[13] <= 30 && h[14] >= 1 && h[14] <= 30))
    refuse("the song has a bad header");
  song.title = trim(to_utf8(title, kCp775));
  song.restart = h[10];
  song.global_tempo = h[12];
  const size_t ninst = static_cast<size_t>(nsmp + nbank);
  for (int i = 0; i < nseq; ++i) {
    const uint32_t s = bits.byte();
    if (bits.overrun() || s >= static_cast<uint32_t>(npat)) refuse("the song has a bad sequence");
    song.sequence.push_back(s);
  }
  song.patterns.resize(npat);
  for (Pattern& p : song.patterns) {
    std::string name;
    if (!bits.string(16, name)) refuse("a pattern has a bad name");
    read_pattern1(bits, ninst, p);
  }
  for (int i = 0; i < nsmp; ++i) {
    song.instruments.emplace_back();
    stored_instrument(bits, kCp775, song.instruments.back(), files);
  }
  for (int i = 0; i < nbank; ++i) {
    std::string name;
    if (!bits.string(16, name)) refuse("an instrument has a bad name");
    song.instruments.emplace_back();
    bank_instrument(name, kCp775, song.instruments.back(), files);
  }
}

// ---------------------------------------------------------------------------------------
// Sound Club 2

void read_voice2(const uint8_t* b, size_t n, Voice& v) {
  if (n < 4) refuse("a voice is cut off");
  v.instrument = le32(b);
  if (v.instrument > 0x7FFFFFFFu) refuse("a voice has a bad instrument");
  size_t at = 4;
  uint64_t pos = 0;
  while (at < n) {
    if (n - at < 2) refuse("a voice is cut off");
    uint64_t delta = b[at];
    unsigned type = b[at + 1] & 15;
    uint64_t size = b[at + 1] >> 4;
    size_t head = 2;
    if (delta == 0xFF) {
      if (n - at < 10) refuse("a voice is cut off");
      delta = le32(b + at + 1);
      type = b[at + 5];
      size = le32(b + at + 6);
      head = 10;
    }
    at += head;
    if (size > n - at) refuse("a voice is cut off");
    pos += delta;
    if (pos > kLongestSong) refuse("a voice is too long");
    const uint8_t* d = b + at;
    if (type >= 1 && type <= 5) {
      if (size < 1) refuse("a voice has a bad event");
      v.events.push_back({static_cast<uint32_t>(pos), static_cast<uint8_t>(type), d[0], 0, 0});
    } else if (type == 6) {
      if (size < 3) refuse("a voice has a bad event");
      v.events.push_back({static_cast<uint32_t>(pos), kSlide, d[0], d[1], d[2]});
    }
    at += size;
  }
}

void read_pattern2(const uint8_t* b, size_t n, Pattern& p) {
  if (n < 4) refuse("a pattern is cut off");
  p.length = le32(b);
  if (p.length > kLongestSong) refuse("a pattern is too long");
  size_t at = 4;
  while (at < n) {
    if (n - at < 7) refuse("a pattern has a bad chunk");
    const uint32_t m = le32(b + at + 3);
    if (m > n - at - 7) refuse("a pattern has a bad chunk");
    const uint8_t* body = b + at + 7;
    if (std::memcmp(b + at, "tem", 3) == 0) {
      if (m % 8 || p.has_tempo) refuse("a pattern has a bad tempo");
      p.has_tempo = true;
      for (uint32_t i = 0; i < m / 8; ++i) {
        const uint32_t step = le32(body + 8 * i);
        const int tempo = body[8 * i + 4];
        if (step > kLongestSong || tempo > 30) refuse("a pattern has a bad tempo");
        p.tempo.push_back({step, tempo});
      }
    } else if (std::memcmp(b + at, "voi", 3) == 0) {
      p.voices.emplace_back();
      read_voice2(body, m, p.voices.back());
    }
    at += 7 + m;
  }
}

// The releases of one voice made into note offs.
void work_out_releases(const Song& song, const Pattern& pat, Voice& v) {
  std::vector<Event>& ev = v.events;
  if (std::none_of(ev.begin(), ev.end(), [](const Event& e) { return e.type == kRelease; })) return;
  const Instrument* ins = v.instrument < song.instruments.size() ? &song.instruments[v.instrument] : nullptr;
  if (!ins || !ins->valid() || ins->loop >= 0) {
    for (Event& e : ev)
      if (e.type == kRelease) e.type = kNoteOff;
    sort_events(ev);
    return;
  }
  const size_t count = ev.size();
  const uint32_t last_step = count ? ev[count - 1].step : 0;
  int64_t start[kNotes], rel[kNotes];
  std::fill(start, start + kNotes, -1);
  std::fill(rel, rel + kNotes, -1);
  for (size_t i = 0; i <= count; ++i) {
    const bool end = i == count;
    for (int note = 0; note < kNotes; ++note) {
      const bool here = end || (ev[i].type == kNoteOn && ev[i].a == note);
      if (!here || start[note] < 0 || rel[note] < 0) continue;
      int tempo = 20;
      for (const TempoEntry& t : pat.tempo) {
        if (t.step < static_cast<uint64_t>(start[note])) tempo = t.tempo;
        else break;
      }
      const uint32_t c = 1280 - 32 * tempo;
      const uint32_t per = static_cast<uint32_t>(
          (static_cast<uint64_t>(c / 2 + song.global_tempo + 40) * ins->rate) / c);
      uint32_t e = static_cast<uint32_t>(start[note]);
      if (per) e = static_cast<uint32_t>(start[note]) + ins->length / per;
      const uint32_t limit = end ? last_step : ev[i].step;
      if (e < static_cast<uint32_t>(rel[note])) e = static_cast<uint32_t>(rel[note]);
      if (e > limit) e = limit;
      ev.push_back({e, kNoteOff, static_cast<uint8_t>(note), 0, 0});
      start[note] = rel[note] = -1;
    }
    if (end) break;
    const Event& x = ev[i];
    if (x.a >= kNotes) continue;
    if (x.type == kNoteOn) {
      start[x.a] = x.step;
      rel[x.a] = -1;
    } else if (x.type == kRelease) {
      rel[x.a] = x.step;
    } else if (x.type == kSlide && x.b < kNotes) {
      start[x.b] = start[x.a];
      rel[x.b] = rel[x.a];
      start[x.a] = rel[x.a] = -1;
    }
  }
  ev.erase(std::remove_if(ev.begin(), ev.end(), [](const Event& e) { return e.type == kRelease; }), ev.end());
  sort_events(ev);
}

void load_sc2(const std::vector<uint8_t>& f, Song& song, DataFiles& files) {
  song.version = 2;
  if (f.size() < 31 || std::memcmp(f.data(), "SN2", 3) != 0) refuse("not a song of Sound Club 2");
  const uint32_t total = le32(&f[3]);
  if (total < 24) refuse("the song has a bad header");
  const size_t end = std::min<uint64_t>(static_cast<uint64_t>(total) + 7, f.size());
  const uint32_t npat = le32(&f[7]), ninst = le32(&f[11]), gtempo = le32(&f[19]);
  if (npat > 0x7FFFFFFFu || ninst > 64 || gtempo > 120) refuse("the song has a bad header");
  song.restart = le32(&f[15]);
  song.global_tempo = static_cast<int>(gtempo);
  bool have_name = false, have_seq = false;
  size_t p = 31;
  while (p < end) {
    if (end - p < 7) refuse("the song is cut off");
    const uint32_t n = le32(&f[p + 3]);
    if (n > end - p - 7) refuse("the song is cut off");
    const uint8_t* body = &f[p + 7];
    if (std::memcmp(&f[p], "NAM", 3) == 0) {
      if (!have_name) {
        have_name = true;
        const std::string text = trim(to_utf8(std::string(reinterpret_cast<const char*>(body), n), kCp1252));
        const size_t lf = text.find('\n');
        if (lf == std::string::npos) {
          song.title = text;
        } else {
          song.title = trim(text.substr(0, lf));
          song.comment = trim(text.substr(lf + 1));
        }
      }
    } else if (std::memcmp(&f[p], "SEQ", 3) == 0) {
      if (!have_seq) {
        have_seq = true;
        if (n % 4) refuse("the song has a bad sequence");
        for (uint32_t i = 0; i < n / 4; ++i) {
          const uint32_t s = le32(body + 4 * i);
          if (s > 0x7FFFFFFFu) refuse("the song has a bad sequence");
          song.sequence.push_back(s);
        }
      }
    } else if (std::memcmp(&f[p], "PAT", 3) == 0) {
      song.patterns.emplace_back();
      read_pattern2(body, n, song.patterns.back());
    } else if (std::memcmp(&f[p], "INS", 3) == 0) {
      if (n == 0 || body[0] > 1) refuse("an instrument is of an unknown kind");
      if (song.instruments.size() >= ninst) refuse("the song has more instruments than it says");
      song.instruments.emplace_back();
      Instrument& ins = song.instruments.back();
      if (body[0] == 0) {
        Bits bits(body + 1, n - 1);
        stored_instrument(bits, kCp1252, ins, files);
      } else {
        if (n - 1 < 1 || n - 1 > 16) refuse("an instrument has a bad name");
        bank_instrument(std::string(reinterpret_cast<const char*>(body + 1), n - 1), kCp1252, ins, files);
      }
    }
    p += 7 + n;
  }
  if (song.patterns.size() != npat || song.instruments.size() != ninst || song.patterns.empty() ||
      song.sequence.empty())
    refuse("the song is not complete");
  for (uint32_t s : song.sequence)
    if (s >= song.patterns.size()) refuse("the song has a bad sequence");
  for (Pattern& pat : song.patterns) {
    for (Voice& v : pat.voices) {
      sort_events(v.events);
      work_out_releases(song, pat, v);
    }
  }
}

// After either reader: the value after the end of each sound, the song's length.
void finish(Song& song) {
  for (Instrument& ins : song.instruments) {
    ins.sound.resize(ins.length);
    ins.sound.push_back(ins.loop >= 0 && ins.length ? ins.sound[static_cast<size_t>(ins.loop)] : 0);
  }
  uint64_t steps = 0;
  for (uint32_t s : song.sequence) steps += song.patterns[s].length;
  if (steps == 0) refuse("the song is empty");
  if (steps > kLongestSong) refuse("the song is too long");
}

void load(const std::vector<uint8_t>& f, Song& song, DataFiles& files) {
  if (f.size() >= 4 && std::memcmp(f.data(), "SNGs", 4) == 0) load_sc1(f, song, files);
  else if (f.size() >= 3 && std::memcmp(f.data(), "SN2", 3) == 0) load_sc2(f, song, files);
  else refuse("not a song of Sound Club");
  finish(song);
}

long long duration_ms(const Song& song) {
  double sec = 0;
  for (uint32_t s : song.sequence) {
    const Pattern& p = song.patterns[s];
    size_t k = 0;
    int tempo = 20;
    for (uint32_t step = 0; step < p.length; ++step) {
      tempo_at(p, step, k, tempo);
      sec += static_cast<double>(song.global_tempo + 40) / static_cast<double>(1280 - 32 * tempo);
    }
  }
  return static_cast<long long>(sec * 1000 + 0.5);
}

// ---------------------------------------------------------------------------------------
// Playing

class Player {
 public:
  Player(const Song& song, int rate) : song_(song), rate_(rate) {
    for (int t = 0; t <= 30; ++t) {
      const int64_t c = 1280 - 32 * t;
      sps_[t] = static_cast<int>((static_cast<int64_t>(song.global_tempo + 40) * rate + c / 2) / c);
    }
    pitchdiv_ = static_cast<uint32_t>((26163ll * rate) / 100);
    enter();
  }

  bool ended() const { return ended_; }

  // Up to `frames` frames; fewer only at the end of the song.
  int render(int16_t* out, int frames) {
    int done = 0;
    while (done < frames) {
      if (block_at_ >= block_len_) {
        mix_block();
        block_at_ = 0;
        if (block_len_ == 0) break;
      }
      const int n = std::min(frames - done, block_len_ - block_at_);
      std::memcpy(out + 2 * done, &block_[2 * block_at_], 4 * static_cast<size_t>(n));
      block_at_ += n;
      done += n;
    }
    return done;
  }

 private:
  enum State { kFree, kPlaying, kHeld };
  struct Place {
    State state = kFree;
    size_t voice = 0;
    int note = 0;
    const Instrument* ins = nullptr;
    uint32_t pos = 0, frac = 0;
    int16_t before = 0;
    uint32_t step = 0;
    bool sliding = false;
    int32_t delta = 0;
    uint32_t interval = 0, countdown = 0, slides_left = 0;
    bool narrow = false, silent = false;
    float gl = 0, gr = 0, tl = 0, tr = 0;
    int ramp = 0;
  };
  struct VoiceState {
    int volume = 30, balance = 15;
    size_t next = 0;
  };

  const Pattern& pattern() const { return song_.patterns[song_.sequence[seq_]]; }

  void enter() {
    while (seq_ < song_.sequence.size() && song_.patterns[song_.sequence[seq_]].length == 0) ++seq_;
    if (seq_ >= song_.sequence.size()) {
      ended_ = true;
      return;
    }
    voices_.assign(pattern().voices.size(), VoiceState());
    counter_ = 0;
  }

  uint32_t pitch(const Instrument* ins, int note) const {
    const uint64_t v = (static_cast<uint64_t>(ins->rate) * kPitch[note] + pitchdiv_ / 2) / pitchdiv_;
    return static_cast<uint32_t>(std::min<uint64_t>(v, 0xFFFFFFFFu));
  }

  static void gains(int volume, int balance, float& l, float& r) {
    volume = std::clamp(volume, 0, 30);
    balance = std::clamp(balance, 0, 30);
    const int v = kVolumeGain[volume];
    int right = ((30 - balance) * v + 7) / 15;
    int left = 2 * v - right;
    const int m = std::max(v, 256);
    right = std::min(right, m);
    left = std::min(left, m);
    l = left / 1024.0f;
    r = right / 1024.0f;
  }

  const Instrument* instrument_of(const Voice& v) const {
    if (v.instrument >= song_.instruments.size()) return nullptr;
    const Instrument* ins = &song_.instruments[v.instrument];
    return ins->valid() ? ins : nullptr;
  }

  Place* held_or_playing(size_t voice, int note) {
    for (Place& p : places_)
      if ((p.state == kPlaying || p.state == kHeld) && p.voice == voice && p.note == note) return &p;
    return nullptr;
  }

  void events() {
    const Pattern& pat = pattern();
    for (size_t vi = 0; vi < pat.voices.size(); ++vi) {
      const Voice& voice = pat.voices[vi];
      VoiceState& vs = voices_[vi];
      const Instrument* ins = instrument_of(voice);
      while (vs.next < voice.events.size() && voice.events[vs.next].step <= counter_) {
        const Event& e = voice.events[vs.next++];
        if (e.step < counter_) continue;
        switch (e.type) {
          case kNoteOn: {
            if (!ins || e.a >= kNotes) break;
            Place* p = nullptr;
            for (Place& q : places_)
              if (q.state == kFree) {
                p = &q;
                break;
              }
            if (!p) {
              if (places_.size() >= kMostPlaces) break;
              places_.emplace_back();
              p = &places_.back();
            }
            *p = Place();
            p->state = kPlaying;
            p->voice = vi;
            p->note = e.a;
            p->ins = ins;
            p->step = pitch(ins, e.a);
            gains(vs.volume, vs.balance, p->gl, p->gr);
            p->tl = p->gl;
            p->tr = p->gr;
            break;
          }
          case kNoteOff:
            if (Place* p = held_or_playing(vi, e.a)) p->state = kFree;
            break;
          case kVolume:
          case kBalance: {
            (e.type == kVolume ? vs.volume : vs.balance) = e.a;
            float l, r;
            gains(vs.volume, vs.balance, l, r);
            for (Place& p : places_)
              if (p.state == kPlaying && p.voice == vi) {
                p.tl = l;
                p.tr = r;
                p.ramp = 1;
              }
            break;
          }
          case kSlide: {
            if (!ins || e.b >= kNotes) break;
            Place* p = held_or_playing(vi, e.a);
            if (!p) break;
            p->note = e.b;
            if (p->state == kHeld) break;
            const uint32_t target = pitch(ins, e.b);
            const uint32_t every = static_cast<uint32_t>(rate_ / 100);
            size_t k = 0;
            int tempo = 20;
            tempo_at(pat, counter_, k, tempo);
            uint64_t dur = 0;
            for (uint32_t j = 0; j < e.c; ++j) {
              tempo_at(pat, counter_ + j, k, tempo);
              dur += static_cast<uint64_t>(sps_[tempo]);
            }
            dur = std::min<uint64_t>(dur, 0x7FFFFFFF);
            const uint64_t count = (dur + every / 2) / every;
            p->sliding = false;
            if (count > 0) {
              p->interval = p->countdown = static_cast<uint32_t>(dur / count);
              p->slides_left = static_cast<uint32_t>(count);
              p->delta = static_cast<int32_t>((static_cast<int64_t>(target) - static_cast<int64_t>(p->step)) /
                                              static_cast<int64_t>(count));
              p->sliding = p->delta != 0 && p->interval != 0;
            }
            break;
          }
          default: break;
        }
      }
    }
  }

  void mix_samples(Place& p, float* mix, uint32_t count) {
    const Instrument& ins = *p.ins;
    const int16_t* d = ins.sound.data();
    for (uint32_t i = 0; i < count; ++i) {
      const float b = d[p.pos];
      const float a = p.pos > 0 ? d[p.pos - 1] : p.before;
      const float v = p.narrow ? a + (b - a) * (static_cast<float>(p.frac >> 12) + 0.5f) * (1.0f / 16) : b;
      if (p.ramp > 0) {
        p.gl += (p.tl - p.gl) / p.ramp;
        p.gr += (p.tr - p.gr) / p.ramp;
        if (--p.ramp == 0) {
          p.gl = p.tl;
          p.gr = p.tr;
        }
      }
      double xl, xr;
      if (p.narrow) {
        xl = std::floor(static_cast<double>(v) * p.gl);
        xr = std::floor(static_cast<double>(v) * p.gr);
      } else if (ins.bits8) {
        const int s8 = d[p.pos] >> 8;
        const int gli = static_cast<int>(p.gl * 1024) >> 2, gri = static_cast<int>(p.gr * 1024) >> 2;
        xl = s8 * gli + 128;
        xr = s8 * gri + 128;
      } else {
        xl = std::rint(static_cast<double>(d[p.pos]) * p.gl);
        xr = std::floor(static_cast<double>(d[p.pos]) * p.gr);
      }
      float* m = mix + 2 * i;
      m[0] = static_cast<float>(std::clamp(static_cast<double>(m[0]) + xl, -32768.0, 32767.0));
      m[1] = static_cast<float>(std::clamp(static_cast<double>(m[1]) + xr, -32768.0, 32767.0));
      const uint32_t f = p.frac + (p.step & 65535);
      p.pos += (p.narrow ? 0 : p.step >> 16) + (f >> 16);
      p.frac = f & 65535;
    }
  }

  void mix_place(Place& p, float* mix, uint32_t frames) {
    const Instrument& ins = *p.ins;
    const uint32_t L = ins.length;
    const int16_t* d = ins.sound.data();
    uint32_t left = frames;
    int guard = 0;
    while (left > 0) {
      uint32_t count = left;
      if (p.sliding) {
        count = std::min(count, p.countdown);
        p.countdown -= count;
      }
      if (p.pos < L) {
        const uint64_t ahead = (static_cast<uint64_t>(L - p.pos) << 16) - p.frac + p.step - 1;
        const uint64_t toend = p.step ? ahead / p.step : count;
        count = static_cast<uint32_t>(std::min<uint64_t>(count, toend));
      } else {
        count = 0;
      }
      if (p.silent) {
        const uint64_t by = static_cast<uint64_t>(p.step) * count;
        p.pos += static_cast<uint32_t>(by >> 16);
        p.frac = static_cast<uint32_t>((p.frac + by) & 65535);
      } else if (count > 0) {
        mix_samples(p, mix, count);
      }
      mix += 2 * static_cast<size_t>(count);
      left -= count;
      if (p.pos >= L) {
        if (ins.loop < 0 || ins.loop >= L) {
          p.state = kHeld;
          return;
        }
        const uint32_t loop = static_cast<uint32_t>(ins.loop);
        p.pos = p.pos - L + loop;
        if (p.pos >= L) {
          p.pos = L;
        } else {
          p.before = p.pos > 0 ? d[p.pos - 1] : d[L - 1];
          if (p.pos == loop) p.before = d[L - 1];
        }
      }
      if (p.sliding && p.countdown == 0) {
        p.step = static_cast<uint32_t>(static_cast<int32_t>(p.step) + p.delta);
        p.countdown = p.interval;
        if (--p.slides_left == 0) p.sliding = false;
      }
      if (count == 0) {
        if (++guard > 4) return;
      } else {
        guard = 0;
      }
    }
  }

  void mix_block() {
    std::fill(mix_, mix_ + 2 * kBlock, 0.0f);
    int done = 0;
    while (done < kBlock && !ended_) {
      if (left_ == 0) {
        if (counter_ >= pattern().length) {
          for (Place& p : places_) p.state = kFree;
          ++seq_;
          enter();
          if (ended_) break;
        }
        events();
        left_ = static_cast<uint32_t>(sps_[tempo_from_start(pattern(), counter_)]);
        ++counter_;
      }
      const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(kBlock - done), left_);
      for (Place& p : places_) {
        p.narrow = p.step < 65536;
        p.silent = p.state == kPlaying && p.tl == 0 && p.tr == 0 && p.ramp <= 1;
        if (p.silent) {
          p.gl = p.gr = 0;
          p.ramp = 0;
        }
      }
      for (Place& p : places_)
        if (p.state == kPlaying) mix_place(p, mix_ + 2 * done, n);
      while (!places_.empty() && places_.back().state == kFree) places_.pop_back();
      left_ -= n;
      done += static_cast<int>(n);
    }
    for (int i = 0; i < 2 * done; ++i) {
      const float v = mix_[i];
      block_[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : static_cast<int16_t>(v < 0 ? v - 0.5f : v + 0.5f);
    }
    block_len_ = done;
  }

  const Song& song_;
  int rate_;
  int sps_[31];
  uint32_t pitchdiv_;
  size_t seq_ = 0;
  uint32_t counter_ = 0, left_ = 0;
  bool ended_ = false;
  std::vector<VoiceState> voices_;
  std::vector<Place> places_;
  float mix_[2 * kBlock];
  int16_t block_[2 * kBlock];
  int block_len_ = 0, block_at_ = 0;
};

// ---------------------------------------------------------------------------------------
// Test mode: what was loaded

void dump(const std::string& prefix, const Song* song, const std::string& error, long long ms) {
  FILE* inst = std::fopen((prefix + ".inst.txt").c_str(), "w");
  FILE* evf = std::fopen((prefix + ".events.txt").c_str(), "w");
  if (!song) {
    if (inst) std::fprintf(inst, "error: %s\n", error.c_str());
    if (evf) std::fprintf(evf, "error: %s\n", error.c_str());
  } else {
    if (inst) {
      for (size_t i = 0; i < song->instruments.size(); ++i) {
        const Instrument& x = song->instruments[i];
        uint64_t h = 1469598103934665603ull;
        for (int16_t v : x.sound) {
          const uint16_t u = static_cast<uint16_t>(v);
          h = (h ^ (u & 0xFF)) * 1099511628211ull;
          h = (h ^ (u >> 8)) * 1099511628211ull;
        }
        std::fprintf(inst, "%zu kind %d valid %d rate %u length %u loop %lld bits8 %d hash %016llx name \"%s\"\n", i,
                     (x.fm ? 1 : 0) + (x.bank ? 2 : 0), x.valid() ? 1 : 0, x.rate, x.length,
                     static_cast<long long>(x.loop), x.bits8 ? 1 : 0, static_cast<unsigned long long>(h),
                     x.name.c_str());
      }
    }
    if (evf) {
      std::fprintf(evf, "type: %s\ntitle: %s\nduration: %.1f s\nglobal tempo: %d  restart: %u\n",
                   song->version == 1 ? "Sound Club" : "Sound Club 2", song->title.c_str(), ms / 1000.0,
                   song->global_tempo, song->restart < song->sequence.size() ? song->restart : 0);
      if (!song->comment.empty()) std::fprintf(evf, "comment: %s\n", song->comment.c_str());
      std::fprintf(evf, "sequence:");
      for (uint32_t s : song->sequence) std::fprintf(evf, " %u", s);
      std::fprintf(evf, "\n");
      for (size_t i = 0; i < song->instruments.size(); ++i) {
        const Instrument& x = song->instruments[i];
        std::fprintf(evf, "instrument %2zu: %-16s  %s%s  rate %5u  length %6u  loop %6lld\n", i, x.name.c_str(),
                     x.fm ? "FM" : "sample", x.bank ? " (bank)" : "", x.rate, x.length,
                     static_cast<long long>(x.loop));
      }
      for (size_t i = 0; i < song->patterns.size(); ++i) {
        const Pattern& p = song->patterns[i];
        std::fprintf(evf, "pattern %zu: %u steps, %zu voices, tempo", i, p.length, p.voices.size());
        for (const TempoEntry& t : p.tempo) std::fprintf(evf, " %d@%u", t.tempo, t.step);
        std::fprintf(evf, "\n");
        for (size_t v = 0; v < p.voices.size(); ++v) {
          const Voice& vo = p.voices[v];
          std::fprintf(evf, "  voice %zu: instrument %u, %zu events\n", v, vo.instrument, vo.events.size());
          for (const Event& e : vo.events) std::fprintf(evf, "%10u %d %d %d %d\n", e.step, e.type, e.a, e.b, e.c);
        }
      }
    }
  }
  if (inst) std::fclose(inst);
  if (evf) std::fclose(evf);
}

}  // namespace

int playSoundClub(const char* path, int /*track*/, int rate, long long startMs) {
  const std::vector<uint8_t> data = readFile(path);
  const char* dump_to = std::getenv("BBMP_SOUNDCLUB_DUMP");
  Song song;
  DataFiles files;
  try {
    load(data, song, files);
  } catch (const Refusal& r) {
    if (dump_to) dump(dump_to, nullptr, r.why, 0);
    return fail(r.why);
  }
  const long long ms = duration_ms(song);
  if (dump_to) dump(dump_to, &song, std::string(), ms);
  std::unique_ptr<Player> player(new Player(song, rate));
  if (player->ended()) return fail("the Sound Club song can't be started");

  Info info;
  info.type = song.version == 1 ? "Sound Club" : "Sound Club 2";
  info.title = song.title;
  info.comment = song.comment;
  info.durationMs = ms;
  int fm = 0, bank = 0;
  for (const Instrument& ins : song.instruments) {
    info.names.push_back(ins.name);
    fm += ins.fm;
    bank += ins.bank;
  }
  info.facts.push_back({"instruments", std::to_string(song.instruments.size()) + ", " + std::to_string(fm) +
                                           " of them FM, " + std::to_string(bank) + " from the bank"});

  std::vector<int16_t> buf(2 * 4096);
  long long skip = startMs * rate / 1000;
  while (skip > 0) {
    const int n = skip > 4096 ? 4096 : static_cast<int>(skip);
    if (player->render(buf.data(), n) < n) break;
    skip -= n;
  }
  header(info);
  for (;;) {
    const int got = player->render(buf.data(), 4096);
    if (got <= 0) break;
    if (!emit(buf.data(), static_cast<size_t>(got))) break;
  }
  return 0;
}

}  // namespace misc
