// SPDX-License-Identifier: GPL-3.0-or-later
// First Samurai's music (Modland "MUS", .mus): the Ad Lib sound module of the PC game
// (Vivid Image, 1991; music by Nick Jones) with its four tunes. Written for this player
// in a clean room, from a description of the format only.
//
// The file is the game's whole sound module, a program followed by its data. This player
// treats it as data only: it checks the module's identification bytes, and plays the
// song tables that start at $0D0D. The file is also the player's memory (a 64 KiB image
// with the file at address 0): the state of the ten channels lives in 64-byte records at
// fixed places in it, so tracks of a damaged file that write into tables do what they
// would do there.
//
// A sound is a track: its channel (0..8 a voice, 9 a control channel that starts and
// stops other sounds), its priority, then pairs of bytes: a note and its length, or a
// command and its argument (instruments, rests, pattern calls and returns, loops, jumps,
// slurs, levels, speeds, slides, vibrato, random lengths and pitches...). Each channel
// steps when its speed, added up every tick, passes 255. The four tunes are sounds 33,
// 30, 31 and 32, each a control-channel track that starts the voices. A tune ends once
// every voice it started has gone round once or stopped.
//
// The test mode (BBMP_ADLIB_LOG, common.h) lists every chip write.

#include <cstring>

#include "common.h"

namespace adlib {

namespace {

constexpr double kMusHz = 1193182.0 / 23864;   // the PC timer, divided as the game sets it
constexpr int kTunes[4] = {33, 30, 31, 32};
constexpr uint8_t kId[18] = {0x50, 0x53, 0x51, 0x52, 0x1e, 0x06, 0x57, 0x56, 0x8c,
                             0xc8, 0x8e, 0xd8, 0xe8, 0xac, 0x01, 0xe8, 0x0d, 0x02};

// Fixed places in the image.
enum : uint16_t {
  aActive = 0x26, aGlobalSpeed = 0x27, aRandom = 0x28, aSync = 0x2b,   // sync: $2B..$2F
  aQueueWrite = 0x36, aQueueRead = 0x38, aQueue = 0x3a,
  aRecords = 0x2ca, aOperators = 0x2de, aFnums = 0x2e7, aBase = 0x0d0d,
};

// Fields of a channel record.
enum : uint16_t {
  fTranspose0 = 0x00, fAtten = 0x01, fNoteSent = 0x02, fSteps = 0x03, fPos = 0x04, fLoopCount = 0x06,
  fInsTranspose = 0x07, fPriority = 0x08, fDepth = 0x09, fReturns = 0x0a, fTranspose = 0x12,
  fSlideRate = 0x13, fSlideAcc = 0x14, fSlideStep = 0x15, fVibStep = 0x17, fVibShift = 0x19,
  fVibCount = 0x1a, fVibPeriod = 0x1b, fVibDelay = 0x1c, fVibDelayCount = 0x1d, fNote = 0x1e,
  fSound = 0x20, fPattern = 0x23, fFine = 0x24, fSpeed = 0x25, fSpeedAcc = 0x26, fA0 = 0x27,
  fB0 = 0x28, fKind = 0x29, fModLevel = 0x2e, fCarLevel = 0x2f, fVolume = 0x30, fAdditive = 0x31,
  fExtra1 = 0x32, fExtra2 = 0x33, fExtraByte = 0x34, fKeyOffAt = 0x35, fRandomMask = 0x36,
  fFollow = 0x39, fInstrument = 0x3b, fLoopPos = 0x3c, fTrack = 0x3e,
};

// The pitch effect kinds.
constexpr uint16_t kNone = 0x0be3, kVibrato = 0x0be4, kSlide = 0x0c31;

uint8_t clamp(uint32_t v) {
  v &= 0xff;
  if (v <= 63) return static_cast<uint8_t>(v);
  return v >= 128 ? 0 : 63;
}

class Mus : public Song {
 public:
  Mus(const std::vector<uint8_t>& d, int sound, Copl& opl, OplLog* log) : opl_(opl), log_(log) {
    opl.init();
    mem_.assign(0x10000, 0);
    std::memcpy(mem_.data(), d.data(), d.size() < 0x10000 ? d.size() : 0xffff);
    // reset
    put8(aActive, 0);
    put16(aRandom, 0x1234);
    out(0x01, 0x20);
    out(0x08, 0x00);
    out(0xbd, 0xc0);
    for (int c = 9; c >= 0; c--) {
      if (c != 9) {
        out(0x40 + op(c), 0x3f);
        out(0x43 + op(c), 0x3f);
      }
      clear(record(c));
    }
    put16(aQueueRead, 0);   // both queue indexes 0
    put16(aQueueWrite, 0);
    // queue the tune
    const uint16_t wi = w16(aQueueWrite);
    put8(aQueue + wi, sound);
    put16(aQueueWrite, (wi + 1) & 15);
  }

