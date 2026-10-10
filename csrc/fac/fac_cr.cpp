// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-fac: play a song of FAC SoundTracker (MSX, .MUS), as raw PCM on stdout.
//
//   bbmp-fac <file> <track> <samplerate> [start-ms]
//
// FAC SoundTracker (FAC, 1990-1992; versions 1.0, 2.0 and Pro) writes for one of two
// sound chips: MSX-AUDIO (Yamaha Y8950: nine FM channels and an ADPCM channel playing
// sampled drums from a drum kit, the files <kit>.SM1 and <kit>.SM2 beside the song) or
// MSX-MUSIC (Yamaha YM2413: six FM channels and the chip's own rhythm sounds). Written for
// this player in a clean room, from a description of the format only. The chips are
// Mitsutaka Okazaki's emulators emu8950 and emu2413 (MIT licence).
//
// A song is a 16 KB memory image (84 tracks of 16 steps of 12 bytes, then a page of
// settings), usually with a 7-byte BLOAD header in front. It plays once, from step 0 to
// the end of its last track, at the MSX's 60 Hz (BBMP_FAC_HZ=50 for a 50 Hz machine),
// followed by a short tail in which the sound dies away. BBMP_FAC_DEVICE=audio|music
// forces the chip.
//
// The chips run at 3579545 / 72 Hz; their sum is brought to the output rate by a
// windowed-sinc resampler, through a DC blocker. The song is played once without sound
// first, to know its length.
//
// Same protocol as the other helpers: one JSON line describing the track, then signed
// 16-bit little-endian stereo until the song ends. A damaged file is refused with the
// reason.
//
// Test mode: with BBMP_FAC_LOG=<file>, every chip write is listed in that file, tick by
// tick, with the frames each tick lasts:
//   # fac log 1: song "<file name>" chip y8950|ym2413
//   H <the header line>
//   # dry-run ticks <T>
//   T 0             then the kit upload ("D <half> <file>" for its sample bytes) and the
//                   writes before the first tick ("ff 00": register, value)
//   T 1             then tick 1's writes, then "S <frames>", and so on
//   END             in the last tick, after its writes; then "TAIL <frames>"

#include <dirent.h>

#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "emu2413.h"
#include "emu8950.h"
}

