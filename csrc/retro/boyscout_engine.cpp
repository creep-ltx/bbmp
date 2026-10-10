// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-retro, engine "boyscout": BoyScout (Christer Andersson, 2001-2005), *.bsf.
//
// BoyScout is a Windows tracker for the four Game Boy sound channels of the Game Boy
// Advance. A .bsf is its working file; for the GBA the tracker exports a packed .bgf,
// which its playback library (Library/BoyScout.c of BoyScout 1.0, free to use) plays
// once a frame. This engine reads the .bsf and plays it the way that library plays the
// export of it (only from the song's Begin to its End, as the export does): the same
// beat counting, the same order of events, and the same values written to the same
// sound registers, which here go to a GBA sound unit written for this engine.
//
// Where the library and the tracker part ways, the tracker's intent is followed:
// - Sound 3's length: the library writes SOUND3CNT_H twice, the second time with a
//   read-modify-write that on the hardware leaves the length at 0 (one second); the
//   length the song sets is used.
// - Sound 1's frequency: the library ORs the start bit into SOUND1CNT_X by read-modify-
//   write, whose frequency bits read as 0; the note's frequency is written with it.
//
// The .bsf layout (read from the files; every number is 32 bits, 10000 means "empty"):
//   version (1), beat length (frames), sequencer beats per row, begin, end (beats),
//   octave, view;
//   then for each of the four channels: six slider values, the chosen preset, the
//   chosen pattern, the number of presets, the presets (a 16-byte name and the
//   channel's parameters; for Sound 3 the name and 16 bytes of wave, two steps a byte,
//   the first in the low nibble), the number of patterns and the patterns (the length
//   in beats, a 16-byte name, and each parameter's column: Sound 1 envelope, envelope
//   steps, length, duty, sweep shifts, sweep time, note; Sound 2 envelope, envelope
//   steps, length, duty, note; Sound 3 envelope, length, wave preset, note; Sound 4
//   envelope, envelope steps, length, divider, shift, 7-step flag, note);
//   then the sequencer: for each channel 10000 rows of pattern numbers.
//
// The GBA sound unit here: the four Game Boy channels as GBATEK describes them, at
// 2^22 clocks a second with a frame sequencer at 512 Hz (length 256 Hz, sweep 128 Hz,
// envelope 64 Hz), Sound 3 with its two wave banks, and the library's settings of the
// mixer (full volume, all channels on both sides, PSG at 100%).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

#include "engine.h"

namespace {

using namespace retro;

const int kEmpty = 10000;
const double kClock = 4194304.0;                       // the PSG's clock
// The library runs once a GBA frame (59.73 a second); the tracker, whose beat lengths
// are in 60ths of a second, at 60: its WAV renders of the songs are 0.45% faster than
// the GBA's frames would make them. The songs were made with the tracker.
const double kFrameRate = 60.0;

// ---------------------------------------------------------------- the sound unit

class Psg {
 public:
  explicit Psg(int rate) : clocksPerSample_(kClock / rate) {
    const double dt = 1.0 / rate;
    highBeta_ = dt / (dt + 1.0 / (2 * M_PI * 20));
    std::memset(wave_, 0, sizeof(wave_));
    if (const char* m = std::getenv("BBMP_BOYSCOUT_CHANNELS")) mask_ = std::strtoul(m, nullptr, 0) & 15;
    raw_ = std::getenv("BBMP_BOYSCOUT_RAW") != nullptr;
  }