  double hz() const override { return kMusHz; }

  bool tick() override {
    if (log_) log_->tickStart();
    // the sounds asked for (a damaged queue could go round for ever: once round)
    for (int n = 0; n < 16 && w16(aQueueRead) != w16(aQueueWrite); n++) {
      const uint16_t ri = w16(aQueueRead);
      startSound(b8(aQueue + ri));
      put16(aQueueRead, (ri + 1) & 15);
    }
    if (b8(aActive) != 0) {
      for (int ch = 9; ch >= 0; ch--) channel(ch);
    }
    ticks_++;
    bool goesOn = false, anyUsed = false;
    for (int c = 0; c < 9; c++) {
      if (!used_[c]) continue;
      anyUsed = true;
      if (!looped_[c] && b8(record(c) + fSteps) != 0) goesOn = true;
    }
    if (!anyUsed) goesOn = ticks_ == 1;
    if (log_) log_->tickEnd(goesOn);
    return goesOn;
  }

 private:
  enum class End { kEffect, kNext };

  // the image: bytes and little-endian words, addresses modulo 65536
  uint8_t b8(uint32_t a) const { return mem_[a & 0xffff]; }
  uint16_t w16(uint32_t a) const { return static_cast<uint16_t>(b8(a) | b8(a + 1) << 8); }
  void put8(uint32_t a, uint32_t v) { mem_[a & 0xffff] = static_cast<uint8_t>(v); }
  void put16(uint32_t a, uint32_t v) {
    put8(a, v);
    put8(a + 1, v >> 8);
  }

  void out(int reg, int val) {
    opl_.write(reg & 0xff, val & 0xff);
    if (log_) log_->write(reg, val);
  }

  uint16_t record(int c) const { return w16(aRecords + 2 * c); }
  uint8_t op(int c) const { return b8(aOperators + c); }

  void clear(uint16_t r) {
    for (int i = 1; i < 0x40; i++) put8(r + i, 0);
    put8(r + fSpeed, 0xff);
    put8(r + fSpeedAcc, 0xff);
    put8(r + fInstrument, 0xff);
    put8(r + fPriority, 0xff);
    put16(r + fKind, kNone);
  }

  uint16_t draw() {
    uint32_t s = (w16(aRandom) + 0x9248) & 0xffff;
    s = ((s >> 3) | (s << 13)) & 0xffff;
    put16(aRandom, s);
    return static_cast<uint16_t>(s);
  }

  void length(uint16_t r, uint8_t n) {
    const uint8_t mask = b8(r + fRandomMask);
    put8(r + fSteps, mask ? n + (draw() & mask) : n);
  }

