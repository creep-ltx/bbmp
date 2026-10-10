// SPDX-License-Identifier: GPL-3.0-or-later
// Westwood SND: the PC speaker version of Westwood's sound files (Eye of the
// Beholder 1 and 2, Legend of Kyrandia). The same games' .ADL files are for the AdLib.
//
// A file is a table of 120 track numbers (one byte each, the number of a program),
// a table of 16-bit program offsets (150, 250, 300 or 500 of them, counted from the end
// of the track table), and the programs: byte code for one square wave voice.
//
//   00..7f dd     note (index into a table of timer periods, 0 = rest), lasts
//                 scale * (1 + dd) ticks
//   e2 nn         scale = nn (0 counts as 1)
//   e9 nn, e8 nn  scale += nn, scale -= nn
//   e6 ffff dd    tone of ffff Hz, lasts scale * (1 + dd) ticks
//   cd nn ... ce oo oo   repeat the part between nn times
//   d1 nn ... d2 oo oo   the same with a counter of its own
//   anything else (ff)   end
//
// The driver runs at 292 ticks per second. Written after the description of the
// format in ScummVM's driver for it (engines/kyra/sound/drivers/pcspeaker_v1.cpp,
// GPL 3 or later); the note periods are the table of the original driver.

#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>

#include "engine.h"

using namespace retro;

namespace {

const int kTickRate = 292;
const int kClock = 1193180;         // the PC's timer chip, which makes the speaker's tone
const int kLevel = 7000;
const long long kLongest = 600;     // seconds

const uint16_t kPeriods[96] = {
  0x0000, 0xfdff, 0xefa2, 0xe241, 0xd582, 0xc998, 0xbe3d, 0xb38a,
  0xa97c, 0x9ff2, 0x96fc, 0x8e89, 0x8683, 0x7ef7, 0x77d9, 0x7121,
  0x6ac7, 0x64c6, 0x5f1f, 0x59ca, 0x54be, 0x4ffd, 0x4b7e, 0x4742,
  0x4342, 0x3f7b, 0x3bdb, 0x388f, 0x3562, 0x3263, 0x2f8f, 0x2ce4,
  0x2a5f, 0x27fe, 0x25c0, 0x23a1, 0x21a1, 0x1fbe, 0x1df6, 0x1c48,
  0x1ab1, 0x1932, 0x17c8, 0x1672, 0x1530, 0x13ff, 0x12e0, 0x11d1,
  0x10d1, 0x0fdf, 0x0efb, 0x0e24, 0x0d59, 0x0c99, 0x0be4, 0x0b39,
  0x0a98, 0x0a00, 0x0970, 0x08e8, 0x0868, 0x07f0, 0x077e, 0x0712,
  0x06ac, 0x064c, 0x05f2, 0x059d, 0x054c, 0x0500, 0x04b8, 0x0474,
  0x0434, 0x03f8, 0x03bf, 0x0382, 0x0356, 0x0326, 0x02f9, 0x02ce,
  0x02a6, 0x0280, 0x025c, 0x023a, 0x021a, 0x01fc, 0x01df, 0x01c4,
  0x01ab, 0x0193, 0x017c, 0x0167, 0x0153, 0x0140, 0x012e, 0x011d
};

// The byte code, one step at a time: either a tone (or rest) and how long it lasts,
// or the end.
struct Program {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t pos = 0;
  int scale = 1;
  uint8_t repeat1 = 0, repeat2 = 0;
  bool bad = false;         // ran into the end of the file or a jump out of it