  void write(unsigned reg, unsigned v) {
    switch (reg) {
      case 0x60: sq_[0].sweepShift = v & 7; sq_[0].sweepDown = v & 8; sq_[0].sweepTime = (v >> 4) & 7; break;
      case 0x62: squareEnvelope(sq_[0], v); break;
      case 0x64: squareControl(sq_[0], v, true); break;
      case 0x68: squareEnvelope(sq_[1], v); break;
      case 0x6c: squareControl(sq_[1], v, false); break;
      case 0x70:
        wv_.dimension = v & 0x20;
        wv_.bank = (v >> 6) & 1;
        wv_.dac = v & 0x80;
        if (!wv_.dac) wv_.on = false;
        break;
      case 0x72:
        wv_.length = 256 - (v & 0xff);
        wv_.volume = (v >> 13) & 3;
        wv_.force = v & 0x8000;
        break;
      case 0x74:
        wv_.freq = v & 0x7ff;
        wv_.lengthOn = v & 0x4000;
        if (v & 0x8000) {
          wv_.on = wv_.dac;
          if (wv_.length == 0) wv_.length = 256;
          wv_.timer = (2048 - wv_.freq) * 2;
          wv_.pos = 0;
        }
        break;
      case 0x78:
        ns_.length = 64 - (v & 63);
        envelope(ns_.env, v >> 8);
        if (!ns_.env.dac) ns_.on = false;
        break;
      case 0x7c:
        ns_.divider = v & 7;
        ns_.narrow = v & 8;
        ns_.shift = (v >> 4) & 15;
        ns_.lengthOn = v & 0x4000;
        if (v & 0x8000) {
          ns_.on = ns_.env.dac;
          if (ns_.length == 0) ns_.length = 64;
          ns_.env.volume = ns_.env.initial;
          ns_.env.timer = ns_.env.period;
          ns_.lfsr = 0x7fff;
          ns_.timer = noisePeriod();
        }
        break;
    }
  }

  // Wave RAM as the CPU writes it: into the bank that isn't playing.
  void writeWave(unsigned offset, uint8_t v) { wave_[(wv_.bank ^ 1) * 16 + (offset & 15)] = v; }

  bool silent() const { return !sq_[0].on && !sq_[1].on && !wv_.on && !ns_.on; }

  void run(double seconds, std::deque<int16_t>& out) {
    clocksDue_ += seconds * kClock;
    while (clocksDue_ >= clocksPerSample_ - carry_) {
      const double want = clocksPerSample_ - carry_;
      const unsigned n = static_cast<unsigned>(want);
      carry_ = n - want + 1.0;        // n + 1 clocks are run
      clocksDue_ -= n + 1;
      double sum = 0;
      for (unsigned k = 0; k <= n; k++) sum += clock();
      const double level = sum / (n + 1);          // 0..60
      high_ += highBeta_ * (level - high_);
      double s = raw_ ? level * 512 : (level - high_) * (32767.0 / 48.0);
      if (s > 32767) s = 32767;
      if (s < -32768) s = -32768;
      out.push_back(static_cast<int16_t>(std::lrint(s)));
    }
  }

 private:
  struct Envelope {
    unsigned initial = 0, period = 0, timer = 0, volume = 0;
    bool up = false, dac = false;
  };
  struct Square {
    Envelope env;
    unsigned duty = 0, freq = 0, length = 0, pos = 0;
    int timer = 0;
    bool on = false, lengthOn = false;
    unsigned sweepShift = 0, sweepTime = 0, sweepTimer = 0, shadow = 0;
    bool sweepDown = false, sweepOn = false;
  };
  struct Wave {
    bool dimension = false, dac = false, on = false, lengthOn = false, force = false;
    unsigned bank = 0, length = 0, volume = 0, freq = 0, pos = 0;
    int timer = 0;
    uint8_t sample = 0;
  };
  struct Noise {
    Envelope env;
    unsigned divider = 0, shift = 0, length = 0, lfsr = 0x7fff;
    int timer = 0;
    bool narrow = false, on = false, lengthOn = false;
  };

  static void envelope(Envelope& e, unsigned v) {
    e.period = v & 7;
    e.up = v & 8;
    e.initial = (v >> 4) & 15;
    e.dac = (v & 0xf8) != 0;
  }
  void squareEnvelope(Square& s, unsigned v) {
    s.length = 64 - (v & 63);
    s.duty = (v >> 6) & 3;
    envelope(s.env, v >> 8);
    if (!s.env.dac) s.on = false;
  }
  void squareControl(Square& s, unsigned v, bool sweep) {
    s.freq = v & 0x7ff;
    s.lengthOn = v & 0x4000;
    if (!(v & 0x8000)) return;
    s.on = s.env.dac;
    if (s.length == 0) s.length = 64;
    s.env.volume = s.env.initial;
    s.env.timer = s.env.period;
    s.timer = (2048 - s.freq) * 4;
    s.pos = 0;
    if (sweep) {
      s.shadow = s.freq;
      s.sweepTimer = s.sweepTime ? s.sweepTime : 8;
      s.sweepOn = s.sweepTime || s.sweepShift;
      if (s.sweepShift && sweepNext(s) > 2047) s.on = false;
    }
  }
  static unsigned sweepNext(const Square& s) {
    const unsigned d = s.shadow >> s.sweepShift;
    return s.sweepDown ? s.shadow - d : s.shadow + d;
  }
  int noisePeriod() const {
    static const int kDiv[8] = {8, 16, 32, 48, 64, 80, 96, 112};
    return kDiv[ns_.divider] << ns_.shift;
  }

