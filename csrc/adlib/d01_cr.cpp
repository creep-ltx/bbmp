// SPDX-License-Identifier: GPL-3.0-or-later
// EdLib D01 (.d01 with its .s01): songs made with JCH's EdLib for his player 1.03 "with
// samples": nine Ad Lib voices and a tenth track of Sound Blaster samples. Written for this
// player in a clean room, from a description of the format only.
//
// A song has subsongs; a subsong gives each of its ten tracks an order list (patterns,
// transpositions, a jump back) and a volume. A pattern is a list of words: notes with their
// lengths, rests, and commands for the next note (instrument, volume, vibrato, slide). An
// instrument may run a "level pulse": a little table that steps the modulator's level. The
// samples lie in the .s01 beside the song; the tenth track starts one per event, and it is
// mixed into the chip's sound here, the way a Sound Blaster would add it.
//
// All five registers of every playing voice are written every tick, as the references were
// made: the chip emulator takes buffered writes one after the other, so the number of writes
// is part of the sound. The test mode (BBMP_ADLIB_LOG, common.h) lists them all, and each
// sample start as a "P" line.

#include <cmath>

#include "common.h"

namespace adlib {

namespace {

constexpr uint8_t kId[6] = {0x4a, 0x43, 0x48, 0x26, 0x02, 0x66};   // "JCH" and three more
constexpr uint8_t kModOp[9] = {0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12};
constexpr uint8_t kCarOp[9] = {0x03, 0x04, 0x05, 0x0b, 0x0c, 0x0d, 0x13, 0x14, 0x15};
constexpr uint16_t kFnum[12] = {0x157, 0x16b, 0x181, 0x198, 0x1b0, 0x1ca,
                                0x1e5, 0x202, 0x220, 0x241, 0x263, 0x287};
constexpr uint16_t kNoVibrato = 0x1000;   // no vibrato command waiting
constexpr int kMostPasses = 65536;        // order entries looked at in one fetch
constexpr int kMostEvents = 4098;         // this many events read in one pattern end it
// Order entries and pattern events read in the whole song before it is taken as damaged and
// ended. The real songs read at most about 18,000; a damaged file can make a single fetch
// read millions.
constexpr long long kSongWork = 2000000;
// Ticks of dry runs (finding the subsongs' lengths) after which no more subsongs are tried:
// a damaged file may claim 255 subsongs of 20 minutes each. The real songs need 17,000.
constexpr long long kMostDryTicks = 2000000;

bool hasId(const std::vector<uint8_t>& d) {
  if (d.size() < 7) return false;
  for (int i = 0; i < 6; i++)
    if (d[i] != kId[i]) return false;
  return true;
}

int timerRate(const std::vector<uint8_t>& d) { return d[8] ? d[8] : 70; }

class D01 : public Song {
 public:
  D01(const std::vector<uint8_t>& d, const std::vector<uint8_t>* samples, int subsong, int rate, Copl& opl,
      OplLog* log)
      : d_(d), s_(samples), rate_(rate), opl_(opl), log_(log) {
    opl.init();
    hz_ = timerRate(d);
    subsongs_ = word(0x6b);
    patterns_ = word(0x6d);
    instruments_ = word(0x6f);
    pulses_ = word(0x73);
    sampleTable_ = word(0x75);
    for (int c = 0; c < 9; c++) {
      for (int op : {kCarOp[c], kModOp[c]}) {
        out(0x20 + op, 0);
        out(0xe0 + op, 0);
        out(0x40 + op, 0x3f);
        out(0x60 + op, 0xff);
        out(0x80 + op, 0xff);
      }
      out(0xa0 + c, 0);
      out(0xb0 + c, 0);
      out(0xc0 + c, 0);
    }
    out(0x01, 0x20);
    out(0x08, 0x00);
    out(0xbd, 0xc0);
    const long long sub = subsongs_ + 32LL * subsong;
    for (int t = 0; t < 10; t++) {
      const long long q = word(sub + 2 * t);
      if (q == 0 || q == 0xffff) continue;
      Track& k = tr_[t];
      k.speed = static_cast<int8_t>(word(q) & 0xff);
      k.orders = q + 2;
      k.trackVolume = byte(sub + 20 + t) & 0x3f;
      k.active = true;
    }
  }

  double hz() const override { return hz_; }
  long long work() const { return work_; }

  bool tick() override {
    if (log_) log_->tickStart();
    for (int t = 0; t < 10; t++) track(t);
    bool goesOn = false;
    for (int t = 0; t < 9; t++)
      if (tr_[t].active && !tr_[t].round) goesOn = true;
    if (work_ > kSongWork) goesOn = false;
    if (log_) log_->tickEnd(goesOn);
    return goesOn;
  }

