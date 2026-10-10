// SPDX-License-Identifier: GPL-3.0-or-later
// MONOTONE songs (.mon): Jim Leonard's (Trixter's) tracker for the PC speaker, 2008-2019.
//
// A port of the tracker's own player (MTSRC/MT_PLAY.PAS, TMTV1Player) and of its PC
// speaker output (MTSRC/MT_OUTP.PAS, TPCSpeakerSimple, as MT_EDIT.PAS sets it up:
// equalShare off), from github.com/MobyGamer/MONOTONE (The Unlicense: public domain).
//
// The file (all of it little-endian):
//   0    "\x08MONOTONE"  (a Pascal string[8])
//   9    title, string[40] (a length byte and 40 characters)
//   50   comment, string[40]
//   91   version (1), patterns, tracks, cell size (2)
//   95   the order list, 256 pattern numbers, 0xFF after the last
//   351  the patterns: 64 rows of <tracks> cells of 2 bytes, a word
//        nnnnnnne eedddddd: note (1 = A0 .. 100 = C9, 127 = off), effect, data
//
// The player runs 60 times a second. It works out a frequency for each track; the PC
// speaker can sound one, so the output device plays them in turn, one per 1/60 s
// (the first four tracks only), starting again with the first on every new row, and
// holds the tone of a track that is silent. The tone is the timer chip's square wave:
// 1193182 Hz divided by a whole number.
//
// Differences from the tracker, on purpose: a position jump (Bxx) back to where the
// song has been ends the song (the tracker loops for ever; its disk writer stops at
// every Bxx), and the tracker's last tick of row 0 of the last pattern after the end
// is not played.

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>

#include "common.h"

namespace pcmusic {

namespace {

const int kIBN = 8;                      // intervals between notes (1/8 semitone)
const int kMaxNote = 3 + 8 * 12 + 1;     // 100: C9
const int kNoteEnd = 127;
const int kMaxRows = 64;
const int kSpeakerVoices = 4;            // the PC speaker device plays four tracks at most
const double kPitClock = 14318181.818181818 / 12.0;
const int kPitFreq = 1193182;            // round() of it, as the program has it
const uint16_t kSilent = 0xFFFF;
const double kLevel = 7000;              // as the player's other PC speaker replayer

struct Channel {
  bool active = false;
  int noteInterval = 0;
  uint16_t frequency = 0;
};

struct Vars {
  uint8_t note = 0, effect = 0, parm1 = 0, parm2 = 0, lastNote = 0;
  uint16_t effectRaw = 0;
  uint16_t portaStopHz = 0;
  uint8_t portaDelta = 0, vibSpeed = 0, vibDepth = 0, vibIndex = 0;
};

class Monotone {
 public:
  bool load(const std::vector<uint8_t>& file, std::string& why) {
    if (file.size() < 351 || std::memcmp(file.data(), "\x08MONOTONE", 9) != 0) {
      why = "not a MONOTONE song";
      return false;
    }
    title_ = fromCp437(std::string(reinterpret_cast<const char*>(&file[10]), std::min<int>(file[9], 40)));
    comment_ = fromCp437(std::string(reinterpret_cast<const char*>(&file[51]), std::min<int>(file[50], 40)));
    version_ = file[91];
    patterns_ = file[92];
    tracks_ = file[93];
    if (version_ != 1 || tracks_ < 1 || tracks_ > 12 || file[94] != 2) {
      why = "a MONOTONE song of a kind this player doesn't know";
      return false;
    }
    std::memcpy(orders_, &file[95], 256);
    // The tracker keeps all 256 patterns; those not in the file are empty.
    const size_t patternSize = size_t(kMaxRows) * tracks_ * 2;
    data_.assign(256 * patternSize, 0);
    const size_t have = std::min(file.size() - 351, size_t(patterns_) * patternSize);
    std::memcpy(data_.data(), &file[351], have);
    if (orders_[0] == 0xFF) {
      why = "the MONOTONE song is empty";
      return false;
    }

    // notesHz: the frequency of every 1/8 semitone, from A0 = 27.5 Hz
    const double interval = std::exp(std::log(2.0) / (12 * kIBN));
    double hz = 27.5;
    notesHz_[0] = 440;
    notesHz_[kIBN] = 28;                       // round(27.5)
    for (int n = kIBN - 1; n >= 1; n--) {
      hz /= interval;
      if (hz < 19) hz = 19;
      notesHz_[n] = static_cast<uint16_t>(std::lround(hz));
    }
    hz = 27.5;
    for (int n = kIBN + 1; n <= kMaxNote * kIBN; n++) {
      hz *= interval;
      notesHz_[n] = static_cast<uint16_t>(std::lround(hz));
    }
    for (int b = 0; b < 32; b++)
      vibTable_[b] = static_cast<int8_t>(std::lround(64 * std::sin(b * M_PI / 32 * 2)));
    voices_ = tracks_ < kSpeakerVoices ? tracks_ : kSpeakerVoices;
    return true;
  }