namespace {

// ---- tables

// Note frequencies, notes 1..64: low byte of the F-number, then block and the F-number's
// top two bits (the Y8950's $B0 form, without the key bit).
const uint8_t kNoteLo[12] = {0x59, 0x6D, 0x83, 0x9A, 0xB2, 0xCC,
                             0xE8, 0x05, 0x23, 0x44, 0x67, 0x8B};
const uint8_t kNoteHi[12] = {0x09, 0x09, 0x09, 0x09, 0x09, 0x09,
                             0x09, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A};
inline uint8_t noteLo(int n) { return kNoteLo[(n - 1) % 12]; }
inline uint8_t noteHi(int n) { return uint8_t(kNoteHi[(n - 1) % 12] + 4 * ((n - 1) / 12)); }

// ADPCM DELTA-N for the drum's frequency 1..60.
const uint16_t kDeltaN[61] = {
    0,
    0x0522, 0x056A, 0x05BA, 0x0612, 0x0673, 0x06D3, 0x073B, 0x07AB, 0x081B, 0x089C,
    0x0924, 0x09AC, 0x0A3D, 0x0AD5, 0x0B7D, 0x0C2D, 0x0CE6, 0x0DA6, 0x0E7F, 0x0F57,
    0x103F, 0x1138, 0x1240, 0x1351, 0x147A, 0x15AA, 0x16FB, 0x185B, 0x19C4, 0x1B4D,
    0x1CFE, 0x1EB6, 0x2077, 0x2270, 0x2481, 0x26AA, 0x28FC, 0x2B5D, 0x2DF6, 0x30AF,
    0x3389, 0x36A2, 0x39F4, 0x3D66, 0x40F7, 0x44E1, 0x490B, 0x4D4D, 0x51F0, 0x56B2,
    0x5BEC, 0x615F, 0x671A, 0x6D45, 0x73E8, 0x7ACB, 0x81F0, 0x89C4, 0x9218, 0x9AA4,
};

// Where drum 1..15 lies in the sample memory: start and stop address (the chip's units).
// 1..8 are the 4 KB blocks, 9..12 the 8 KB blocks, 13..14 the halves, 15 the whole.
const uint16_t kDrumStart[16] = {0,      0x0000, 0x0400, 0x0800, 0x0C00, 0x1000,
                                 0x1400, 0x1800, 0x1C00, 0x0000, 0x0800, 0x1000,
                                 0x1800, 0x0000, 0x1000, 0x0000};
const uint16_t kDrumStop[16] = {0,      0x03FF, 0x07FF, 0x0BFF, 0x0FFF, 0x13FF,
                                0x17FF, 0x1BFF, 0x1FFF, 0x07FF, 0x0FFF, 0x17FF,
                                0x1FFF, 0x0FFF, 0x1FFF, 0x1FFF};

// The YM2413's register $0E for rhythm sound 1..15 ($20 rhythm mode, $10 bass drum, $08
// snare, $04 tom, $02 cymbal, $01 hi-hat).
const uint8_t kRhythm[16] = {0,    0x30, 0x2A, 0x21, 0x24, 0x22, 0x23, 0x3A,
                             0x28, 0x32, 0x31, 0x2E, 0x2B, 0x29, 0x27, 0x3F};

// The Y8950's operator offsets of channels 0..8.
const uint8_t kOp[9] = {0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12};

const int kImage = 16384;
const int kTracks = 0x3F00;     // the track area; the settings page follows
const int kHeader = 7;          // FE 00 80 FF BF and two more bytes
const long kMaxFile = kHeader + kImage + 512;
const double kPi = 3.14159265358979323846;
const double kChipRate = 3579545.0 / 72.0;
const uint32_t kChipClock = 3579545;
const uint32_t kChipRateInt = 3579545 / 72;

enum Version { V10 = 1, V20 = 2, VPro = 3 };

// ---- the song

struct Song {
  uint8_t img[kImage] = {};
  int version = V20;
  bool music = false;          // plays on MSX-MUSIC (YM2413), else MSX-AUDIO (Y8950)
  bool samples = false;        // has sampled drums (MSX-AUDIO)
  int steps = 0;               // steps played
  std::string title, author, kit;
  const uint8_t* set() const { return img + kTracks; }
  const uint8_t* step(int r) const { return img + 12 * r; }
};

inline bool printable(uint8_t b) { return b >= 0x20 && b <= 0x7E; }

// Unprintable bytes become blanks; blanks at the ends are dropped.
std::string cleaned(const uint8_t* p, int n) {
  std::string s;
  for (int i = 0; i < n; ++i) s += printable(p[i]) ? char(p[i]) : ' ';
  while (!s.empty() && s.back() == ' ') s.pop_back();
  size_t a = 0;
  while (a < s.size() && s[a] == ' ') ++a;
  return s.substr(a);
}

// A 40-byte name field: the last five bytes count only when they are text and not filler.
std::string nameField(const uint8_t* p, int version) {
  bool cut = version == VPro;
  bool allAt = true;
  for (int i = 35; i < 40; ++i) {
    if (!printable(p[i])) cut = true;
    if (p[i] != '@') allAt = false;
  }
  return cleaned(p, cut || allAt ? 35 : 40);
}

std::vector<uint8_t> readUpTo(const std::string& path, size_t most) {
  std::vector<uint8_t> d;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return d;
  d.resize(most);
  size_t n = 0;
  while (n < most) {
    size_t got = std::fread(d.data() + n, 1, most - n, f);
    if (got == 0) break;
    n += got;
  }
  std::fclose(f);
  d.resize(n);
  return d;
}

bool endsWithNoCase(const std::string& s, const char* tail) {
  size_t n = std::strlen(tail);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    char a = s[s.size() - n + i], b = tail[i];
    if (a >= 'A' && a <= 'Z') a = char(a + 32);
    if (b >= 'A' && b <= 'Z') b = char(b + 32);
    if (a != b) return false;
  }
  return true;
}

