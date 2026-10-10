// SPDX-License-Identifier: GPL-3.0-or-later
// Sound Images Generation 2 (.sig): the Ad Lib music of the DOS Dizzy games (Codemasters,
// 1993), for Sound Images' second sound driver (Tony Williams). Written for this player in
// a clean room, from a description of the format only.
//
// A file holds several songs. A song is a tempo, a multiplier and up to 16 tracks; a track
// is a list of MIDI-like events (notes with velocities, note offs, instruments, bends,
// volumes, a drum mode that picks the instrument by note), each after a delay. The game
// loaded the file right behind the driver, so its pointers count from the start of the
// driver's segment: here they are addresses, and a byte outside the file reads as 0.
//
// Every write is made, also one that would not change its register: the chip emulator takes
// buffered writes one after the other, so the number of writes is part of the sound. The
// test mode (BBMP_ADLIB_LOG, common.h) lists them all.

#include "common.h"
#include "sig_cr_tables.h"

namespace adlib {

namespace {

using sigcr::kLevel;
using sigcr::kPitch;

constexpr double kSigHz = 1193182.0 / 23789;   // the PC timer, divided as the game sets it
constexpr long long kLoad = 0x10de;            // where the file lay in the driver's segment
constexpr long long kSongs = kLoad + 4;        // what song positions count from
constexpr int kMostEvents = 100000;            // read at once by a track before it gives up
// Events read in the whole song before it is taken as damaged and ended. The real songs read
// at most about 6,000; a damaged file can make a track read thousands every tick.
constexpr long long kSongEvents = 500000;

// Operator offsets by voice; the voices past 8 are not on the chip and land elsewhere.
constexpr uint8_t kModOp[16] = {0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11,
                                0x12, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x88};
constexpr uint8_t kCarOp[16] = {0x03, 0x04, 0x05, 0x0b, 0x0c, 0x0d, 0x13, 0x14,
                                0x15, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88};

class Sig : public Song {
 public:
  Sig(const std::vector<uint8_t>& d, int song, Copl& opl, OplLog* log) : d_(d), opl_(opl), log_(log) {
    opl.init();
    out(0x01, 0x20);
    out(0xbd, 0x00);
    out(0x08, 0x00);
    drums_ = word(kLoad + 2);
    for (int t = 0; t < 16; t++) tr_[t].voice = t;
    if (song < 1 || song > byte(kSongs)) {
      error_ = true;
      return;
    }
    const long long h = kSongs + word(kSongs + word(kSongs + 1) + 2 * (song - 1));
    tempo_ = byte(h);
    multiplier_ = byte(h + 1);
    tracks_ = byte(h + 2) > 16 ? 16 : byte(h + 2);
    for (int t = 0; t < tracks_; t++) {
      Track& k = tr_[t];
      k.start = kSongs + word(h + 3 + 2 * t);
      k.pos = k.start;
      k.time = delay(k.pos);
      k.active = true;
    }
    speed();
  }

  // What a damaged file is refused for. Reads outside the file stay safe by themselves.
  static bool ok(const std::vector<uint8_t>& d) {
    const size_t size = d.size();
    auto w = [&](size_t i) -> size_t { return i + 1 < size ? d[i] | d[i + 1] << 8 : 0xffff; };
    if (size < 16) return false;
    if (w(0) + 1 != w(2) || w(2) < kSongs || w(2) - kLoad + 0x90 > size) return false;
    const size_t n = d[4];
    if (n < 1 || n > 64) return false;
    const size_t p = w(5) + 4;
    if (p + 2 * n > size) return false;
    for (size_t s = 0; s < n; s++) {
      const size_t q = w(p + 2 * s) + 4;
      if (q + 3 > size) return false;
      const size_t c = d[q + 2];
      if (c > 16 || q + 3 + 2 * c > size) return false;
      for (size_t t = 0; t < c; t++)
        if (w(q + 3 + 2 * t) + 4 >= size) return false;
    }
    return true;
  }

  double hz() const override { return kSigHz; }
  long long events() const { return events_; }

  bool tick() override {
    if (log_) log_->tickStart();
    const uint32_t g = acc_ + fraction_;
    const uint32_t elapsed = (whole_ + (g >> 16)) & 0xffff;
    acc_ = g & 0xffff;
    for (int t = 0; t < tracks_; t++) track(t, elapsed);
    bool goesOn = false;
    if (!error_ && events_ <= kSongEvents)
      for (int t = 0; t < 16; t++)
        if (tr_[t].active && !tr_[t].round) goesOn = true;
    if (log_) log_->tickEnd(goesOn);
    return goesOn;
  }