  void start() {
    tempo_ = tracks_ < 4 ? 4 : tracks_;       // the player's default: the song's tracks, at least 4
    order_ = 0;
    pattern_ = orders_[0];
    row_ = 0;
    tickCount_ = tempo_;
    playing_ = true;
    started_ = false;
    for (auto& c : ch_) c = Channel();
    for (auto& v : vars_) v = Vars();
    dominant_ = 0;
    lastFreq_ = kSilent;
    visited_.clear();
  }

  // One 1/60 s: the player, then the speaker. False once the song is over.
  bool tick() {
    if (!playing_) return false;
    if (!started_) {                           // pa_PlaySongTop: this tick only starts
      started_ = true;
    } else {
      tickCount_++;
      if (tickCount_ >= tempo_) {
        tickCount_ = 0;
        if (row_ >= kMaxRows) {
          order_++;
          if (order_ > 255 || orders_[order_] == 0xFF) {
            playing_ = false;
            return false;
          }
          pattern_ = orders_[order_];
          row_ = 0;
        }
        if (!visited_.insert(order_ * 64 + row_).second) {   // looped
          playing_ = false;
          return false;
        }
        rowTick();
        row_++;
      } else {
        effectTick();
      }
      if (order_ > 255 || orders_[order_] == 0xFF) {       // a Dxx on the last pattern
        playing_ = false;
        return false;
      }
    }
    speaker();
    return true;
  }

  uint16_t frequency() const { return lastFreq_; }

  std::string title_, comment_;
  int tracks_ = 0, patterns_ = 0, version_ = 0;

 private:
  uint16_t hz(int interval) const {
    if (interval < 0) interval = 0;
    if (interval > kMaxNote * kIBN) interval = kMaxNote * kIBN;
    return notesHz_[interval];
  }

