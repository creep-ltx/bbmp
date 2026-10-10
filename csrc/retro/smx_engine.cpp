// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-retro, engine "sccmusixx": SCC-Musixx songs (*.sng), a tracker for the MSX and
// Konami's SCC wave-table chip (Tyfoon Soft, 1990). Five voices, each playing a 32-byte
// wave; the fifth voice shares the fourth voice's wave.
//
// The tracker's replay routine may not be shipped, so this replayer was written for this
// player from a description of what the routine does with a song (which value goes to
// which SCC register, and when), without the original code: a clean room. The chip is
// emu2212 by Mitsutaka Okazaki (MIT), which the build fetches.
//
// The song is put at address 0 of a 64 KiB image that is otherwise zero, and every read
// goes through that image with the address taken modulo 64 KiB: a damaged song reads
// zeros, never outside its buffer. The song's length is found first by playing it once
// without the chip, until a position is entered a second time.
//
// The chip runs at eight times the output rate; its samples are filtered down to the
// output rate by a 97-tap low-pass filter. The output is mono, written as stereo.
//
// For testing: with BBMP_SMX_LOG=<file> the engine first writes every chip write of the
// whole song, tick by tick, to <file>, in the format of the clean-room description.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "emu2212.h"
#include "engine.h"

using namespace retro;

namespace {

// The engine's name; one place, so that renaming it is a one-line change.
const char* const kEngineName = "sccmusixx";

const double kTickHz = 21477270.0 / 1368 / 313;   // one PAL frame of an MSX2
const int kOversample = 8;                        // chip samples per output frame
const int kTaps = 97;
const long long kMostTicks = 4194304;             // safety limit of the dry run

const int kSizeMin = 0x7e5 + 0x600;   // 3557: the tables and one whole pattern
const int kSizeMax = 32768;
const int kLengthAt = 0x780;          // the song length
const int kOrderAt = 0x781;           // the position list
const int kStartAt = 0x201;           // the start position
const int kPatternsAt = 0x7e5;
const int kPatternSize = 0x600;       // 64 rows of 24 bytes
const int kRowSize = 24;
const int kInstrSize = 40;            // 32 bytes of wave, 8 of name
const int kInstruments = 48;

// Where each voice's fields start in a row.
const int kVoiceAt[5] = {0, 5, 10, 15, 20};

struct Voice {
  uint16_t pitch = 0;        // playing pitch: what goes to the chip
  uint16_t base = 0;         // the pitch before vibrato
  uint8_t written[2] = {0, 0};
  bool transposeUp = false;
  uint8_t transpose = 0;
  uint8_t volume = 0;
  uint8_t effect = 0;        // the active effect
  uint8_t slideStep = 0, slideExtra = 0;
  bool slideUp = false;      // true: the period grows (effect 2)
  uint8_t volStep = 0, volExtra = 0;
  bool volDown = false;
  uint8_t vibStartPhase = 0, vibStartDepth = 0;
  uint8_t vibPhase = 0, vibDepth = 0;
  uint8_t wave = 0;
};

// The replay itself: the same code plays the dry run (no chip), the log and the sound.
class Replay {
 public:
  Replay(const uint8_t* image, SCC* chip, FILE* log) : mem_(image), chip_(chip), log_(log) {}

  void start() {
    write(0x9000, 0x3f);
    for (unsigned a = 0x9800; a <= 0x9889; a++) write(a, 0);
    write(0x98e0, 0);
    speed_ = 8;
    countdown_ = 1;
    vibClock_ = 1;
    row_ = 0;
    std::memset(entered_, 0, sizeof(entered_));
    for (Voice& v : voice_) v = Voice();
    const uint8_t length = mem_[kLengthAt];
    uint8_t first = mem_[kStartAt];
    if (first == length) first = 0;
    enter(first);
  }

  // One tick. True when a position was entered a second time during it.
  bool tick() {
    repeated_ = false;
    if (--countdown_ == 0) {
      read_row();
      countdown_ = speed_;
    }
    for (Voice& v : voice_) {
      if (v.effect == 1 || v.effect == 2) slide_pitch(v);
      else if (v.effect == 5) slide_volume(v);
    }
    if (--vibClock_ == 0) {
      vibClock_ = 3;
      for (Voice& v : voice_) {
        v.vibPhase++;
        if (v.vibDepth) {
          if (v.vibPhase & 1) v.pitch += v.vibDepth;
          else v.pitch -= v.vibDepth;
        }
      }
    }
    for (int i = 0; i < 5; i++) write(0x988a + i, voice_[i].volume);
    write(0x988f, 0x1f);
    for (int i = 0; i < 10; i++) {
      Voice& v = voice_[i / 2];
      const uint8_t b = (i & 1) ? v.pitch >> 8 : v.pitch & 0xff;
      if (b != v.written[i & 1]) {
        write(0x9880 + i, b);
        v.written[i & 1] = b;
      }
    }
    return repeated_;
  }

