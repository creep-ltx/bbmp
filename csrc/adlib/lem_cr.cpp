// SPDX-License-Identifier: GPL-3.0-or-later
// Sound Interface System (.lem): the Ad Lib music of Xmas Lemmings, for the driver "AdLib
// Music by Sound Images (c) 1991" that Lemmings carries. Written for this player in a clean
// room, from a description of the format only.
//
// A file is one song cut out of the driver's song area: a header, one order list per track,
// patterns of single bytes (notes, rests, holds, lengths, instruments) and 16-byte
// instruments. Track t plays on OPL voice t. Positions count from the start of that song
// area, not of the file, so the file's place in it is worked out from the first order list,
// which follows the header.
//
// The voices move once a row (every `speed` ticks); between rows nothing is written. The song
// ends when it says so, or when every track has gone round its order list once.
//
// The test mode (BBMP_ADLIB_LOG, common.h) lists every chip write as it is made.

#include "common.h"

namespace adlib {

namespace {

constexpr double kLemHz = 70.086;   // once a VGA frame

constexpr uint8_t kModOp[9] = {0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12};
constexpr uint8_t kCarOp[9] = {0x03, 0x04, 0x05, 0x0b, 0x0c, 0x0d, 0x13, 0x14, 0x15};
// C to B; the last five are one block up, written as negative numbers
constexpr int kFnum[12] = {690, 731, 774, 820, 869, 921, 975, -507, -477, -444, -409, -373};
// a pattern of 128 rows of rest that the song area shares and one file leaves out
constexpr long long kSharedRest = 0xb34;
constexpr uint8_t kSharedRestBytes[6] = {0xff, 0x81, 0x81, 0x81, 0x81, 0x80};
constexpr int kMostBytes = 4096;   // read at once by a voice before it gives up
// Pattern bytes read in the whole song before it is taken as damaged and ended. The real
// songs read at most about 2,000; a damaged file can make every voice read 4,096 a row.
constexpr long long kSongBytes = 1000000;

class Lem : public Song {
 public:
  Lem(const std::vector<uint8_t>& d, Copl& opl, OplLog* log) : d_(d), opl_(opl), log_(log) {
    opl.init();
    base_ = start(d);
    tracks_ = hdr(5);
    speed_ = hdr(4);
    instruments_ = hdr(2) | hdr(3) << 8;
    for (int t = 0; t < tracks_; t++) {
      Voice& v = v_[t];
      const long long o = hdr(6 + 2 * t) | hdr(7 + 2 * t) << 8;
      v.pattern = word(o);
      v.order = o + 2;
      v.wait = 1;
      v.active = true;
    }
    for (int c = 0; c < 9; c++) out(0xb0 + c, 0);
    out(0x01, 0x20);
    out(0xbd, 0xc0);
    out(0x08, 0x00);
    row_ = 1;
  }

  // Where the file starts in the song area: the first order list follows the header.
  static long long start(const std::vector<uint8_t>& d) {
    return 6 + 2 * static_cast<long long>(d[5]) - (d[6] | d[7] << 8);
  }

  // What a damaged file is refused for. Reads stay safe by themselves.
  static bool ok(const std::vector<uint8_t>& d) {
    const size_t size = d.size();
    if (size < 16 || d[0] != 0x1c || d[1] != 0x52) return false;
    const int n = d[5];
    if (n < 1 || n > 9 || d[4] == 0) return false;
    const long long base = start(d);
    auto within = [&](long long p, long long room) {
      return static_cast<uint64_t>(p + base) + static_cast<uint64_t>(room) <= size;
    };
    if (!within(d[2] | d[3] << 8, 16)) return false;
    for (int t = 0; t < n; t++) {
      // past the file's end the header reads as the song area does
      auto at = [&](long long i) {
        if (i < static_cast<long long>(size)) return static_cast<long long>(d[i]);
        const long long p = i - base;
        return p >= kSharedRest && p < kSharedRest + 6 ? kSharedRestBytes[p - kSharedRest] : 0ll;
      };
      if (!within(at(6 + 2 * t) | at(7 + 2 * t) << 8, 2)) return false;
    }
    return true;
  }

  double hz() const override { return kLemHz; }
  long long bytes() const { return bytes_; }

  bool tick() override {
    if (log_) log_->tickStart();
    row_ = (row_ - 1) & 0xff;
    for (int c = 0; c < 9; c++) voice(c);
    if (row_ == 0) row_ = speed_;
    bool goesOn = false;
    if (!ended_ && bytes_ <= kSongBytes)
      for (int t = 0; t < tracks_; t++)
        if (v_[t].active && !v_[t].round) goesOn = true;
    if (log_) log_->tickEnd(goesOn);
    return goesOn;
  }

 private:
  struct Voice {
    uint8_t note = 0, wait = 0, length = 0, transpose = 0, release = 0, key = 0;
    int slide = 0;
    long long order = 0, pattern = 0;
    bool active = false, round = false;
  };

  // A byte of the song area, by position.
  uint8_t byte(long long p) const {
    const long long i = p + base_;
    if (i >= 0 && i < static_cast<long long>(d_.size())) return d_[i];
    if (p >= kSharedRest && p < kSharedRest + 6) return kSharedRestBytes[p - kSharedRest];
    return 0;
  }
  long long word(long long p) const { return byte(p) | byte(p + 1) << 8; }
  // A byte of the header, which is the file's own start.
  uint8_t hdr(long long i) const { return i < static_cast<long long>(d_.size()) ? d_[i] : byte(i - base_); }