  static void stepEnvelope(Envelope& e) {
    if (!e.period) return;
    if (e.timer) e.timer--;
    if (e.timer) return;
    e.timer = e.period;
    if (e.up && e.volume < 15) e.volume++;
    if (!e.up && e.volume > 0) e.volume--;
  }

  void frameStep() {
    const unsigned step = frameStep_++ & 7;
    if (!(step & 1)) {   // length, 256 Hz
      for (Square& s : sq_)
        if (s.lengthOn && s.length && --s.length == 0) s.on = false;
      if (wv_.lengthOn && wv_.length && --wv_.length == 0) wv_.on = false;
      if (ns_.lengthOn && ns_.length && --ns_.length == 0) ns_.on = false;
    }
    if (step == 2 || step == 6) {   // sweep, 128 Hz
      Square& s = sq_[0];
      if (s.sweepTimer) s.sweepTimer--;
      if (!s.sweepTimer) {
        s.sweepTimer = s.sweepTime ? s.sweepTime : 8;
        if (s.sweepOn && s.sweepTime) {
          const unsigned f = sweepNext(s);
          if (f > 2047) {
            s.on = false;
          } else if (s.sweepShift) {
            s.shadow = s.freq = f;
            if (sweepNext(s) > 2047) s.on = false;
          }
        }
      }
    }
    if (step == 7) {   // envelopes, 64 Hz
      stepEnvelope(sq_[0].env);
      stepEnvelope(sq_[1].env);
      stepEnvelope(ns_.env);
    }
  }

  // One clock of the unit; the sum of the channels' outputs (0..60).
  unsigned clock() {
    if (++frameTimer_ >= 8192) {
      frameTimer_ = 0;
      frameStep();
    }
    // the duty cycles, high part first and from the start of a note, as the tracker
    // plays them (the hardware's phase differs, which can't be heard)
    static const uint8_t kDuty[4] = {0x80, 0xc0, 0xf0, 0xfc};
    unsigned sum = 0;
    for (int c = 0; c < 2; c++) {
      Square& s = sq_[c];
      if (--s.timer <= 0) {
        s.timer += (2048 - s.freq) * 4;
        s.pos = (s.pos + 1) & 7;
      }
      if (s.on && (kDuty[s.duty] >> (7 - s.pos) & 1) && (mask_ >> c & 1)) sum += s.env.volume;
    }
    if (--wv_.timer <= 0) {
      wv_.timer += (2048 - wv_.freq) * 2;
      if (wv_.on) {
        const unsigned steps = wv_.dimension ? 64 : 32;
        wv_.pos = (wv_.pos + 1) % steps;
        const unsigned bank = wv_.dimension ? wv_.pos / 32 : wv_.bank;
        const uint8_t b = wave_[bank * 16 + (wv_.pos % 32) / 2];
        wv_.sample = (wv_.pos & 1) ? (b & 15) : (b >> 4);
      }
    }
    if (wv_.on && (mask_ & 4)) {
      if (wv_.force) sum += wv_.sample * 3 / 4;
      else if (wv_.volume) sum += wv_.sample >> (wv_.volume - 1);
    }
    if (--ns_.timer <= 0) {
      ns_.timer += noisePeriod();
      const unsigned bit = (ns_.lfsr ^ (ns_.lfsr >> 1)) & 1;
      ns_.lfsr = (ns_.lfsr >> 1) | (bit << 14);
      if (ns_.narrow) ns_.lfsr = (ns_.lfsr & ~0x40u) | (bit << 6);
    }
    if (ns_.on && !(ns_.lfsr & 1) && (mask_ & 8)) sum += ns_.env.volume;
    return sum;
  }