// Reads the song from the file's bytes. Returns the reason when it is refused, else "".
// `device` is 0, or 'a' / 'm' to force the chip.
std::string load(const std::vector<uint8_t>& d, Song& s, int device) {
  const size_t L = d.size();
  if (L < size_t(kImage) || L > size_t(kMaxFile))
    return "not a FAC SoundTracker song (a song is 16391 bytes)";
  static const uint8_t bload[5] = {0xFE, 0x00, 0x80, 0xFF, 0xBF};
  const bool header = std::memcmp(d.data(), bload, 5) == 0;
  const bool bare = L == size_t(kImage) && !header;
  const size_t from = bare ? 0 : kHeader;
  std::memset(s.img, 0, sizeof s.img);
  const size_t n = std::min(L - from, size_t(kImage));
  std::memcpy(s.img, d.data() + from, n);

  const uint8_t* set = s.set();
  const char* notSong = "not a FAC SoundTracker song";
  if (set[0x82] == 0 || set[0x82] > 27 || set[0xF8] > 83) return notSong;
  for (int i = 0; i < 8; ++i)
    if (!printable(set[0x84 + i])) return notSong;
  if (!header) {
    int text = 0;
    for (int i = 0; i < 35; ++i) text += printable(set[0xA6 + i]) + printable(set[0xCF + i]);
    if (text < 60) return notSong;
  }

  s.version = set[0x8C] == 2 ? V20 : set[0x8C] == 3 ? VPro : V10;
  if (s.version == VPro)
    for (int i = 0; i < kTracks; ++i) s.img[i] ^= uint8_t(i & 0xFF);
  if (s.version == V10) {
    for (int r = 0; r < kTracks / 12; ++r) {
      uint8_t* st = s.img + 12 * r;
      for (int c = 0; c < 9; ++c) st[c] = (st[c] & 0x40) ? 0x40 : (st[c] & 0x3F);
      const uint8_t dr = st[11] & 0x0F;
      st[11] = uint8_t(dr * 16 + dr);
    }
  }

  s.steps = (set[0xF8] + 1) * 16;
  bool high = false;
  s.samples = false;
  for (int r = 0; r < s.steps; ++r) {
    const uint8_t* st = s.step(r);
    for (int c = 6; c < 9; ++c)
      if (st[c] >= 0x01 && st[c] <= 0x3F) high = true;
    if (st[9] != 0 && (st[11] & 0x0F) != 0) s.samples = true;
  }
  s.music = set[0xF7] != 0 && !high;
  if (device == 'a') s.music = false;
  if (device == 'm') s.music = true;

  s.title = nameField(set + 0xA6, s.version);
  s.author = nameField(set + 0xCF, s.version);
  s.kit = cleaned(set + 0x84, 8);
  return "";
}

// ---- the drum kit

struct Kit {
  int halves = 0;                       // halves found
  std::vector<uint8_t> mem[2];          // 16 KB each
  std::string from[2];                  // the file each came from, "" for zeros
};

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c + 32);
  return s;
}

// The kit <kit>.SM1 / .SM2 in the song's folder, or else <song's name>.SM1 / .SM2.
Kit findKit(const std::string& path, const std::string& kitName) {
  Kit k;
  k.mem[0].assign(kImage, 0);
  k.mem[1].assign(kImage, 0);
  const size_t slash = path.rfind('/');
  std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
  if (dir.empty()) return k;
  std::vector<std::string> names;
  if (DIR* dp = opendir(dir.c_str())) {
    while (dirent* e = readdir(dp)) names.push_back(e->d_name);
    closedir(dp);
  }
  auto find = [&](const std::string& want) -> const std::string* {
    const std::string w = lower(want);
    for (const std::string& n : names)
      if (lower(n) == w) return &n;
    return nullptr;
  };
  std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t dot = file.rfind('.');
  if (dot != std::string::npos) file.resize(dot);
  for (const std::string& base : {kitName, file}) {
    if (base.empty()) continue;
    const std::string* one = find(base + ".SM1");
    if (!one) continue;
    const std::string* two = find(base + ".SM2");
    const std::string* half[2] = {one, two};
    for (int h = 0; h < 2; ++h) {
      if (!half[h]) continue;
      std::vector<uint8_t> d = readUpTo(dir + "/" + *half[h], 65536);
      if (d.size() < size_t(kHeader + kImage)) continue;
      std::memcpy(k.mem[h].data(), d.data() + kHeader, kImage);
      k.from[h] = *half[h];
      ++k.halves;
    }
    if (k.halves > 0) break;
  }
  return k;
}