  void out(int reg, int val) {
    opl_.write(reg & 0xff, val & 0xff);
    if (log_) log_->write(reg, val);
  }

  void keyOff(int c) { out(0xb0 + c, v_[c].key); }

  void noteOn(int c) {
    Voice& v = v_[c];
    const int i = (v.note + v.transpose + 4) & 0xff;
    int block = i / 12, f = kFnum[i % 12];
    block -= 1;
    if (f < 0) block += 1;
    if (block < 0) {
      block = 0;
      f >>= 1;
    }
    out(0xa0 + c, f & 0xff);
    v.key = static_cast<uint8_t>(((f >> 8) & 3) | (4 * block));
    out(0xb0 + c, v.key | 0x20);
  }

  static int level(int x) { return 63 - ((x & 0x7f) >> 1); }

  void instrument(int c, int k) {
    const long long at = instruments_ + 2 * ((8 * (k - 1)) & 0xff);
    uint8_t b[16];
    for (int i = 0; i < 16; i++) b[i] = byte(at + i);
    const int mod = kModOp[c], car = kCarOp[c];
    out(0x60 + mod, b[0]);
    out(0x60 + car, b[1]);
    out(0x80 + mod, b[2]);
    out(0x80 + car, b[3]);
    out(0xe0 + mod, b[6]);
    out(0xe0 + car, b[7]);
    out(0xc0 + c, b[9]);
    out(0x20 + mod, b[4]);
    out(0x20 + car, b[5]);
    const int scaling = b[12];
    out(0x40 + mod, level(b[10]) | ((scaling >> 4) & 3) * 64);
    out(0x40 + car, level((b[11] + b[10]) & 0xff) | (scaling & 3) * 64);
    v_[c].transpose = b[8];
    v_[c].release = b[14];
  }

  void voice(int c) {
    Voice& v = v_[c];
    if (!v.active || row_ != 0 || bytes_ > kSongBytes) return;
    v.wait = (v.wait - 1) & 0xff;
    if (v.wait != 0) {   // the event goes on
      if (v.slide != 0) {
        v.note = (v.note + v.slide) & 0xff;
        noteOn(c);
      }
      // the read position is already at the next event: a hold keeps the key through it
      if (byte(v.pattern) != 0x82 && v.release == v.wait) {
        keyOff(c);
        v.slide = 0;
      }
      return;
    }
    long long p = v.pattern;
    for (int n = 0; n < kMostBytes; n++) {
      bytes_++;
      const uint8_t e = byte(p++);
      if (e < 0x80) {   // a note
        v.note = e;
        keyOff(c);
        noteOn(c);
        break;
      }
      if (e >= 0xe0) {
        v.length = e - 0xdf;
        continue;
      }
      if (e >= 0xa0) {
        instrument(c, e - 0xa0);
        continue;
      }
      if (e == 0x80) {   // the next pattern of the order list
        const long long w = word(v.order);
        v.order += 2;
        if (w != 0) {
          p = w;
        } else {   // the end of the list: on from its loop point
          v.order = word(v.order);
          p = word(v.order);
          v.order += 2;
          v.round = true;
        }
        continue;
      }
      if (e == 0x81) {   // a rest
        keyOff(c);
        v.slide = 0;
        break;
      }
      if (e == 0x82) break;   // a hold
      if (e == 0x83) {        // the end of the song
        for (int k = 0; k < tracks_; k++) {
          keyOff(k);
          v_[k].active = false;
        }
        ended_ = true;
        return;
      }
      if (e == 0x84) {
        n++;
        bytes_++;
        v.transpose = byte(p++);
        continue;
      }
      if (e == 0x86) {
        v.slide = 1;
        continue;
      }
      if (e == 0x87) {
        v.slide = -1;
        continue;
      }
      // $85 and the unknown ones: this track ends
      keyOff(c);
      v.active = false;
      return;
    }
    v.pattern = p;
    v.wait = v.length;
  }

  const std::vector<uint8_t>& d_;
  Copl& opl_;
  OplLog* log_;
  long long base_ = 0, instruments_ = 0;
  int tracks_ = 0;
  uint8_t speed_ = 1, row_ = 0;
  bool ended_ = false;
  long long bytes_ = 0;
  Voice v_[9];
};

}  // namespace

int playLemCr(const char* path, int track, int rate, long long startMs) {
  (void)track;   // one song per file
  const std::vector<uint8_t> d = readFile(path);
  if (!Lem::ok(d)) return fail("not a Lemmings song");
  long long ticks = 0;
  {
    Chip probe(8000);
    Lem p(d, probe, nullptr);
    const long long most = static_cast<long long>(1200 * kLemHz);
    while (ticks < most && p.tick()) ticks++;
    ticks++;   // the tick that ends the song is played too
  }
  Info info;
  info.title = baseName(path);
  info.type = "Lemmings (Sound Images)";
  info.facts.emplace_back("driver",
                          "AdLib Music by Sound Images, 1991 (Lemmings' ADLIB.DAT); played by a replayer written for this player");
  info.facts.emplace_back("timer", "70.1 Hz (a VGA frame)");
  info.durationMs = static_cast<long long>(ticks * 1000 / kLemHz);
  OplLog log("lem", path, 0, info, ticks, kLemHz, rate);
  Chip opl(rate);
  Lem song(d, opl, log.on() ? &log : nullptr);
  return play(song, opl, info, rate, startMs);
}

}  // namespace adlib