  uint8_t carrierLevel(uint16_t r) const {
    return clamp((b8(r + fCarLevel) & 63) + b8(r + fAtten) + b8(r + fVolume)) | (b8(r + fCarLevel) & 0xc0);
  }
  uint8_t modulatorLevel(uint16_t r) const {
    uint32_t v = b8(r + fModLevel) & 63;
    if (b8(r + fAdditive)) v += b8(r + fAtten) + b8(r + fVolume);
    return clamp(v) | (b8(r + fModLevel) & 0xc0);
  }
  void writeLevels(uint16_t r, int c) {
    out(0x43 + op(c), carrierLevel(r));
    if (b8(r + fAdditive)) out(0x40 + op(c), modulatorLevel(r));
  }

  void setNote(uint16_t r, int ch, uint8_t a) {
    put8(r + fNote, a);
    const uint8_t n = a + b8(r + fTranspose) + b8(r + fInsTranspose) + b8(r + fTranspose0);
    put8(r + fNoteSent, n);
    const uint8_t block = n / 12 - 1;
    const uint16_t f = w16(aFnums + 2 * (n % 12)) + b8(r + fFine);
    const uint8_t hi = ((block * 4) & 0x1c) | (f >> 8) | (b8(r + fB0) & 0x20);
    put8(r + fB0, hi);
    put8(r + fA0, f & 0xff);
    out(0xa0 + ch, f & 0xff);
    out(0xb0 + ch, hi);
  }

  void keyOn(uint16_t r, int ch) {
    put8(r + fB0, b8(r + fB0) | 0x20);
    out(0xb0 + ch, b8(r + fB0));
    const uint8_t s = 9 - b8(r + fVibShift);
    const uint16_t f = w16(r + fA0) & 0x3ff;
    put16(r + fVibStep, (s < 16 ? f >> s : 0) & 0xff);
    put8(r + fVibDelayCount, b8(r + fVibDelay));
  }

  void keyOff(uint16_t r, int ch) {
    if (ch == 9) return;
    put8(r + fB0, b8(r + fB0) & 0xdf);
    out(0xb0 + ch, b8(r + fB0));
  }

  void silence(int c) {
    out(0x60 + op(c), 0xff);
    out(0x63 + op(c), 0xff);
    out(0x80 + op(c), 0xff);
    out(0x83 + op(c), 0xff);
    out(0xb0 + c, 0);
    out(0xb0 + c, 0x20);
  }

  void loadInstrument(uint16_t r, int ch, uint8_t i) {
    const uint16_t ins = aBase + w16(aBase + 4) + 16 * i;
    out(0x20 + op(ch), b8(ins + 0));
    out(0x23 + op(ch), b8(ins + 1));
    out(0xc0 + ch, b8(ins + 2));
    put8(r + fAdditive, b8(ins + 2) & 1);
    out(0xe0 + op(ch), b8(ins + 3));
    out(0xe3 + op(ch), b8(ins + 4));
    put8(r + fModLevel, b8(ins + 5));
    out(0x40 + op(ch), modulatorLevel(r));
    put8(r + fCarLevel, b8(ins + 6));
    out(0x43 + op(ch), carrierLevel(r));
    out(0x60 + op(ch), b8(ins + 7));
    out(0x63 + op(ch), b8(ins + 8));
    out(0x80 + op(ch), b8(ins + 9));
    out(0x83 + op(ch), b8(ins + 10));
    put8(r + fInsTranspose, b8(ins + 11));
  }

  uint16_t pattern(uint8_t p) const { return aBase + w16(aBase + 8) + w16(aBase + w16(aBase + 2) + 2 * p); }

  void startSound(uint8_t n) {
    const uint16_t t = aBase + w16(aBase + 6) + w16(aBase + w16(aBase) + 2 * n);
    const uint8_t c = b8(t), p = b8(t + 1);
    const uint16_t r = record(c);
    if (p > b8(r + fPriority)) return;   // something more important is playing
    clear(r);
    put8(r + fSound, n);
    put8(r + fPriority, p);
    put16(r + fTrack, t);
    put16(r + fPos, t + 2);
    put8(r + fSteps, 1);
    put8(aActive, 1);
    if (c < 9) {
      used_[c] = true;
      looped_[c] = false;
    }
    if (c != 9) silence(c);
  }