// ---- where the writes go

struct Sink {
  OPL* opl = nullptr;
  OPLL* opll = nullptr;
  FILE* log = nullptr;
  void w(int reg, int val) {
    if (opl) OPL_writeReg(opl, uint32_t(reg), uint8_t(val));
    if (opll) OPLL_writeReg(opll, uint32_t(reg), uint8_t(val));
    if (log) std::fprintf(log, "%02x %02x\n", reg & 0xFF, val & 0xFF);
  }
};

// ---- the player

class Player {
 public:
  Player(const Song& s, bool fifty, Sink* out) : s_(s), p_(fifty ? 1 : 0), out_(out) {}

  void uploadKit(const Kit& k) {
    if (k.halves == 0) return;
    for (int h = 0; h < 2; ++h) {
      w(0x04, 0x78);
      w(0x07, 0x01);
      w(0x08, 0x00);
      w(0x09, 0x00);
      w(0x0A, h ? 0x10 : 0x00);
      w(0x0B, 0xFF);
      w(0x0C, h ? 0x1F : 0x0F);
      w(0x10, 0xF0);
      w(0x07, 0x60);
      if (out_) {
        FILE* log = out_->log;
        out_->log = nullptr;
        for (int i = 0; i < kImage; ++i) out_->w(0x0F, k.mem[h][i]);
        out_->log = log;
        if (log)
          std::fprintf(log, "D %d %s\n", h + 1, k.from[h].empty() ? "-" : k.from[h].c_str());
      }
    }
  }

  void start() {
    const uint8_t* set = s_.set();
    if (!s_.music) {
      for (int c = 0; c < 9; ++c) {
        const uint8_t* v = set + 11 * c;
        const int op = kOp[c];
        const int regs[11] = {0x20 + op, 0x23 + op, 0x40 + op, 0x43 + op, 0x60 + op,
                              0x63 + op, 0x80 + op, 0x83 + op, 0xA0 + c,  0xB0 + c,
                              0xC0 + c};
        for (int i = 0; i < 11; ++i) w(regs[i], v[i]);
      }
      w(0xBD, set[0xCE]);
      w(0x18, 0x08);
      w(0x19, 0x08);
    } else {
      for (int i = 0; i < 8; ++i) w(i, set[0x6C + i]);
      for (int i = 0; i < 6; ++i) w(0x30 + i, set[0x74 + i]);
    }
    speed_ = uint8_t(set[0x82] - p_);
    counter_ = uint8_t(speed_ - 1);
  }

  bool playing() const { return playing_; }
  long long ticks() const { return ticks_; }

  // The last drum started: its length in nibbles, DELTA-N and the tick it started.
  bool drumStarted = false;
  long long drumNib = 0, drumTick = 0;
  int drumDeltaN = 0;

  // One tick (the last one stops the player).
  void tick() {
    ++ticks_;
    counter_ = uint8_t(counter_ + 1);
    if (counter_ != speed_) {
      between();
      return;
    }
    counter_ = 0;
    if (pos_ >= s_.steps) {
      for (int c = 0; c < channels(); ++c) {
        w(fa(c), 0);
        w(fb(c), 0);
      }
      playing_ = false;
      return;
    }
    playStep(s_.step(pos_));
    ++pos_;
  }

 private:
  enum { None, Up, Down };
  const Song& s_;
  const int p_;
  Sink* out_;
  bool keyOff_[9] = {true, true, true, true, true, true, true, true, true};
  uint8_t detune_[9] = {}, bend_[9] = {}, dir_[9] = {}, lo_[9] = {}, hi_[9] = {};
  uint8_t v36_ = 0xF0, v37_ = 0x70, v27_ = 0x01, v0e_ = 0x20;
  int pos_ = 0;
  uint8_t speed_ = 0, counter_ = 0;
  bool playing_ = true;
  long long ticks_ = 0;