  Square sq_[2];
  Wave wv_;
  Noise ns_;
  uint8_t wave_[32];
  unsigned frameTimer_ = 0, frameStep_ = 0;
  // for comparing with the tracker: the channels heard (BBMP_BOYSCOUT_CHANNELS) and
  // the sum of the channels as it is, without the high-pass (BBMP_BOYSCOUT_RAW)
  unsigned mask_ = 15;
  bool raw_ = false;
  double clocksPerSample_, clocksDue_ = 0, carry_ = 0;
  double highBeta_, high_ = 0;
};

// ---------------------------------------------------------------- the song

struct Pattern {
  int length = 0;
  std::vector<std::vector<int>> params;   // [parameter][beat]
};

struct Song {
  int beatLength = 1, beatsPerRow = 1, begin = 0, end = 0;
  std::vector<Pattern> patterns[4];
  std::vector<std::vector<uint8_t>> waves;   // 16 bytes each, as the GBA wants them
  std::vector<int> sequencer[4];             // from the Begin row on
};

const int kParams[4] = {7, 5, 4, 7};
const int kRows = 10000;

struct Reader {
  const std::vector<uint8_t>& d;
  size_t at = 0;
  bool bad = false;
  int i32() {
    if (at + 4 > d.size()) {
      bad = true;
      return 0;
    }
    const int v = static_cast<int>(le32(d.data() + at));
    at += 4;
    return v;
  }
  const uint8_t* bytes(size_t n) {
    if (at + n > d.size()) {
      bad = true;
      return nullptr;
    }
    const uint8_t* p = d.data() + at;
    at += n;
    return p;
  }
};

bool readSong(const std::vector<uint8_t>& file, Song& song, std::string& error) {
  Reader r{file};
  int header[7];
  for (int& h : header) h = r.i32();
  if (header[0] != 1 || header[1] < 1 || header[1] > 255 || header[2] < 1 || header[2] > 255 ||
      header[3] < 0 || header[4] <= header[3]) {
    error = "not a BoyScout song";
    return false;
  }
  song.beatLength = header[1];
  song.beatsPerRow = header[2];
  song.begin = header[3];
  // the sequencer has 10000 rows; after them nothing plays (a damaged End would
  // otherwise be hours of silence, and rows past 2^31 beats overflow)
  song.end = std::min(header[4], kRows * header[2]);
  if (song.end <= song.begin) {
    error = "not a BoyScout song";
    return false;
  }
  for (int ch = 0; ch < 4; ch++) {
    for (int k = 0; k < 6 + 2; k++) r.i32();   // sliders, chosen preset and pattern
    const int presets = r.i32();
    if (presets < 0 || presets > 1000) {
      error = "not a BoyScout song";
      return false;
    }
    for (int k = 0; k < presets && !r.bad; k++) {
      r.bytes(16);
      if (ch == 2) {
        const uint8_t* w = r.bytes(16);
        if (!w) break;
        // two steps a byte, the first in the low nibble; the GBA plays the high one first
        std::vector<uint8_t> wave(16);
        for (int b = 0; b < 16; b++) wave[b] = static_cast<uint8_t>(w[b] << 4 | w[b] >> 4);
        song.waves.push_back(wave);
      } else {
        for (int p = 0; p < kParams[ch]; p++) r.i32();
      }
    }
    const int patterns = r.i32();
    if (patterns < 0 || patterns > 10000) {
      error = "not a BoyScout song";
      return false;
    }
    for (int k = 0; k < patterns && !r.bad; k++) {
      Pattern pat;
      pat.length = r.i32();
      r.bytes(16);
      if (pat.length < 0 || pat.length > 65535) {
        error = "a pattern's length is wrong";
        return false;
      }
      pat.params.assign(kParams[ch], std::vector<int>(pat.length));
      for (int p = 0; p < kParams[ch]; p++)
        for (int b = 0; b < pat.length; b++) pat.params[p][b] = r.i32();
      song.patterns[ch].push_back(pat);
    }
  }
  if (r.bad || file.size() - r.at != 4u * 4 * kRows) {
    error = "the song's layout is not the one known here";
    return false;
  }
  const int firstRow = song.begin / song.beatsPerRow;
  const int rows = (song.end - song.begin + song.beatsPerRow - 1) / song.beatsPerRow;
  for (int ch = 0; ch < 4; ch++) {
    for (int row = 0; row < kRows; row++) {
      const int v = r.i32();
      if (row >= firstRow && row < firstRow + rows) song.sequencer[ch].push_back(v);
    }
  }
  return true;
}

// ---------------------------------------------------------------- the library's player

// BoyScoutPlaySong / BoyScoutUpdateSong of BoyScout.c, on the song as read above.
class Player {
 public:
  Player(const Song& song, Psg* psg) : s_(song), psg_(psg) {}