  // The sample voice, added to the chip's frames as they are made.
  void mix(short* buf, int frames) {
    for (int f = 0; f < frames && playing_; f++) {
      const long long j = static_cast<long long>(position_);
      if (j >= length_) {
        playing_ = false;
        break;
      }
      const double a = static_cast<double>((*s_)[start_ + j]) - 128;
      const double b = j + 1 < length_ ? static_cast<double>((*s_)[start_ + j + 1]) - 128 : 0.0;
      const double diff = b - a;
      const double frac = position_ - static_cast<double>(j);
      const double prod = diff * frac;
      const double sum = a + prod;
      const double scaled = sum * 256;
      const int v = static_cast<int>(std::round(scaled));
      for (int side = 0; side < 2; side++) {
        int x = buf[2 * f + side] + v;
        x = x < -32768 ? -32768 : x > 32767 ? 32767 : x;
        buf[2 * f + side] = static_cast<short>(x);
      }
      position_ += step_;
    }
  }

 private:
  struct Track {
    bool active = false, round = false;
    int8_t counter = 0, speed = 0, vibCounter = 0;
    int16_t rowsLeft = 0;
    uint16_t fetched = 0, vibCommand = kNoVibrato, frequency = 0, nextFrequency = 0;
    long long orders = 0, orderIndex = 0, patternIndex = 0, pulsePos = 0;
    int transpose = 0, instrument = 0;
    uint8_t volumeCommand = 0, volume = 0, trackVolume = 0, vibDepth = 0, vibSpeed = 0;
    int16_t slideCommand = 0, slide = 0;
    uint8_t key = 0, nextKey = 0, kind = 0, level = 0;
    uint8_t carLevel = 0, modLevel = 0, connection = 0;
    uint8_t pulseOn = 0, pulseSpeed = 0, pulseTimer = 0, pulseSteps = 0;
  };

  uint8_t byte(long long p) const { return p >= 0 && p < static_cast<long long>(d_.size()) ? d_[p] : 0; }
  long long word(long long p) const {
    return p >= 0 && p + 1 < static_cast<long long>(d_.size()) ? d_[p] | d_[p + 1] << 8 : 0xffff;
  }

  void out(int reg, int val) {
    opl_.write(reg & 0xff, val & 0xff);
    if (log_) log_->write(reg, val);
  }

  void track(int t) {
    Track& k = tr_[t];
    if (!k.active) return;
    k.counter = static_cast<int8_t>(k.counter - 1);
    if (k.counter < 0) k.counter = k.speed;
    if (k.counter == 0) {
      k.rowsLeft = static_cast<int16_t>(k.rowsLeft - 1);
      if (k.rowsLeft < 0) startEvent(t);
    } else if (k.counter == 1 && k.rowsLeft == 0) {
      if (!fetch(k)) {   // the end of the track
        k.active = false;
        k.frequency = 0;
        k.key = 0;
        k.counter = 0;
        k.vibDepth = 0;
        if (t < 9) writeVoice(t);
        return;
      }
    }
    if (t < 9) {
      effects(k);
      writeVoice(t);
    }
  }

  // Finds the next event: a note or a rest, after the commands before it.
  bool fetch(Track& k) {
    for (int pass = 0; pass < kMostPasses && work_ <= kSongWork; pass++) {
      work_++;
      int16_t slideSeen = 0;   // a slide given in this pattern before the note
      const long long o = word(k.orders + k.orderIndex);
      if (o == 0xfffe) return false;
      if (o == 0xffff) {
        k.orderIndex = 2 * word(k.orders + k.orderIndex + 2);
        k.round = true;
        continue;
      }
      if (o >= 0x8000 && o <= 0x8fff) {
        k.transpose = (o & 0x0f00) ? -static_cast<int>(o & 0xff) : static_cast<int>(o & 0xff);
        k.orderIndex += 2;
        continue;
      }
      const long long p = word(patterns_ + 2 * o);
      for (int read = 1;; read++) {
        const long long e = word(p + k.patternIndex);
        k.patternIndex += 2;
        work_++;
        if (e == 0xffff || read >= kMostEvents) {   // the pattern's end: on to the next order
          k.patternIndex = 0;
          k.orderIndex += 2;
          break;
        }
        switch (e >> 12) {
          case 0xc: k.instrument = e & 0xfff; continue;
          case 0x9: k.volumeCommand = e & 0x3f; continue;
          case 0x7: k.vibCommand = e & 0xfff; continue;
          case 0xd: k.slideCommand = slideSeen = static_cast<int16_t>(e & 0xfff); continue;
          case 0xe: k.slideCommand = slideSeen = static_cast<int16_t>(-(e & 0xfff)); continue;
          case 0x6:   // a rest
            k.fetched = static_cast<uint16_t>(e);
            k.key = 0;
            return true;
          default: break;
        }
        // a note
        k.fetched = (e >> 8) & 0x1f;
        int n = e & 0x7f;
        if (n == 0x7e || n == 0) {   // no new note, or a key off
          if (n == 0) k.key = 0;
          k.kind = 1;
          return true;
        }
        if (e & 0x2000) {
          k.kind = 2;   // tied: only the pitch changes
        } else {
          k.kind = 0;
          k.key = 0;   // off in this tick, on again in the next
        }
        k.nextKey = 0x20;
        if (!(e & 0x80)) n += k.transpose;
        n &= 0xff;
        k.nextFrequency = n < 96 ? static_cast<uint16_t>(kFnum[n % 12] | (n / 12) * 1024) : 0;
        k.slideCommand = slideSeen;
        k.vibDepth = 0;
        return true;
      }
    }
    return false;
  }