 private:
  struct Track {
    int voice = 0;
    uint8_t mod = 0, car = 0;
    long long pos = 0, start = 0;
    uint32_t time = 0;
    uint8_t note = 0, key = 0, instrument = 0xff, volume = 0x7f, drums = 0, bend = 0x40;
    long long instr = 0;   // the instrument's address (0 until one is chosen)
    bool active = false, round = false;
  };

  uint8_t byte(long long a) const {
    const long long i = a - kLoad;
    return i >= 0 && i < static_cast<long long>(d_.size()) ? d_[i] : 0;
  }
  long long word(long long a) const { return byte(a) | byte(a + 1) << 8; }

  void out(int reg, int val) {
    opl_.write(reg & 0xff, val & 0xff);
    if (log_) log_->write(reg, val);
  }

  // A variable-length number: seven bits a byte while bit 7 is set, at most five bytes.
  uint32_t delay(long long& pos) const {
    uint32_t b = byte(pos++);
    if (b < 0x80) return b;
    uint32_t v = 0;
    for (int n = 0; n < 4; n++) {
      v = (v + (b & 0x7f)) * 128;
      b = byte(pos++);
      if (b < 0x80) return v + b;
    }
    return 0xffffffffu;   // a fifth byte with bit 7 set: skipped, and the longest wait
  }

  void speed() {
    constexpr uint32_t k = 71590800 / 23789;
    const uint32_t m = static_cast<uint32_t>(tempo_) * multiplier_;
    const uint32_t q = m / k, r = m % k;
    whole_ = q & 0xffff;
    fraction_ = ((static_cast<uint64_t>(r) * 65536 + (q & 0xffff)) / k) & 0xffff;
  }

  void keyOff(Track& k) {
    if (!(k.key & 0x20)) return;
    k.key &= ~0x20;
    out(0xb0 + k.voice, k.key);
  }

  // The block of a high semitone: read on from the byte view of the pitch table.
  static uint8_t pitchByte(int i) {
    const uint16_t v = kPitch[i / 2];
    return i & 1 ? v >> 8 : v & 0xff;
  }

  void sound(Track& k, uint8_t x) {
    const int bend = k.bend >= 64 ? (k.bend - 64) * 2 : -((64 - k.bend) * 2);
    const uint32_t p = (64u * x + bend) & 0xffff;
    const uint32_t step = ((p + 768) & 0xffff) % 768;
    const uint32_t semi = (p >> 6) & 0xff;
    uint32_t block;
    if (semi < 7) block = 0;
    else if (semi < 12) block = 0xfc;
    else if (semi < 101) block = (semi / 12 - 1) * 4;
    else block = pitchByte(semi - 101);
    uint32_t f = kPitch[step];
    if (f & 0x8000) {
      f &= 0x1fff;
      block = (block + 4) & 0xff;
    }
    if (semi < 7) f >>= 1;
    const uint32_t w = ((f & 0x1fff) | block << 8) & 0xffff;
    out(0xa0 + k.voice, w & 0xff);
    k.key = static_cast<uint8_t>((w >> 8) | 0x20);
    out(0xb0 + k.voice, k.key);
  }

  uint8_t ins(const Track& k, int i) const { return byte(k.instr + i); }

  void load(Track& k) {
    out(0x60 + k.mod, ins(k, 0));
    out(0x60 + k.car, ins(k, 1));
    out(0x80 + k.mod, ins(k, 2));
    out(0x80 + k.car, ins(k, 3));
    out(0xe0 + k.mod, ins(k, 6));
    out(0xe0 + k.car, ins(k, 7));
    out(0xc0 + k.voice, ins(k, 9));
    out(0x20 + k.mod, ins(k, 4));
    out(0x20 + k.car, ins(k, 5));
  }

  void choose(Track& k, uint8_t i) {
    k.instrument = i;
    k.instr = drums_ + 128 + 16 * i;
    load(k);
  }

  void noteOn(Track& k, uint8_t e) {
    if (k.drums) {
      const uint8_t i = byte(drums_ + e);
      if (i != k.instrument) choose(k, i);
      k.note = ins(k, 8);
      sound(k, k.note);
    } else {
      sound(k, (e - 24 + ins(k, 8)) & 0xff);
    }
  }

  static uint8_t table(uint32_t i) { return i <= 128 ? kLevel[i] : 0; }

  static uint8_t level(uint8_t a, uint8_t v, uint8_t c) {
    const uint32_t x = static_cast<uint32_t>((a + 1) & 0xff) * ((v + 1) & 0xff) / 2;
    const uint32_t y = (8 * x / 2) & 0xffff;
    const uint32_t z = y * (2 * ((c + 1) & 0xff));
    return table((z >> 16) & 0xff);
  }