  void start() {
    // BoyScoutInitialize: the mixer; here fixed in Psg
    position_ = 0;
    tick_ = s_.beatLength;
    rowBeats_ = 0;
    row_ = 0;
    for (int ch = 0; ch < 4; ch++) {
      patPos_[ch] = 0;
      playing_[ch] = sequence(ch, 0);
    }
    for (int ch = 0; ch < 4; ch++)
      for (int p = 0; p < 7; p++) params_[ch][p] = 0;
    params_[0][0] = params_[1][0] = params_[3][0] = 15;
    params_[2][0] = 4;
    playBank_ = 1;
    playWave_ = kEmpty;
    const int w = value(2, 2);
    if (w != kEmpty && w >= 0 && w < static_cast<int>(s_.waves.size())) {
      playWave_ = w;
      write(0x70, 0x40);           // play bank 1, so that bank 0 is written
      copyWave(w);
      write(0x70, 0x00);           // play bank 0
    }
  }

  // One frame. False when the song has ended.
  bool update() {
    if (++tick_ < s_.beatLength) return true;
    tick_ = 0;

    for (int ch = 0; ch < 4; ch++) {
      if (playing_[ch] < 0) continue;
      const int P = kParams[ch];
      if (value(ch, P - 1) != kEmpty) {
        for (int p = 0; p < P; p++)
          if (value(ch, p) != kEmpty) params_[ch][p] = static_cast<int16_t>(value(ch, p));
        strike(ch);
      }
      if (++patPos_[ch] >= s_.patterns[ch][playing_[ch]].length) playing_[ch] = -1;
    }

    rowBeats_++;
    if (++position_ >= s_.end - s_.begin) return false;
    if (rowBeats_ >= s_.beatsPerRow) {
      rowBeats_ = 0;
      row_++;
      for (int ch = 0; ch < 4; ch++) {
        const int p = sequence(ch, row_);
        if (p >= 0) {
          patPos_[ch] = 0;
          playing_[ch] = p;
        } else if (rawSequence(ch, row_) != kEmpty) {
          // a pattern that was deleted: the tracker plays nothing there
          playing_[ch] = -1;
        }
      }
    }

    // Sound 3's next wave, into the bank that isn't playing
    const int w = value(2, 2);
    if (w != playWave_ && w != kEmpty && w >= 0 && w < static_cast<int>(s_.waves.size())) {
      playWave_ = w;
      copyWave(w);
      playBank_ ^= 1;
    }
    return true;
  }

 private:
  int rawSequence(int ch, int row) const {
    return row < static_cast<int>(s_.sequencer[ch].size()) ? s_.sequencer[ch][row] : kEmpty;
  }
  int sequence(int ch, int row) const {
    const int v = rawSequence(ch, row);
    return v >= 0 && v < static_cast<int>(s_.patterns[ch].size()) ? v : -1;
  }
  // The value of a parameter at the current beat of the channel's pattern (as the
  // library's iterators have it: they stay on the last beat once the pattern is over).
  int value(int ch, int p) const {
    if (playing_[ch] < 0) return lastValue(ch, p);
    const Pattern& pat = s_.patterns[ch][playing_[ch]];
    if (pat.length == 0) return kEmpty;
    return pat.params[p][patPos_[ch] < pat.length ? patPos_[ch] : pat.length - 1];
  }
  int lastValue(int, int) const { return kEmpty; }

  static unsigned frequency(int note) {
    static const unsigned short kNotes[72] = {
      44, 156, 262, 363, 457, 547, 631, 710, 786, 854, 923, 986,
      1046, 1102, 1155, 1205, 1253, 1297, 1339, 1379, 1417, 1452, 1486, 1517,
      1546, 1575, 1602, 1627, 1650, 1673, 1694, 1714, 1732, 1750, 1767, 1783,
      1798, 1812, 1825, 1837, 1849, 1860, 1871, 1881, 1890, 1899, 1907, 1915,
      1923, 1930, 1936, 1943, 1949, 1954, 1959, 1964, 1969, 1974, 1978, 1982,
      1985, 1988, 1992, 1995, 1998, 2001, 2004, 2006, 2009, 2011, 2015, 2015};
    const int i = note - 36;
    return kNotes[i < 0 ? 0 : i > 71 ? 71 : i];
  }

  static unsigned envelopeBits(int volume, int steps) {
    const unsigned v = static_cast<unsigned>(volume) & 15;
    if (steps <= 0) return ((-steps) & 7) << 8 | v << 12;
    return (steps & 7) << 8 | 1 << 11 | v << 12;
  }