  void rowTick() {
    static const uint8_t kVisual[8] = {0x0, 0x1, 0x2, 0x3, 0x4, 0xB, 0xD, 0xF};
    const size_t rowAt = (size_t(pattern_) * kMaxRows + row_) * tracks_ * 2;
    for (int c = 0; c < voices_; c++) {
      const uint16_t w = data_[rowAt + 2 * c] | (data_[rowAt + 2 * c + 1] << 8);
      Vars& v = vars_[c];
      v.note = w >> 9;
      const int eff = (w >> 6) & 7, dat = w & 63;
      if (eff == 0 && dat == 0) {
        v.effectRaw = 0;
        v.effect = v.parm1 = v.parm2 = 0;
      } else {
        v.effect = kVisual[eff];
        v.effectRaw = (v.effect << 8) | dat;
        v.parm1 = dat;
        if (v.effect == 0 || v.effect == 4) {
          v.parm1 = dat >> 3;
          v.parm2 = dat & 7;
        }
      }
    }
    for (int c = 0; c < voices_; c++) {
      Vars& v = vars_[c];
      Channel& ch = ch_[c];
      if (v.note == kNoteEnd) {
        ch.active = false;
      } else if (v.note >= 1 && v.note <= kMaxNote && v.effect != 3) {
        ch.active = true;
        ch.noteInterval = v.note * kIBN;
        ch.frequency = hz(ch.noteInterval);
        v.lastNote = v.note;
        v.vibIndex = 0;
      }
    }
    bool manualBreak = false, manualJump = false;
    for (int c = 0; c < voices_; c++) {
      Vars& v = vars_[c];
      if (v.effectRaw == 0) continue;
      switch (v.effect) {
        case 0x3:
          if (v.note >= 1 && v.note <= kMaxNote) v.portaStopHz = hz(v.note * kIBN);
          if (v.parm1) v.portaDelta = v.parm1;
          break;
        case 0x4:
          if (v.parm1) v.vibSpeed = v.parm1;
          if (v.parm2) v.vibDepth = v.parm2;
          v.vibIndex = (v.vibIndex + v.vibSpeed) & 31;
          break;
        case 0xB:
          order_ = v.parm1;
          row_ = -1;
          manualJump = true;
          pattern_ = orders_[order_];
          break;
        case 0xD:
          row_ = v.parm1 - 1;
          if (row_ >= kMaxRows) row_ = -1;
          if (!manualBreak && !manualJump) order_++;
          if (order_ > 255) order_ = 0;          // the tracker's byte wraps
          manualBreak = true;
          if (orders_[order_] != 0xFF) pattern_ = orders_[order_];
          break;
        case 0xF:
          tempo_ = v.parm1;
          break;
      }
    }
  }

  void effectTick() {
    const int minHz = 20, maxHz = 65535 - 63;
    for (int c = 0; c < voices_; c++) {
      Vars& v = vars_[c];
      Channel& ch = ch_[c];
      if (v.effectRaw == 0) continue;
      switch (v.effect) {
        case 0x0:
          switch (tickCount_ % 3) {
            case 0: ch.noteInterval = v.lastNote * kIBN; break;
            case 1: ch.noteInterval = (v.lastNote + v.parm1) * kIBN; break;
            case 2: ch.noteInterval = (v.lastNote + v.parm2) * kIBN; break;
          }
          ch.frequency = hz(ch.noteInterval);
          break;
        case 0x1: {
          const int f = ch.frequency + v.parm1;
          ch.frequency = f > maxHz ? maxHz : f;
          break;
        }
        case 0x2: {
          const int f = ch.frequency - v.parm1;
          ch.frequency = f < minHz ? minHz : f;
          break;
        }
        case 0x3:
          if (ch.frequency < v.portaStopHz) {
            ch.frequency += v.portaDelta;
            if (ch.frequency > v.portaStopHz) ch.frequency = v.portaStopHz;
          }
          if (ch.frequency > v.portaStopHz) {
            ch.frequency -= v.portaDelta;
            if (ch.frequency < v.portaStopHz) ch.frequency = v.portaStopHz;
          }
          break;
        case 0x4:
          // Pascal's div truncates toward zero, as C's does
          ch.frequency = hz(ch.noteInterval + (vibTable_[v.vibIndex] * v.vibDepth) / 64);
          v.vibIndex = (v.vibIndex + v.vibSpeed) & 31;
          break;
      }
    }
  }

  // TPCSpeakerSimple.updateAllChannels, equalShare off
  void speaker() {
    if (ch_[dominant_].active) {
      lastFreq_ = ch_[dominant_].frequency;
    } else {
      bool any = false;
      for (int c = 0; c < voices_; c++) any = any || ch_[c].active;
      if (!any) lastFreq_ = kSilent;
    }
    dominant_++;
    if (dominant_ > voices_ - 1 || tickCount_ == 0) dominant_ = 0;
  }