  void levels(Track& k, uint8_t v) {
    const uint8_t scaling = ins(k, 12);
    out(0x40 + k.car, level(ins(k, 11), v, k.volume) | (scaling & 3) * 64);
    const uint8_t m = (ins(k, 9) & 1) ? level(ins(k, 10), v, k.volume) : table(ins(k, 10));
    out(0x40 + k.mod, m | ((scaling >> 4) & 3) * 64);
  }

  void track(int t, uint32_t elapsed) {
    Track& k = tr_[t];
    if (!k.active || events_ > kSongEvents) return;
    const uint32_t old = k.time;
    k.time = old - elapsed;
    if (old >= elapsed) return;
    long long p = k.pos;
    for (int n = 0; n < kMostEvents; n++) {
      events_++;
      const uint8_t e = byte(p++);
      if (e < 0x80) {   // a note and its velocity
        k.note = e;
        keyOff(k);
        noteOn(k, e);
        levels(k, byte(p++));
      } else if (e < 0x90) {   // the track moves to another voice
        k.voice = e & 15;
        k.mod = kModOp[k.voice];
        k.car = kCarOp[k.voice];
      } else if (e == 0x90) {
        if (byte(p++) == k.note) keyOff(k);
      } else if (e == 0x91) {   // the track ends
        k.active = false;
        keyOff(k);
        return;
      } else if (e == 0x92) {
        k.volume = 0x7f;
        k.bend = 0x40;
        choose(k, byte(p++));
      } else if (e == 0x93) {
        multiplier_ = byte(p++);
        speed();
      } else if (e == 0x95) {
        k.bend = byte(p++);
        if (k.key & 0x20) sound(k, (k.note + ins(k, 8) - 24) & 0xff);
      } else if (e == 0x96) {
        k.volume = byte(p++);
      } else if (e == 0x97) {
        k.drums = byte(p++);
      } else if (e == 0x98) {   // back to the track's start
        p = k.start;
        k.round = true;
      } else if (e == 0x99) {
        keyOff(k);
      } else if (e == 0x9a) {   // the track stops where it is
        k.round = true;
        k.active = false;
        return;
      } else if (e >= 0x9b && e <= 0xfe) {   // not an event: the track stops
        k.active = false;
        keyOff(k);
        return;
      }
      // ($94 and $FF do nothing)
      k.time += delay(p);
      if (!(k.time & 0x80000000u)) break;
    }
    k.pos = p;
  }

  const std::vector<uint8_t>& d_;
  Copl& opl_;
  OplLog* log_;
  long long drums_ = 0;
  uint8_t tempo_ = 0, multiplier_ = 0;
  int tracks_ = 0;
  uint32_t whole_ = 0, fraction_ = 0, acc_ = 0;
  bool error_ = false;
  long long events_ = 0;
  Track tr_[16];
};

// The ticks a song lasts, up to and with the one that ends it (at most 20 minutes).
long long length(const std::vector<uint8_t>& d, int song) {
  Chip probe(8000);
  Sig p(d, song, probe, nullptr);
  const long long most = static_cast<long long>(1200 * kSigHz);
  long long n = 1;
  while (p.tick() && ++n < most) {}
  return n;
}

}  // namespace

int playSigCr(const char* path, int track, int rate, long long startMs) {
  const std::vector<uint8_t> d = readFile(path);
  if (!Sig::ok(d)) return fail("not a Sound Images Generation 2 song");
  // the songs that last more than two ticks are the tracks; the rest are silent stubs
  const int songs = d[4];
  std::vector<std::pair<int, long long>> offered;
  for (int s = 1; s <= songs; s++) {
    const long long n = length(d, s);
    if (n > 2) offered.emplace_back(s, n);
  }
  if (offered.empty()) return fail("the file holds no songs");
  if (track < 0 || track >= static_cast<int>(offered.size())) track = 0;
  const int song = offered[track].first;
  const long long ticks = offered[track].second;
  Info info;
  info.tracks = static_cast<int>(offered.size());
  info.track = track;
  info.title = baseName(path);
  info.type = "Sound Images Generation 2";
  info.facts.emplace_back("driver", "Sound Images Generation 2 (Tony Williams, 1993); played by a replayer written for this player");
  info.facts.emplace_back("song", std::to_string(song) + " of " + std::to_string(songs));
  info.durationMs = static_cast<long long>(ticks * 1000 / kSigHz);
  OplLog log("sig", path, track, info, ticks, kSigHz, rate);
  Chip opl(rate);
  Sig s(d, song, opl, log.on() ? &log : nullptr);
  return play(s, opl, info, rate, startMs);
}

}  // namespace adlib