 private:
  const uint8_t* mem_;   // 64 KiB
  SCC* chip_;
  FILE* log_;
  Voice voice_[5];
  int speed_ = 8, countdown_ = 1, vibClock_ = 1, row_ = 0;
  uint8_t position_ = 0;
  uint16_t rowAt_ = 0;
  bool entered_[256];
  bool repeated_ = false;

  uint8_t rd(unsigned address) const { return mem_[address & 0xffff]; }

  void write(unsigned address, unsigned value) {
    if (chip_) SCC_write(chip_, address, value);
    if (log_) std::fprintf(log_, "%04x %02x\n", address, value & 0xff);
  }

  void enter(uint8_t q) {
    position_ = q;
    if (entered_[q]) repeated_ = true;
    entered_[q] = true;
    rowAt_ = static_cast<uint16_t>(kPatternsAt + rd(kOrderAt + q) * kPatternSize);
  }

  uint8_t next_position() const {
    const uint8_t n = position_ + 1;
    return n == rd(kLengthAt) ? 0 : n;
  }

  void move_pitch(Voice& v, uint8_t amount, bool up) {
    if (up) {
      v.base += amount;
      v.pitch += amount;
    } else {
      v.base -= amount;
      v.pitch -= amount;
    }
  }

  // Reads the row at rowAt_, and once more after a break or a jump (at most once).
  void read_row() {
    for (;;) {
      const uint16_t at = rowAt_;
      auto cell = [&](int v, int k) { return rd(at + kVoiceAt[v] + k); };
      auto control = [&](int v) { return cell(v, v == 4 ? 2 : 3); };
      auto param = [&](int v) { return cell(v, v == 4 ? 3 : 4); };
      // notes
      for (int i = 0; i < 5; i++) {
        const uint8_t lo = cell(i, 0), hi = cell(i, 1);
        if (!lo && !hi) continue;
        Voice& v = voice_[i];
        uint16_t p = lo | hi << 8;
        p = v.transposeUp ? p + v.transpose : p - v.transpose;
        v.pitch = v.base = p;
        v.vibPhase = v.vibStartPhase;
        v.vibDepth = v.vibStartDepth;
      }
      // volumes
      for (int i = 0; i < 5; i++) voice_[i].volume = control(i) >> 4;
      // waves
      for (int i = 0; i < 4; i++) {
        const uint8_t n = cell(i, 2);
        if (!n || n == voice_[i].wave) continue;
        voice_[i].wave = n;
        write(0x988f, 0);
        const unsigned from = (n - 1) * kInstrSize;
        for (int k = 0; k < 32; k++) write(0x9800 + 32 * i + k, rd(from + k));
        write(0x988f, 0x1f);
      }
      // effects
      bool jumped = false;
      for (int i = 0; i < 5 && !jumped; i++) {
        Voice& v = voice_[i];
        const uint8_t e = control(i) & 15, x = param(i);
        uint8_t keep = 0;
        switch (e) {
          case 0: case 8: case 13:
            keep = e;
            break;
          case 1: case 2:
            v.slideUp = e == 2;
            v.slideStep = x / speed_;
            v.slideExtra = x % speed_;
            keep = e;
            break;
          case 3: case 4:
            v.vibStartPhase = v.vibPhase = e == 3 ? 1 : 0;
            v.vibStartDepth = v.vibDepth = x;
            v.pitch = v.base;
            break;
          case 5: {
            const uint8_t n = x & 15;
            v.volStep = n / speed_;
            v.volDown = (x >> 4) != 0;
            v.volExtra = n % speed_;
            keep = 5;
            break;
          }
          case 6: case 7:
            if (v.transpose) move_pitch(v, v.transpose, !v.transposeUp);
            v.transposeUp = e == 7;
            v.transpose = x;
            move_pitch(v, x, v.transposeUp);
            break;
          case 9:
            write(0x98e0, x);
            break;
          case 10:
            if (row_ != 0) {
              enter(next_position());
              row_ = 0;
              jumped = true;
            }
            break;
          case 11:
            if (row_ != 0) {
              const uint8_t last = rd(kLengthAt) - 1;
              enter(x <= last ? x : 0);
              row_ = 0;
              jumped = true;
            }
            break;
          case 15:
            speed_ = x < 2 ? 2 : x;
            break;
          default:   // 12, 14: signals for a host program
            break;
        }
        if (!jumped) v.effect = keep;
      }
      if (jumped) continue;   // row_ is 0 now: the new row cannot jump again
      rowAt_ += kRowSize;
      if (++row_ == 64) {
        row_ = 0;
        enter(next_position());
      }
      return;
    }
  }