  void channel(int ch) {
    const uint16_t r = record(ch);
    if (b8(r + fSteps) == 0) return;
    if (b8(r + fFollow)) put8(r + fSpeed, b8(aGlobalSpeed));
    const uint32_t sum = b8(r + fSpeedAcc) + b8(r + fSpeed);
    put8(r + fSpeedAcc, sum);
    if (sum <= 255) {
      pitchEffect(r, ch);
      return;
    }
    const uint8_t left = b8(r + fSteps) - 1;
    put8(r + fSteps, left);
    if (left != 0) {
      if (ch == 9) return;
      if (left == b8(r + fKeyOffAt)) keyOff(r, ch);
      pitchEffect(r, ch);
      return;
    }
    if (readTrack(r, ch) == End::kEffect) pitchEffect(r, ch);
  }

  void pitchEffect(uint16_t r, int ch) {
    const uint16_t kind = w16(r + fKind);
    if (kind == kVibrato) {
      if (b8(r + fVibDelayCount)) {
        put8(r + fVibDelayCount, b8(r + fVibDelayCount) - 1);
        return;
      }
      const uint8_t c = b8(r + fVibCount) - 1;
      put8(r + fVibCount, c);
      if (c == 0) {
        put16(r + fVibStep, -w16(r + fVibStep));
        put8(r + fVibCount, b8(r + fVibPeriod));
      }
      const uint16_t f = (w16(r + fA0) & 0x3ff) + w16(r + fVibStep);
      put8(r + fA0, f & 0xff);
      put8(r + fB0, (b8(r + fB0) & 0xfc) | (f >> 8));
      out(0xa0 + ch, b8(r + fA0));
      out(0xb0 + ch, b8(r + fB0));
    } else if (kind == kSlide) {
      const uint32_t acc = b8(r + fSlideAcc) + b8(r + fSlideRate);
      put8(r + fSlideAcc, acc);
      if (acc <= 255) return;
      const uint8_t b0 = b8(r + fB0);
      int block = b0 & 0x1c;
      const int key = b0 & 0x20;
      const int16_t d = static_cast<int16_t>(w16(r + fSlideStep));
      int16_t f = static_cast<int16_t>(b8(r + fA0) + 256 * (b0 & 3) + d);
      if (d >= 0) {
        if (f >= 0x2de) {
          f = static_cast<int16_t>(f >> 1);
          block = (block + 4) & 0x1c;
        }
      } else if (f <= 0x184) {
        f = static_cast<int16_t>(2 * f);
        block = (block - 4) & 0x1c;
      }
      const int fn = f & 0x3ff;
      out(0xa0 + ch, fn & 0xff);
      put8(r + fA0, fn & 0xff);
      const uint8_t hi = (fn >> 8) | block | key;
      out(0xb0 + ch, hi);
      put8(r + fB0, hi);
    }
  }