  // False at the end. <period> 0 is a rest.
  bool step(int& period, int& ticks) {
    // a program without notes can only loop so often: the counters are bytes
    for (int guard = 0; guard < 100000; guard++) {
      if (pos >= size) return fail();
      const uint8_t cmd = data[pos++];
      if (cmd < 0x80) {
        if (pos >= size) return fail();
        period = kPeriods[cmd > 95 ? 0 : cmd];
        ticks = length(data[pos++]);
        return true;
      }
      switch (cmd) {
        case 0xe9:
          if (pos >= size) return fail();
          scale += data[pos++];
          if (scale > 255) scale = 255;
          break;
        case 0xe8:
          if (pos >= size) return fail();
          scale -= data[pos++];
          if (scale < 1) scale = 1;
          break;
        case 0xe6: {
          if (pos + 3 > size) return fail();
          const int hz = le16(data + pos);
          period = hz ? (kClock / hz) & 0xffff : 0;
          ticks = length(data[pos + 2]);
          pos += 3;
          return true;
        }
        case 0xe2:
          if (pos >= size) return fail();
          scale = data[pos++];
          if (!scale) scale = 1;
          break;
        case 0xd1:
        case 0xcd: {
          if (pos >= size) return fail();
          uint8_t& counter = cmd == 0xcd ? repeat1 : repeat2;
          counter = data[pos++];
          if (!counter) counter = 1;
          break;
        }
        case 0xd2:
        case 0xce: {
          if (pos + 2 > size) return fail();
          uint8_t& counter = cmd == 0xce ? repeat1 : repeat2;
          if (--counter) {
            const size_t back = le16(data + pos);
            if (back > pos) return fail();
            pos -= back;
          } else {
            pos += 2;
          }
          break;
        }
        default:
          return false;
      }
    }
    return fail();
  }

 private:
  bool fail() {
    bad = true;
    return false;
  }
  // the driver counts in 16 bits
  int length(int byte) const {
    const int n = static_cast<int16_t>(scale * (1 + byte));
    return n < 1 ? 1 : n;
  }
};

class WestwoodSnd : public Engine {
 public:
  bool open(const std::vector<uint8_t>& file, const std::string&, int track, int rate,
            SongInfo& info, std::string& error) override {
    if (file.size() < 120 + 4) return refuse(error);
    const uint8_t* body = file.data() + 120;
    const size_t size = file.size() - 120;
    // The offset table ends where the first program starts, and that is program 0.
    const size_t table = le16(body);
    if (table != 300 && table != 500 && table != 600 && table != 1000) return refuse(error);
    if (table + 1 > size) return refuse(error);
    const size_t count = table / 2;
    for (size_t i = 0; i < count; i++) {
      const size_t at = le16(body + 2 * i);
      if (at && (at < table || at >= size)) return refuse(error);
    }
    // The tracks: the programs the track table names, each once, which exist and
    // make a sound. Every one has to be sound byte code to its end, or the file is
    // something else (the AdLib files have the same tables).
    std::vector<int> programs, entries;
    bool alien = false;
    auto consider = [&](int id, int entry) {
      bool seen = false;
      for (int p : programs) seen = seen || p == id;
      if (seen || static_cast<size_t>(id) >= count) return;
      const size_t at = le16(body + 2 * id);
      if (!at) return;
      Program p;
      p.data = body;
      p.size = size;
      p.pos = at;
      long long ticks = 0, limit = 0;
      bool sounds = false;
      int period, length;
      // Some effects never end: they jump back to where their counter is set. Such a
      // program comes back to a state it has been in; it is played once around.
      std::map<std::tuple<size_t, int, int, int>, long long> seen_at;
      for (;;) {
        const auto state = std::make_tuple(p.pos, p.scale, int(p.repeat1), int(p.repeat2));
        const auto before = seen_at.find(state);
        if (before != seen_at.end()) {
          limit = ticks;              // once around, as every song (ending.py)
          break;
        }
        seen_at[state] = ticks;
        if (!p.step(period, length)) break;
        ticks += length;
        sounds = sounds || period;
        // Repeats inside repeats count up to 255 x 255 rounds, each through new states:
        // a program that has played for ten minutes is played that long (the longest
        // sound of the games is 18 seconds).
        if (ticks > kLongest * kTickRate) {
          limit = ticks;
          break;
        }
      }
      if (p.bad || (!limit && body[p.pos - 1] != 0xff)) {
        if (debug_on())
          fprintf(stderr, "program %d at %zu: bad %d, pos %zu\n", id, at, p.bad, p.pos);
        alien = true;
        return;
      }
      if (limit) ticks = limit;
      if (!sounds) return;
      programs.push_back(id);
      entries.push_back(entry);
      if (static_cast<int>(programs.size()) - 1 == track) {
        ticks_ = ticks;
        endless_ = limit != 0;
        start_ = at;
      }
    };
    for (int i = 0; i < 120 && !alien; i++) consider(file[i] & 0x7f, i);
    // Some files have an empty track table (the game knew the numbers): then the
    // programs themselves are the tracks.
    if (programs.empty())
      for (size_t i = 0; i < count && i < 0x80 && !alien; i++) consider(static_cast<int>(i), -1);
    if (alien) return refuse(error);
    if (programs.empty()) {
      error = "no tracks in this file";
      return false;
    }
    info.tracks = static_cast<int>(programs.size());
    info.type = "Westwood SND (PC speaker)";
    if (track >= info.tracks) return true;      // the main program says so

    rate_ = rate;
    program_.data = body;
    program_.size = size;
    program_.pos = start_;
    info.comment = "program " + std::to_string(programs[track]);
    if (entries[track] >= 0)
      info.comment = "entry " + std::to_string(entries[track]) + " of the track table, " + info.comment;
    if (endless_) info.comment += "; repeats without end, played twice";
    // A short rest after the sound, and effects of a fraction of a second get a
    // second in all, so that one can tell them apart when played in a row.
    const long long sound = ticks_ * rate / kTickRate;
    tail_ = rate / 4;
    if (sound + tail_ < rate) tail_ = rate - sound;
    info.duration_ms = (sound + tail_) * 1000 / rate;
    return true;
  }