  void slide_pitch(Voice& v) {
    uint8_t step = v.slideStep;
    if (v.slideExtra) {
      v.slideExtra--;
      step++;
    }
    move_pitch(v, step, v.slideUp);
  }

  void slide_volume(Voice& v) {
    uint8_t n = v.volStep;
    if (v.volExtra) {
      v.volExtra--;
      n++;
    }
    const int vol = v.volume;
    if (!v.volDown) {
      int w = (vol + n) & 15;
      if (w < vol) w = 15;
      v.volume = w;
    } else {
      v.volume = n > vol ? 0 : vol - n;
    }
  }
};

// The checks of a song file, in the order the description gives them.
bool check(const std::vector<uint8_t>& file, std::string& error) {
  const size_t size = file.size();
  if (size < static_cast<size_t>(kSizeMin)) {
    error = "too short for an SCC-Musixx song";
    return false;
  }
  if (size > static_cast<size_t>(kSizeMax)) {
    error = "too long for an SCC-Musixx song";
    return false;
  }
  const int length = file[kLengthAt];
  if (length < 1 || length > 100) {
    error = "not an SCC-Musixx song: " + std::to_string(length) + " positions";
    return false;
  }
  for (int i = 0; i < length; i++) {
    if (file[kOrderAt + i] >= 20) {
      error = "not an SCC-Musixx song: a position asks for pattern " +
              std::to_string(file[kOrderAt + i]);
      return false;
    }
  }
  for (int k = 0; k < kInstruments; k++) {
    for (int j = 32; j < kInstrSize; j++) {
      const uint8_t c = file[k * kInstrSize + j];
      if (c != 0 && (c < 32 || c > 126)) {
        error = "not an SCC-Musixx song: an instrument's name is no text";
        return false;
      }
    }
  }
  const size_t patterns = (size - 2021) / kPatternSize;
  long long cells = 0, bad = 0;
  for (size_t p = 0; p < patterns; p++) {
    for (int r = 0; r < 64; r++) {
      const uint8_t* row = &file[kPatternsAt + p * kPatternSize + r * kRowSize];
      for (int v = 0; v < 5; v++) {
        cells++;
        if (row[kVoiceAt[v] + 1] > 15) bad++;
        else if (v < 4 && row[kVoiceAt[v] + 2] > kInstruments) bad++;
      }
    }
  }
  if (bad * 50 > cells) {
    error = "not an SCC-Musixx song: the patterns make no sense";
    return false;
  }
  return true;
}

class SccMusixx : public Engine {
 public:
  ~SccMusixx() override {
    if (chip_) SCC_delete(chip_);
  }

  bool open(const std::vector<uint8_t>& file, const std::string& path, int, int rate,
            SongInfo& info, std::string& error) override {
    if (!check(file, error)) return false;
    image_.assign(65536, 0);
    std::memcpy(image_.data(), file.data(), file.size());

    // the dry run: the song's length in ticks
    {
      Replay dry(image_.data(), nullptr, nullptr);
      dry.start();
      while (ticks_ < kMostTicks && !dry.tick()) ticks_++;
    }
    if (ticks_ <= 0) {
      error = "the song has no length";
      return false;
    }

    info.tracks = 1;
    info.type = "SCC-Musixx";
    info.duration_ms = static_cast<long long>(ticks_ * 1000.0 / kTickHz + 0.5);
    for (int k = 0; k < kInstruments; k++) {
      const std::string name = field(&image_[k * kInstrSize + 32], 8);
      if (!name.empty()) info.names.push_back(name);
    }

    rate_ = rate;
    samplesPerTick_ = static_cast<double>(rate * kOversample) / kTickHz;
    make_filter();

    if (const char* logName = std::getenv("BBMP_SMX_LOG")) write_log(logName, path, info);

    chip_ = SCC_new(3579545, rate * kOversample);
    if (!chip_) {
      error = "out of memory";
      return false;
    }
    SCC_reset(chip_);
    SCC_set_type(chip_, SCC_STANDARD);
    replay_.reset(new Replay(image_.data(), chip_, nullptr));
    replay_->start();
    return true;
  }