  // Reads the channel's track up to the next note, rest or wait (or its end).
  End readTrack(uint16_t r, int ch) {
    const uint16_t start = w16(r + fPos);
    uint16_t pos = start;
    auto next = [&]() { return b8(pos++); };
    for (int items = 0; items < 4096; items++) {
      const uint16_t item = pos;
      const uint8_t a = next(), b = next();
      if (a < 0x80) {   // a note
        if (b8(r + fKeyOffAt) == 0 && ch != 9) keyOff(r, ch);
        setNote(r, ch, a);
        keyOn(r, ch);
        length(r, b);
        if (b8(r + fExtra1) || b8(r + fExtra2) || b8(r + fExtraByte)) {
          put8(r + fExtraByte, next());
          out(0x43 + op(ch), carrierLevel(r));
          out(0x40 + op(ch), modulatorLevel(r));
        }
        put16(r + fPos, pos);
        return End::kEffect;
      }
      switch (a - 0x80) {
        case 0x00:   // instrument
          if (b != b8(r + fInstrument)) {
            put8(r + fInstrument, b);
            loadInstrument(r, ch, b);
          }
          break;
        case 0x01:   // rest
          length(r, b);
          put16(r + fPos, pos);
          keyOff(r, ch);
          return End::kEffect;
        case 0x02: {   // call a pattern
          const uint8_t depth = b8(r + fDepth);
          put16(r + fReturns + 2 * depth, pos);
          put8(r + fDepth, depth + 1);
          put8(r + fPattern, b);
          pos = pattern(b);
          break;
        }
        case 0x03: {   // return from it
          const uint8_t depth = b8(r + fDepth) - 1;
          put8(r + fDepth, depth);
          pos = w16(r + fReturns + 2 * depth);
          break;
        }
        case 0x04:   // loop
          if (b8(r + fLoopCount) == 0) {
            put8(r + fLoopCount, b);
            put16(r + fLoopPos, pos);
          } else {
            const uint8_t left = b8(r + fLoopCount) - 1;
            put8(r + fLoopCount, left);
            if (left) pos = w16(r + fLoopPos);
          }
          break;
        case 0x05:   // jump: the track has gone round
          pos = w16(r + fTrack) + b;
          if (ch < 9) looped_[ch] = true;
          break;
        case 0x07: {   // slur into a note
          const uint8_t n = next(), l = next();
          setNote(r, ch, n);
          length(r, l);
          put16(r + fPos, pos);
          return End::kEffect;
        }
        case 0x08:   // wait
          length(r, b);
          put16(r + fPos, pos);
          return End::kEffect;
        case 0x09:   // strike again
          length(r, b);
          keyOn(r, ch);
          put16(r + fPos, pos);
          return End::kEffect;
        case 0x0a: put8(r + fKeyOffAt, b); break;
        case 0x0b:
          put8(r + fVolume, b ^ 0x3f);
          writeLevels(r, ch);
          break;
        case 0x0c:
          put8(r + fAtten, (b >> 1) ^ 0x3f);
          writeLevels(r, ch);
          break;
        case 0x0d:
          put8(r + fAtten, clamp(b8(r + fAtten) - b));
          writeLevels(r, ch);
          break;
        case 0x0e:
        case 0x0f: {   // another channel's volume
          const uint8_t k = next(), v = next();
          const uint16_t rk = record(k);
          put8(rk + fVolume, a == 0x8e ? v ^ 0x3f : clamp(b8(rk + fVolume) - v));
          writeLevels(rk, k);
          break;
        }
        case 0x10: put8(aGlobalSpeed, b); break;
        case 0x11: put8(r + fSpeed, b); break;
        case 0x12: put8(r + fSpeed, b8(aGlobalSpeed)); break;
        case 0x13: {
          const uint8_t old = b8(r + fSpeed);
          uint32_t v;
          if (b < 0x80) {
            v = old + b;
            if (v > 255) v = 255;
          } else {
            v = (old + b) & 0xff;
            if (v >= old) v = 1;
          }
          put8(r + fSpeed, v);
          break;
        }
        case 0x14: put8(r + fFollow, b); break;
        case 0x15: {   // slide
          put8(r + fSlideRate, b);
          const uint8_t x = next(), y = next();
          put16(r + fSlideStep, 256 * x + y);
          put16(r + fKind, kSlide);
          put8(r + fSlideAcc, 0xff);
          break;
        }
        case 0x16:
          put16(r + fKind, kNone);
          put16(r + fSlideStep, 0);
          break;
        case 0x17: put8(r + fFine, b); break;
        case 0x18: put8(r + fTranspose, b); break;
        case 0x19: {   // a random pitch
          const uint16_t m = static_cast<uint16_t>(next() | next() << 8);
          const uint16_t rnd = draw() & m;
          const uint32_t v = b8(r + fA0) + 256 * (b8(r + fB0) & 0x1f) + rnd;
          out(0xa0 + ch, v & 0xff);
          out(0xb0 + ch, (v >> 8) | (b8(r + fB0) & 0x20));
          break;
        }
        case 0x1a: put8(r + fRandomMask, b); break;
        case 0x1b: startSound(b); break;
        case 0x1c: {   // stop a channel
          const uint16_t rk = record(b);
          put8(rk + fSteps, 0);
          put8(rk + fPriority, 0xff);
          put16(rk + fPos, 0);
          if (b != 9) {
            out(0xc0 + b, 0);
            out(0x43 + op(b), 0x3f);
            out(0x83 + op(b), 0xff);
            out(0xb0 + b, 0);
          }
          break;
        }
        case 0x1d: put8(r + fPriority, b); break;
        case 0x1e:
          put8(aSync, b >> 1);
          put8(aSync + 1, b >> 1);
          put8(aSync + 2, 0xff);
          put8(aSync + 3, 0);
          put8(aSync + 4, 0);
          break;
        case 0x1f: {   // wait for the game's signal, which never comes here
          bool wait = true;
          if (b8(aSync + 4) == 0) {
            if ((b & b8(aSync + 3)) == 0) put8(aSync + 4, b8(aSync + 4) + 1);
          } else if (b & b8(aSync + 3)) {
            put8(aSync + 4, 0);
            wait = false;
          }
          if (wait) {
            put8(r + fSteps, 1);
            put16(r + fPos, item);
            return End::kNext;
          }
          break;
        }
        case 0x20:
          put8(r + fExtra1, b);
          put8(r + fExtra2, next());
          break;
        case 0x21: {   // vibrato
          put8(r + fVibShift, b);
          const uint8_t q = next();
          put8(r + fVibCount, q + 1);
          put8(r + fVibPeriod, 2 * q);
          put8(r + fVibDelay, next());
          put16(r + fKind, kVibrato);
          break;
        }
        default:   // $06 and everything from $22 on: the end of the track
          put8(r + fPriority, 0xff);
          put16(r + fPos, 0);
          keyOff(r, ch);
          if (ch < 9) looped_[ch] = true;
          return End::kNext;
      }
    }
    put16(r + fPos, start);   // a track that never comes to a note: try again next step
    return End::kNext;
  }