  void startEvent(int t) {
    Track& k = tr_[t];
    k.rowsLeft = static_cast<int16_t>(k.fetched & 0xfff);
    if (t == 9) {
      startSample(k.instrument);
      return;
    }
    int i = 0;
    if (k.fetched < 0x6000) {   // a note of any kind
      if (k.vibCommand != kNoVibrato) {
        k.vibDepth = k.vibCommand & 0xff;
        k.vibSpeed = (k.vibCommand >> 8) & 0xf;
        k.vibCounter = static_cast<int8_t>(k.vibSpeed / 2);
        k.vibCommand = kNoVibrato;
      }
      k.volume = k.volumeCommand;
      k.slide = k.slideCommand;
      if (k.kind == 1) return;
      k.frequency = k.nextFrequency;
      if (k.kind == 2) return;
      k.key = k.nextKey;
      i = k.instrument;
    }
    // (a rest loads instrument 0)
    const long long a = instruments_ + 16LL * i;
    k.carLevel = byte(a + 2);
    k.modLevel = byte(a + 7);
    k.connection = byte(a + 10);
    k.pulseOn = byte(a + 11);
    if (k.pulseOn) {
      k.pulseSpeed = byte(a + 12);
      k.pulsePos = a + 8;
      k.pulseSteps = 0;
      k.pulseTimer = 0;
    }
    const int car = kCarOp[t], mod = kModOp[t];
    out(0x60 + car, byte(a + 0));
    out(0x80 + car, byte(a + 1));
    out(0x20 + car, byte(a + 3));
    out(0xe0 + car, byte(a + 4));
    out(0x60 + mod, byte(a + 5));
    out(0x80 + mod, byte(a + 6));
    out(0x20 + mod, byte(a + 8));
    out(0xe0 + mod, byte(a + 9));
  }

  void effects(Track& k) {
    k.frequency = static_cast<uint16_t>(k.frequency + k.slide);
    if (k.vibCounter >= 0) {
      k.frequency = static_cast<uint16_t>(k.frequency + k.vibDepth);
      k.vibCounter = static_cast<int8_t>(k.vibCounter - 1);
      if (k.vibCounter <= 0) k.vibCounter = static_cast<int8_t>(-k.vibSpeed);
    } else {
      k.frequency = static_cast<uint16_t>(k.frequency - k.vibDepth);
      k.vibCounter = static_cast<int8_t>(k.vibCounter + 1);
      if (k.vibCounter == 0) k.vibCounter = static_cast<int8_t>(k.vibSpeed);
    }
    const int level = k.trackVolume + k.volume;
    k.level = static_cast<uint8_t>(level > 63 ? 63 : level);
    if (k.pulseOn) {
      k.pulseTimer = static_cast<uint8_t>(k.pulseTimer - 1);
      if (static_cast<int8_t>(k.pulseTimer) < 0) {
        k.pulseTimer = k.pulseSpeed;
        if (k.pulseSteps == 0) {   // on to the next entry of the table
          const int e = byte(k.pulsePos + 3);
          k.pulsePos = pulses_ + 4LL * (e - 1);
          k.pulseSteps = static_cast<uint8_t>(byte(k.pulsePos + 2) + 1);
          if (byte(k.pulsePos) != 0xff) k.modLevel = byte(k.pulsePos);
        }
        k.pulseSteps = static_cast<uint8_t>(k.pulseSteps - 1);
        k.modLevel = (k.modLevel + byte(k.pulsePos + 1)) & 0x3f;
      }
    }
  }

  static int atLeast(uint8_t l, uint8_t x) {
    const int a = l & 0x3f;
    return (a > x ? a : x) | (l & 0xc0);
  }

  void writeVoice(int c) {
    const Track& k = tr_[c];
    out(0x40 + kCarOp[c], atLeast(k.carLevel, k.level));
    out(0x40 + kModOp[c], (k.connection & 1) ? atLeast(k.modLevel, k.level) : k.modLevel);
    out(0xc0 + c, k.connection);
    out(0xa0 + c, k.frequency & 0xff);
    out(0xb0 + c, ((k.frequency >> 8) + k.key) & 0xff);
  }