  void w(int reg, int val) {
    if (out_) out_->w(reg, val);
  }
  int channels() const { return s_.music ? 6 : 9; }
  int fa(int c) const { return s_.music ? 0x10 + c : 0xA0 + c; }
  int fb(int c) const { return s_.music ? 0x20 + c : 0xB0 + c; }

  // The two frequency bytes as the chip takes them.
  int faValue(int c) const {
    return s_.music ? ((lo_[c] >> 1) | ((hi_[c] & 1) << 7)) & 0xFF : lo_[c];
  }
  int fbValue(int c, bool key) const {
    if (s_.music) return (hi_[c] >> 1) | 0x80 | (key ? 0x10 : 0);
    return hi_[c] | (key ? 0x20 : 0);
  }
  void send(int c, bool key) {
    w(fa(c), faValue(c));
    w(fb(c), fbValue(c, key));
  }
  void store(int c, int lo, int hi) {
    lo_[c] = uint8_t(lo);
    hi_[c] = uint8_t(hi);
    send(c, !keyOff_[c]);
  }

  // Move the frequency up or down by k, stepping across the octave as the table does.
  void raise(int c, int k) {
    const int lo = lo_[c], hi = hi_[c];
    if (hi == 0 || k == 0) return;
    if ((hi & 1) == 0) {
      const int x = (lo + k) & 0xFF;
      if (x >= 0xB2)
        store(c, x - 0xB2 + 0x59, ((hi ^ 3) + 4) & 0xFF);
      else
        store(c, x, hi);
    } else {
      const int x = lo + k;
      if (x > 255) {
        int h = hi ^ 3;
        if (h & 1) h = (h + 4) & 0xFF;
        store(c, x - 256, h);
      } else {
        store(c, x, hi);
      }
    }
  }
  void lowerBy(int c, int k) {
    const int lo = lo_[c], hi = hi_[c];
    if (hi == 0 || k == 0) return;
    if (hi & 1) {
      const int x = (lo - k) & 0xFF;
      if (x < 0x46)
        store(c, (x - 0x45 + 0x8B) & 0xFF, ((hi ^ 3) - 4) & 0xFF);
      else
        store(c, x, hi);
    } else {
      if (lo < k) {
        int h = hi ^ 3;
        if ((h & 1) == 0) h = (h - 4) & 0xFF;
        store(c, lo - k + 256, h);
      } else {
        store(c, lo - k, hi);
      }
    }
  }

  void bends() {
    for (int c = 0; c < channels(); ++c) {
      if (dir_[c] == Up) raise(c, bend_[c]);
      else if (dir_[c] == Down) lowerBy(c, bend_[c]);
    }
  }

  void rhythm() {
    w(0x16, 0x20);
    w(0x17, 0x57);
    w(0x18, 0xAA);
    w(0x36, v36_);
    w(0x37, v37_);
    w(0x38, 0x0F);
    w(0x26, 0x05);
    w(0x27, v27_);
    w(0x28, 0x00);
    w(0x0E, v0e_);
  }

  void between() {
    bends();
    if (v27_ != 1) v27_ = uint8_t(v27_ - 2);
    v0e_ = 0x20;
    if (s_.music) rhythm();
  }

  void code(int c, uint8_t a) {
    const uint8_t* set = s_.set();
    if (a == 0x00) return;
    if (a >= 0x80) {
      const int v = (a & 0x3F) ^ 0x3F;
      if (a < 0xC0) {                                     // volume
        if (!s_.music) w(0x43 + kOp[c], (set[11 * c + 3] & 0xC0) | v);
        else w(0x30 + c, (set[0x74 + c] & 0xF0) | (v >> 2));
      } else {                                            // brightness
        if (!s_.music) w(0x40 + kOp[c], (set[11 * c + 2] & 0xC0) | v);
        else if ((set[0x74 + c] & 0xF0) == 0) w(0x02, (set[0x6E] & 0xC0) | v);
      }
      return;
    }
    if (a == 0x40) {                                      // note off
      keyOff_[c] = true;
      send(c, false);
    } else if (a == 0x41) {                               // detune up
      detune_[c] = uint8_t(detune_[c] + 1);
      raise(c, 1);
    } else if (a == 0x42) {                               // detune down
      detune_[c] = uint8_t(detune_[c] - 1);
      lowerBy(c, 1);
    } else if (a >= 0x43 && a <= 0x56) {                  // bend up / down
      const bool up = a <= 0x4C;
      const int amount = a - (up ? 0x43 : 0x4D);
      dir_[c] = None;
      if (amount != 0) {
        bend_[c] = uint8_t(amount);
        dir_[c] = up ? Up : Down;
      }
    } else if (a >= 0x57 && a <= 0x70) {                  // tempo
      speed_ = uint8_t(0x72 - a - p_);
    } else {                                              // a note
      const int n = a & 0x3F;
      const bool sounding = !keyOff_[c];
      keyOff_[c] = false;
      dir_[c] = None;
      lo_[c] = uint8_t(noteLo(n) + detune_[c]);
      hi_[c] = noteHi(n);
      w(fa(c), faValue(c));
      if (sounding) w(fb(c), fbValue(c, false));
      w(fb(c), fbValue(c, true));
    }
  }