  size_t render(int16_t* out, size_t frames) override {
    size_t made = 0;
    while (made < frames) {
      if (left_ == 0) {
        if (tick_ >= ticks_) break;
        tick_++;
        replay_->tick();
        left_ = next_tick_samples();
      }
      left_--;
      // the history is kept twice over, so the newest 97 samples are always in one piece
      const double s = SCC_calc(chip_);
      history_[at_] = history_[at_ + kTaps] = s;
      at_ = at_ + 1 == kTaps ? 0 : at_ + 1;
      if (++phase_ < kOversample) continue;
      phase_ = 0;
      const double* h = &history_[at_];   // the oldest first
      double sum = 0.0;
      for (int i = 0; i < kTaps; i++) sum += taps_[i] * h[i];
      double y = sum * 2.0;
      if (y > 32767.0) y = 32767.0;
      if (y < -32768.0) y = -32768.0;
      const int16_t v = static_cast<int16_t>(y);
      out[2 * made] = out[2 * made + 1] = v;
      made++;
    }
    return made;
  }

 private:
  std::vector<uint8_t> image_;
  SCC* chip_ = nullptr;
  std::unique_ptr<Replay> replay_;
  long long ticks_ = 0, tick_ = 0;
  int rate_ = 48000;
  double samplesPerTick_ = 0, fraction_ = 0;
  long long left_ = 0;
  double taps_[kTaps];
  double history_[2 * kTaps] = {};
  int at_ = 0, phase_ = 0;

  long long next_tick_samples() {
    fraction_ += samplesPerTick_;
    const double k = std::floor(fraction_);
    fraction_ -= k;
    return static_cast<long long>(k);
  }

  // Blackman-windowed sinc, cut-off 0.45 of the output's Nyquist frequency.
  void make_filter() {
    const double pi = 3.14159265358979323846;
    const double f = 0.45 / kOversample;
    double total = 0.0;
    for (int i = 0; i < kTaps; i++) {
      const double x = i - 48.0;
      const double sinc = x == 0.0 ? 2 * f : std::sin(((2 * pi) * f) * x) / (pi * x);
      const double window = 0.42 - 0.5 * std::cos(((2 * pi) * i) / 96) +
                            0.08 * std::cos(((4 * pi) * i) / 96);
      taps_[i] = sinc * window;
      total += taps_[i];
    }
    for (int i = 0; i < kTaps; i++) taps_[i] /= total;
  }

  // The whole song's chip writes, tick by tick (BBMP_SMX_LOG).
  void write_log(const char* logName, const std::string& path, const SongInfo& info) {
    FILE* log = std::fopen(logName, "w");
    if (!log) return;
    const auto slash = path.rfind('/');
    const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    std::string names;
    for (const std::string& n : info.names) {
      std::string q;
      for (char c : n) {   // the names are plain ASCII text (checked)
        if (c == '"' || c == '\\') q += '\\';
        q += c;
      }
      names += (names.empty() ? "\"" : ",\"") + q + "\"";
    }
    std::fprintf(log, "# sccmusixx scc log 1: song \"%s\"\n", base.c_str());
    std::fprintf(log,
                 "H {\"tracks\":1,\"track\":0,\"title\":\"\",\"author\":\"\",\"type\":\"SCC-Musixx\","
                 "\"duration_ms\":%lld,\"comment\":\"\",\"names\":[%s]}\n",
                 info.duration_ms, names.c_str());
    std::fprintf(log, "# ticks %lld rate %d oversample %d\n", ticks_, rate_, kOversample);
    std::fprintf(log, "T 0\n");
    Replay r(image_.data(), nullptr, log);
    r.start();
    for (long long t = 1; t <= ticks_; t++) {
      std::fprintf(log, "T %lld\n", t);
      r.tick();
      std::fprintf(log, "S %lld\n", next_tick_samples());
    }
    std::fprintf(log, "END\n");
    std::fclose(log);
    fraction_ = 0;
  }
};

}  // namespace

REGISTER_ENGINE(kEngineName, SccMusixx)