  void startSample(int i) {
    playing_ = false;
    const long long a = sampleTable_ + 8LL * i;
    if (!s_ || a + 8 > static_cast<long long>(d_.size())) {
      if (log_) log_->line("P off");
      return;
    }
    const long long size = static_cast<long long>(s_->size());
    start_ = word(a);
    length_ = word(a + 2) + 1;
    if (start_ >= size) {
      if (log_) log_->line("P off");
      return;
    }
    if (start_ + length_ > size) length_ = size - start_;
    const int tc = byte(a + 6);
    step_ = (1000000.0 / (256 - tc)) / rate_;
    position_ = 0;
    playing_ = true;
    if (log_) log_->line("P " + std::to_string(start_) + " " + std::to_string(length_) + " " + std::to_string(tc));
  }

  const std::vector<uint8_t>& d_;
  const std::vector<uint8_t>* s_;   // the samples, or none
  int rate_;
  Copl& opl_;
  OplLog* log_;
  double hz_ = 70;
  long long subsongs_ = 0, patterns_ = 0, instruments_ = 0, pulses_ = 0, sampleTable_ = 0;
  Track tr_[10];
  long long work_ = 0;
  bool playing_ = false;
  long long start_ = 0, length_ = 0;
  double position_ = 0, step_ = 0;
};

// The path with its last extension replaced, if such a file is there.
std::string beside(const std::string& path, const char* ext) {
  const size_t slash = path.find_last_of('/');
  const size_t dot = path.find_last_of('.');
  const std::string stem = dot != std::string::npos && (slash == std::string::npos || dot > slash)
                               ? path.substr(0, dot) : path;
  const std::string there = stem + ext;
  if (FILE* f = std::fopen(there.c_str(), "rb")) {
    std::fclose(f);
    return there;
  }
  return std::string();
}

}  // namespace

int playD01Cr(const char* given, int track, int rate, long long startMs) {
  std::string path = given;
  std::vector<uint8_t> d = readFile(given);
  if (hasId(d) && d[6] == 1) {   // the samples were given: play the song beside them
    path = beside(given, ".d01");
    if (path.empty()) path = beside(given, ".D01");
    if (path.empty()) return fail("these are the samples of an EdLib D01 song; the song isn't there");
    d = readFile(path.c_str());
  }
  if (d.size() < 0x77 || !hasId(d) || d[6] != 0x80 || d[9] == 0) return fail("not an EdLib D01 song");

  std::vector<uint8_t> samples;
  bool haveSamples = false;
  {
    std::string sp = beside(path, ".s01");
    if (sp.empty()) sp = beside(path, ".S01");
    if (!sp.empty()) {
      samples = readFile(sp.c_str());
      haveSamples = samples.size() >= 0x40 && hasId(samples);
    }
  }

  // the subsongs that last more than two seconds are the tracks
  const int hz = timerRate(d);
  const long long most = 1200LL * hz;
  std::vector<std::pair<int, long long>> offered;
  long long tried = 0;
  for (int s = 0; s < d[9] && tried < kMostDryTicks; s++) {
    Chip probe(8000);
    D01 p(d, nullptr, s, 8000, probe, nullptr);
    long long n = 1;
    while (p.tick() && ++n < most) {}
    tried += n;
    if (n > 2LL * hz) offered.emplace_back(s, n);
  }
  if (offered.empty()) return fail("the song is empty");
  if (track < 0 || track >= static_cast<int>(offered.size())) track = 0;
  const long long ticks = offered[track].second;

  Info info;
  info.tracks = static_cast<int>(offered.size());
  info.track = track;
  info.title = fromCp437(field(&d[0x0b], 32));
  info.author = fromCp437(field(&d[0x2b], 32));
  info.type = "EdLib D01";
  info.facts.emplace_back("player", "JCH's player 1.03 with samples; played by a replayer written for this player");
  info.facts.emplace_back("timer", std::to_string(d[8]) + " Hz");
  if (haveSamples) {
    info.facts.emplace_back("samples", "yes");
  } else {
    info.facts.emplace_back("samples", "missing (no .s01 beside the song)");
    info.comment = "The samples (.s01) are not there: the drums are missing.";
  }
  info.durationMs = static_cast<long long>(ticks * 1000 / static_cast<double>(hz));
  OplLog log("d01", path.c_str(), track, info, ticks, hz, rate);
  Chip opl(rate);
  D01 song(d, haveSamples ? &samples : nullptr, offered[track].first, rate, opl, log.on() ? &log : nullptr);
  opl.extra = [&song](short* buf, int frames) { song.mix(buf, frames); };
  return play(song, opl, info, rate, startMs);
}

}  // namespace adlib