  void playStep(const uint8_t* st) {
    for (int c = 0; c < channels(); ++c) code(c, st[c]);
    if (!s_.music) {
      const int f = st[9], drum = st[11] & 0x0F;
      if (f != 0) {
        if (f <= 60) {
          w(0x11, kDeltaN[f] >> 8);
          w(0x10, kDeltaN[f] & 0xFF);
        }
        w(0x12, st[10]);
        w(0x07, 0x01);
        if (f <= 60 && drum != 0) {
          const int a = kDrumStart[drum], b = kDrumStop[drum];
          w(0x0A, a >> 8);
          w(0x09, a & 0xFF);
          w(0x0C, b >> 8);
          w(0x0B, b & 0xFF);
          w(0x07, 0xA0);
          drumStarted = true;
          drumNib = (long long)(b - a + 1) * 8;
          drumDeltaN = kDeltaN[f];
          drumTick = ticks_;
        }
      }
      bends();
    } else {
      const int r = st[11] >> 4;
      if (r != 0) {
        const int u = (s_.version == V10 && st[10] == 0) ? 15 : st[10] >> 4;
        v0e_ = kRhythm[r];
        if (v0e_ & 0x08) v27_ = 5;
        v36_ = uint8_t(((v36_ & 0xF0) | u) ^ 0x0F);
        v37_ = uint8_t(((v37_ & 0xF0) | ((u >> 1) + 7)) ^ 0x0F);
      }
      bends();
      rhythm();
    }
  }
};

// ---- the length

struct Length {
  long long ticks = 0;
  double songSeconds = 0, tail = 1.5;
};

double tickHz(bool fifty) {
  return 21477270.0 / double(fifty ? 1368 * 313 : 1368 * 262);
}

Length measure(const Song& s, bool fifty, int kitHalves) {
  Player dry(s, fifty, nullptr);
  dry.start();
  while (dry.playing()) dry.tick();
  Length len;
  const double hz = tickHz(fifty);
  len.ticks = dry.ticks();
  len.songSeconds = double(len.ticks) / hz;
  if (kitHalves >= 1) {
    double first = 0;
    long long t0 = 0;
    if (dry.drumStarted) {
      first = double(dry.drumNib) / (kChipRate * double(dry.drumDeltaN) / 65536.0);
      t0 = dry.drumTick;
    }
    const double rem = first - double(len.ticks - t0) / hz + 0.3;
    if (rem > 1.5) len.tail = rem < 8.0 ? rem : 8.0;
  }
  return len;
}

// ---- from the chip to the output