  void strike(int ch) {
    const int16_t* q = params_[ch];
    switch (ch) {
      case 0: {
        write(0x62, (q[2] & 63) | (q[3] & 3) << 6 | envelopeBits(q[0], q[1]));
        if (q[4] <= 0) write(0x60, ((-q[4]) & 7) | 8 | (q[5] & 7) << 4);
        else write(0x60, (q[4] & 7) | (q[5] & 7) << 4);
        write(0x64, frequency(q[6]) | 0x4000 | 0x8000);
        break;
      }
      case 1:
        write(0x68, (q[2] & 63) | (q[3] & 3) << 6 | envelopeBits(q[0], q[1]));
        write(0x6c, frequency(q[4]) | 0x4000 | 0x8000);
        break;
      case 2: {
        write(0x70, playBank_ == 1 ? 0x00 : 0x40);
        write(0x70, (playBank_ == 1 ? 0x00 : 0x40) | 0x80);
        static const unsigned kVolume[5] = {0, 3 << 13, 2 << 13, 1 << 15, 1 << 13};
        const unsigned volume = q[0] >= 0 && q[0] <= 4 ? kVolume[q[0]] : 0;
        write(0x72, (q[1] & 0xff) | volume);
        write(0x74, frequency(q[3]) | 0x4000 | 0x8000);
        break;
      }
      case 3: {
        write(0x78, (q[2] & 63) | envelopeBits(q[0], q[1]));
        write(0x7c, (q[3] & 7) | (q[5] ? 8 : 0) | (q[4] & 15) << 4 | 0x4000 | 0x8000);
        break;
      }
    }
  }

  void copyWave(int w) {
    if (!psg_) return;
    for (int b = 0; b < 16; b++) psg_->writeWave(b, s_.waves[w][b]);
  }
  void write(unsigned reg, unsigned v) {
    if (psg_) psg_->write(reg, v & 0xffff);
  }

  const Song& s_;
  Psg* psg_;
  int position_ = 0, tick_ = 0, rowBeats_ = 0, row_ = 0;
  int playing_[4] = {-1, -1, -1, -1};
  int patPos_[4] = {};
  int16_t params_[4][7] = {};
  int playBank_ = 1, playWave_ = kEmpty;
};

class BoyscoutEngine : public Engine {
 public:
  bool open(const std::vector<uint8_t>& file, const std::string&, int track, int rate,
            SongInfo& info, std::string& error) override {
    if (!readSong(file, song_, error)) return false;
    if (track != 0) {
      error = "no such track";
      return false;
    }
    bool any = false;
    for (int ch = 0; ch < 4; ch++)
      for (int v : song_.sequencer[ch])
        if (v >= 0 && v < static_cast<int>(song_.patterns[ch].size())) any = true;
    if (!any) {
      error = "the song is empty";
      return false;
    }
    info.tracks = 1;
    info.type = "BoyScout";
    const long long frames = static_cast<long long>(song_.end - song_.begin) * song_.beatLength;
    info.duration_ms = static_cast<long long>(frames * 1000.0 / kFrameRate);
    psg_.reset(new Psg(rate));
    player_.reset(new Player(song_, psg_.get()));
    player_->start();
    if (debug_on())
      std::fprintf(stderr, "boyscout: %d frames a beat, %d beats a row, beats %d to %d, %zu waves\n",
                   song_.beatLength, song_.beatsPerRow, song_.begin, song_.end, song_.waves.size());
    return true;
  }

  size_t render(int16_t* out, size_t frames) override {
    while (samples_.size() < frames && !done_) {
      if (playing_) {
        if (!player_->update()) playing_ = false;
      } else {
        // what still sounds dies away, a second at most
        if (psg_->silent() || ++tail_ > kFrameRate) {
          done_ = true;
          break;
        }
      }
      psg_->run(1.0 / kFrameRate, samples_);
    }
    size_t n = 0;
    for (; n < frames && !samples_.empty(); n++) {
      out[2 * n] = out[2 * n + 1] = samples_.front();
      samples_.pop_front();
    }
    return n;
  }

 private:
  Song song_;
  std::unique_ptr<Psg> psg_;
  std::unique_ptr<Player> player_;
  std::deque<int16_t> samples_;
  bool playing_ = true, done_ = false;
  int tail_ = 0;
};

}  // namespace

REGISTER_ENGINE("boyscout", BoyscoutEngine)