  Copl& opl_;
  OplLog* log_;
  std::vector<uint8_t> mem_;
  long long ticks_ = 0;
  bool used_[9] = {}, looped_[9] = {};
};

bool isMus(const std::vector<uint8_t>& d) {
  if (d.size() < 4096 || d.size() > 65535) return false;
  if (std::memcmp(&d[0x340], kId, sizeof(kId)) != 0) return false;
  return (d[0x1c] | d[0x1d] << 8) == 0x340;
}

}  // namespace

int playMusCr(const char* path, int track, int rate, long long startMs) {
  const std::vector<uint8_t> d = readFile(path);
  if (!isMus(d)) return fail("not First Samurai's music driver");
  if (track < 0 || track > 3) track = 0;
  const int sound = kTunes[track];
  long long ticks = 0;
  {
    Chip probe(8000);
    Mus p(d, sound, probe, nullptr);
    const long long most = static_cast<long long>(1200 * p.hz());
    while (ticks < most && p.tick()) ticks++;
  }
  Info info;
  info.tracks = 4;
  info.track = track;
  info.title = baseName(path);
  info.type = "First Samurai (Ad Lib driver)";
  info.facts.emplace_back("tune", "sound " + std::to_string(sound) + " of the driver");
  info.facts.emplace_back("driver", "the game's own, in the file; played by a replayer written for this player");
  info.facts.emplace_back("timer", "50 Hz");
  info.durationMs = static_cast<long long>(ticks * 1000 / kMusHz);
  OplLog log("samurai", path, track, info, ticks, kMusHz, rate);
  Chip opl(rate);
  Mus song(d, sound, opl, log.on() ? &log : nullptr);
  return play(song, opl, info, rate, startMs);
}

}  // namespace adlib