class Resampler {
 public:
  explicit Resampler(int rate) {
    const double cut = 0.92 * (double(rate) < kChipRate ? double(rate) / kChipRate : 1.0);
    for (int q = 0; q <= 512; ++q) {
      double sum = 0;
      for (int t = 0; t < 32; ++t) {
        const double x = double(t - 15) - double(q) / 512.0;
        const double w = 0.42 + 0.5 * std::cos(kPi * x / 16.0) +
                         0.08 * std::cos(2.0 * kPi * x / 16.0);
        const double s = x == 0 ? 1.0 : std::sin(kPi * cut * x) / (kPi * cut * x);
        const double v = s * (std::fabs(x) < 16 ? w : 0.0);
        table_[q][t] = float(v);
        sum += v;
      }
      for (int t = 0; t < 32; ++t) table_[q][t] = float(double(table_[q][t]) / sum);
    }
    step_ = kChipRate / double(rate);
  }
  template <class Chip>
  float next(Chip&& chipSample) {
    while (phase_ >= 1.0) {
      std::memmove(hist_, hist_ + 1, sizeof(float) * 31);
      hist_[31] = chipSample();
      phase_ -= 1.0;
    }
    const float* k = table_[int(phase_ * 512)];
    float acc = 0.0f;
    for (int t = 0; t < 32; ++t) {
      const float prod = hist_[t] * k[t];
      acc = acc + prod;
    }
    phase_ += step_;
    return acc;
  }

 private:
  float table_[513][32];
  float hist_[32] = {};
  double phase_ = 0.0, step_ = 1.0;
};

// The whole run: the header is printed by the caller; this plays and writes `skip`
// frames silently, then the rest to `out` (nullptr: nowhere). Returns false when the
// output closed.
struct Render {
  const Song& song;
  const Kit& kit;
  bool fifty;
  int rate;
  Length len;
  FILE* log = nullptr;

  bool run(long long skip, FILE* out, long long stopAfter = -1) {
    Sink sink;
    sink.log = log;
    OPL* opl = nullptr;
    OPLL* opll = nullptr;
    if (song.music) {
      opll = OPLL_new(kChipClock, kChipRateInt);
      OPLL_reset(opll);
      sink.opll = opll;
    } else {
      opl = OPL_new(kChipClock, kChipRateInt);
      OPL_reset(opl);
      sink.opl = opl;
    }
    Player player(song, fifty, &sink);
    if (log) std::fprintf(log, "T 0\n");
    if (!song.music) player.uploadKit(kit);
    player.start();

    auto chipSample = [&]() -> float {
      int sum = 0;
      if (opl) {
        OPL_calc(opl);
        for (int i = 0; i < 15; ++i) sum += opl->ch_out[i];
      } else {
        OPLL_calc(opll);
        for (int i = 0; i < 14; ++i) sum += opll->ch_out[i];
      }
      return float(sum);
    };
    std::unique_ptr<Resampler> rs(new Resampler(rate));
    const double R = 1.0 - (2.0 * kPi) * 5.0 / double(rate);
    double inPrev = 0, outPrev = 0;
    const double hz = tickHz(fifty);
    const double perTick = double(rate) / hz;
    double due = 0.0;
    const long long total = (long long)((len.songSeconds + len.tail) * double(rate));
    const long long fadeFrom = (long long)((len.songSeconds + len.tail - 0.5) * double(rate));
    long long since = 0;      // frames since the last tick
    bool ended = false;
    bool open = true;
    std::vector<int16_t> buf;
    buf.reserve(8192);
    long long frames = total;
    if (stopAfter >= 0 && stopAfter < frames) frames = stopAfter;

    for (long long i = 0; i < frames; ++i) {
      while (player.playing() && double(i) >= due) {
        if (log && player.ticks() > 0) std::fprintf(log, "S %lld\n", since);
        since = 0;
        if (log) std::fprintf(log, "T %lld\n", player.ticks() + 1);
        player.tick();
        if (!player.playing()) {
          ended = true;
          if (log) std::fprintf(log, "END\n");
        }
        due += perTick;
      }
      const float value = rs->next(chipSample);
      const double in = double(value) * 2.0;
      double o = (in - inPrev) + R * outPrev;
      inPrev = in;
      outPrev = o;
      if (o < -32768.0) o = -32768.0;
      if (o > 32767.0) o = 32767.0;
      long v = std::lrint(o);
      if (i >= fadeFrom)
        v = long(double(v) * double(total - i) / double(total - fadeFrom));
      ++since;
      if (i >= skip && out && open) {
        buf.push_back(int16_t(v));
        buf.push_back(int16_t(v));
        if (buf.size() >= 8192) {
          if (std::fwrite(buf.data(), 2, buf.size(), out) != buf.size()) open = false;
          buf.clear();
        }
      }
    }
    if (out && open && !buf.empty())
      if (std::fwrite(buf.data(), 2, buf.size(), out) != buf.size()) open = false;
    if (out && open && std::fflush(out) != 0) open = false;
    if (log) std::fprintf(log, ended ? "TAIL %lld\n" : "S %lld\n", since);
    if (opl) OPL_delete(opl);
    if (opll) OPLL_delete(opll);
    return open;
  }
};