  uint8_t orders_[256] = {};
  std::vector<uint8_t> data_;
  uint16_t notesHz_[kMaxNote * kIBN + 1] = {};
  int8_t vibTable_[32] = {};
  int voices_ = 1;
  Channel ch_[12];
  Vars vars_[12];
  int tempo_ = 4, order_ = 0, pattern_ = 0, row_ = 0;
  uint8_t tickCount_ = 0;
  bool playing_ = false, started_ = false;
  int dominant_ = 0;
  uint16_t lastFreq_ = kSilent;
  std::set<int> visited_;
};

// The speaker: the timer's square wave, averaged over each sample's time. As on the
// timer chip, a new divisor takes over at the end of the half wave under way.
class Speaker {
 public:
  explicit Speaker(int rate) : rate_(rate) {}
  void set(uint16_t freq) {
    if (freq == kSilent || freq <= 18) {
      on_ = false;
      return;
    }
    const int divisor = kPitFreq / freq;
    newHalf_ = divisor * 0.5 * rate_ / kPitClock;
    if (!on_ && half_ <= 0) {
      half_ = newHalf_;
      left_ = half_;
    }
    on_ = true;
  }
  double sample() {
    if (!on_) return 0;
    double sum = 0, time = 1.0;
    while (time > 0) {
      if (left_ > time) {
        sum += high_ ? time : -time;
        left_ -= time;
        break;
      }
      sum += high_ ? left_ : -left_;
      time -= left_;
      high_ = !high_;
      half_ = newHalf_;
      left_ = half_ > 0.01 ? half_ : 0.01;
    }
    return sum * kLevel;
  }

 private:
  int rate_;
  bool on_ = false, high_ = true;
  double half_ = 0, newHalf_ = 0, left_ = 0;
};

const double kTickRate = kPitClock / (kPitFreq / 60);     // the timer interrupt: 60.0 Hz

}  // namespace

int playMonotone(const char* path, int track, int rate, long long startMs) {
  (void)track;
  const std::vector<uint8_t> file = readFile(path);
  Monotone song;
  std::string why;
  if (!song.load(file, why)) return fail(why);

  // BBMP_MONOTONE_DUMP=<file> writes what the tracker's disk writer (Ctrl+W) writes:
  // the timer divisor sent to the speaker, a 16-bit word for every 1/60 s.
  const char* dumpTo = std::getenv("BBMP_MONOTONE_DUMP");
  FILE* dump = dumpTo ? std::fopen(dumpTo, "wb") : nullptr;
  song.start();
  long long ticks = 0;
  while (song.tick() && ticks < 60LL * 3600) {
    ticks++;
    if (dump) {
      const uint16_t d = static_cast<uint16_t>(kPitFreq / song.frequency());
      std::fwrite(&d, 2, 1, dump);
    }
  }
  if (dump) std::fclose(dump);
  if (ticks == 0) return fail("the MONOTONE song has no notes to play");

  Info info;
  info.type = "MONOTONE (PC speaker)";
  info.title = song.title_;
  info.comment = song.comment_;
  info.durationMs = static_cast<long long>(ticks * 1000.0 / kTickRate);
  info.facts.emplace_back("tracks", std::to_string(song.tracks_) +
                                        (song.tracks_ > kSpeakerVoices ? " (the PC speaker plays the first 4)" : ""));
  info.facts.emplace_back("patterns", std::to_string(song.patterns_));

  song.start();
  Speaker spk(rate);
  const double perTick = rate / kTickRate;
  double due = 0;
  long long skip = startMs * rate / 1000;
  std::vector<int16_t> out(2 * 4096);
  size_t n = 0;
  bool headerSent = false;
  bool more = true;
  while (more) {
    if (due < 1.0) {
      if (!song.tick()) {
        more = false;
        break;
      }
      spk.set(song.frequency());
      due += perTick;
    }
    while (due >= 1.0) {
      const int16_t s = clip16(spk.sample());
      due -= 1.0;
      if (skip > 0) {
        skip--;
        continue;
      }
      if (!headerSent) {
        header(info);
        headerSent = true;
      }
      out[2 * n] = out[2 * n + 1] = s;
      if (++n == 4096) {
        if (!emit(out.data(), n)) return 0;
        n = 0;
      }
    }
  }
  if (!headerSent) header(info);
  if (n) emit(out.data(), n);
  return 0;
}

}  // namespace pcmusic