  size_t render(int16_t* out, size_t frames) override {
    size_t n = 0;
    for (; n < frames; n++) {
      if (samplesLeft_ <= 0 && !ended_) tick();
      if (ended_) {
        if (tail_ <= 0) break;
        tail_--;
      } else {
        samplesLeft_ -= 1.0;
      }
      const int16_t s = static_cast<int16_t>(sample());
      out[2 * n] = out[2 * n + 1] = s;
    }
    return n;
  }

 private:
  static bool refuse(std::string& error) {
    error = "not a Westwood PC speaker file";
    return false;
  }

  // One tick of the driver.
  void tick() {
    samplesLeft_ += static_cast<double>(rate_) / kTickRate;
    const bool over = endless_ && played_ >= ticks_;
    played_++;
    if (--timer_ > 0 && !over) return;
    int period, length;
    if (over || !program_.step(period, length)) {
      ended_ = true;
      on_ = false;
      return;
    }
    timer_ = length;
    on_ = period != 0;
    if (on_) newHalf_ = period * 0.5 * rate_ / kClock;    // half a wave, in samples
    if (on_ && half_ <= 0) {
      half_ = newHalf_;
      left_ = half_;
    }
  }

  // The speaker's level over the next sample: the square wave, averaged over the
  // sample's time so that high tones don't alias too badly. As on the real timer
  // chip a new period begins with the next half wave, and the wave keeps its place
  // while the speaker is off.
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

  Program program_;
  int rate_ = 48000;
  size_t start_ = 0;
  long long ticks_ = 0, tail_ = 0, played_ = 0;
  bool endless_ = false;
  int timer_ = 0;
  double samplesLeft_ = 0;
  bool ended_ = false, on_ = false, high_ = true;
  double half_ = 0, newHalf_ = 0, left_ = 0;
};

}  // namespace

REGISTER_ENGINE("wwsnd", WestwoodSnd)