std::string jsonText(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += (c < 0x20 || c > 0x7E) ? ' ' : char(c);
  }
  return o + "\"";
}

std::string headerLine(const Song& s, const Kit& kit, const Length& len) {
  std::string type = "FAC SoundTracker ";
  type += s.version == VPro ? "Pro" : s.version == V20 ? "2.0" : "1.0";
  type += s.music ? ", MSX-MUSIC" : ", MSX-AUDIO";
  std::string comment;
  if (!s.music && s.samples) {
    if (kit.halves == 2) comment = "Drums: " + s.kit;
    else if (kit.halves == 1) comment = "Drums: " + s.kit + " (only half of the kit was found)";
    else comment = "The drums are missing: the sample kit " + s.kit + ".SM1/.SM2 was not found";
  }
  const long long ms = (long long)((len.songSeconds + len.tail) * 1000.0);
  return "{\"tracks\":1,\"track\":0,\"title\":" + jsonText(s.title) +
         ",\"author\":" + jsonText(s.author) + ",\"type\":" + jsonText(type) +
         ",\"duration_ms\":" + std::to_string(ms) + ",\"comment\":" + jsonText(comment) +
         ",\"names\":[]}";
}

int fail(const char* why) {
  std::printf("{\"tracks\":0,\"error\":\"%s\"}\n", why);
  std::fflush(stdout);
  return 1;
}

int deviceFromEnv() {
  const char* d = std::getenv("BBMP_FAC_DEVICE");
  if (!d) return 0;
  const std::string v = lower(d);
  return v == "audio" ? 'a' : v == "music" ? 'm' : 0;
}

bool fiftyFromEnv() {
  const char* h = std::getenv("BBMP_FAC_HZ");
  return h && std::atoi(h) == 50;
}

}  // namespace

#ifndef FAC_NO_MAIN
int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: bbmp-fac <file> <track> <samplerate> [start-ms]\n");
    return 2;
  }
  std::signal(SIGPIPE, SIG_IGN);
  const std::string path = argv[1];
  const int track = std::atoi(argv[2]);
  const int rate = std::atoi(argv[3]);
  const long long startMs = argc > 4 ? std::atoll(argv[4]) : 0;
  if (rate < 8000 || rate > 192000) return fail("unsupported sample rate");
  const std::vector<uint8_t> file = readUpTo(path, 65536);
  if (file.empty()) return fail("can't read the file");
  if (endsWithNoCase(path, ".sm1") || endsWithNoCase(path, ".sm2"))
    return fail("a sample kit of FAC SoundTracker, not a song");
  Song song;
  const std::string why = load(file, song, deviceFromEnv());
  if (!why.empty()) return fail(why.c_str());
  if (track != 0) return fail("a FAC SoundTracker song has one track");

  const bool fifty = fiftyFromEnv();
  Kit kit;
  if (!song.music && song.samples) kit = findKit(path, song.kit);
  const Length len = measure(song, fifty, kit.halves);
  const std::string head = headerLine(song, kit, len);
  std::printf("%s\n", head.c_str());
  std::fflush(stdout);

  FILE* log = nullptr;
  if (const char* to = std::getenv("BBMP_FAC_LOG")) {
    log = std::fopen(to, "w");
    if (log) {
      const size_t slash = path.rfind('/');
      const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
      std::fprintf(log, "# fac log 1: song \"%s\" chip %s\nH %s\n# dry-run ticks %lld\n",
                   name.c_str(), song.music ? "ym2413" : "y8950", head.c_str(), len.ticks);
    }
  }
  long long skip = 0;
  if (startMs > 0) skip = startMs > LLONG_MAX / 192000 ? LLONG_MAX : startMs * rate / 1000;
  Render r{song, kit, fifty, rate, len, log};
  r.run(skip, stdout);
  if (log) std::fclose(log);
  return 0;
}
#endif
