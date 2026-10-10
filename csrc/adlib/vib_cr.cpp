// SPDX-License-Identifier: GPL-3.0-or-later
// Vibrants' music (.vib): JO's (Poul-Jesper Olsen's) Ad Lib player, as the Copper demo
// (Surprise! Productions, 1992) plays it. Written for this player in a clean room, from a
// description of the format only.
//
// The file is also the player's memory: it lies at address 0 of a 64 KiB image, and every
// voice keeps its state in a record of $5D bytes inside it. The music's own commands
// store bytes into that record by number ("variables"), so the image is the whole state;
// the song ends when the image comes back to one it has had before (the loop is played
// once), or when every voice has ended.
//
// A sound is a list of voices: record, instrument table, sequence, speed, OPL channel.
// Sound 0 silences, sound 1 is the tune. A sequence is a list of pattern addresses and
// commands; a pattern is a list of notes, rests, holds and commands; each voice also runs
// three small programs (lists of stores into its variables) every tick.
//
// Every register of a sounding voice is written every tick, also when its value has not
// changed: the chip emulator takes buffered writes one after the other, so the number of
// writes is part of the sound, and this is how the references were made. The test mode
// (BBMP_ADLIB_LOG, common.h) lists them all.

#include <cstring>
#include <unordered_set>

#include "common.h"

namespace adlib {

namespace {

constexpr double kVibHz = 1193182.0 / 17045;   // the PC timer, divided as the demo sets it
constexpr int kRecord = 0x5d;

// The voice's operators by its channel word: carrier and modulator.
constexpr uint8_t kModOp[9] = {0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12};
constexpr uint8_t kCarOp[9] = {0x03, 0x04, 0x05, 0x0b, 0x0c, 0x0d, 0x13, 0x14, 0x15};
// F-numbers of C to B (the block goes in bits 10..12)
constexpr uint16_t kFnum[12] = {0x157, 0x16b, 0x181, 0x198, 0x1b0, 0x1ca,
                                0x1e5, 0x202, 0x220, 0x241, 0x263, 0x287};

// Variables of a voice's record (byte numbers).
enum : uint16_t {
  vInstrument = 0x00, vSpeed = 0x02, vKey = 0x04, vNewNote = 0x05, vRepeat = 0x06,
  vTranspose1 = 0x08, vTranspose2 = 0x0a, vSlide = 0x0c, vDetune1 = 0x0e, vDetune2 = 0x10,
  vVolume1 = 0x12, vVolume2 = 0x14, vVolume3 = 0x16, vProgramsOff = 0x18, vSlideSum = 0x1e,
  vPitchOffset = 0x20, vFixedNote = 0x22,
  vCountA = 0x24, vReloadA = 0x26,   // B and C follow 4 bytes apart
  vMult = 0x30,                      // carrier; the modulator's at +2 (likewise below)
  vLevel = 0x34, vWave = 0x38, vConnection = 0x3c, vAttack = 0x3e, vSustain = 0x42,
  vProgramA = 0x46,                  // B and C follow 2 bytes apart
  vChannel = 0x4c, vSteps = 0x4e, vNote = 0x4f, vPatternPos = 0x52, vSequence = 0x54,
  vInstrTable = 0x56, vInstrAddr = 0x58, vEnded = 0x5a, vCounter = 0x5c,
};

uint32_t scale(uint32_t a, uint32_t b) { return (a * b / 63) & 0xff; }

class Vib : public Song {
 public:
  Vib(const std::vector<uint8_t>& d, Copl& opl, OplLog* log) : opl_(opl), log_(log) {
    opl.init();
    mem_.assign(0x10000, 0);
    std::memcpy(mem_.data(), d.data(), d.size() < 0x10000 ? d.size() : 0xffff);
    // a clean chip: every register 0, from the top
    for (int r = 0xff; r >= 0; r--) out(r, 0);
    out(0x01, 0x20);
    out(0x08, 0x00);
    out(0xbd, 0xc0);
    setUp(w16(0x03));
    setUp(w16(0x05));
    seen_.insert(hash());
  }

  // What a damaged file is refused for: sound 1's list must name 1 to 9 voices that lie
  // in the file. (Everything else stays inside the 64 KiB image by itself.)
  static bool ok(const std::vector<uint8_t>& d) {
    const size_t size = d.size();
    if (size < 64 || size > 65535) return false;
    const size_t list = d[5] | d[6] << 8;
    if (list < 7 || list + 10 > size) return false;
    auto readable = [&](size_t a) { return a + 1 < size; };
    auto word = [&](size_t a) { return static_cast<size_t>(d[a] | d[a + 1] << 8); };
    int entries = 0;
    for (size_t e = list; readable(e) && word(e) > 0; e += 10) {
      if (e + 10 > size) return false;
      if (d[e + 7] > 8) return false;
      if (word(e) + kRecord > size) return false;
      if (word(e + 2) >= size || word(e + 4) >= size) return false;
      if (++entries > 9) return false;
    }
    return entries > 0;
  }

  double hz() const override { return kVibHz; }

  bool tick() override {
    if (log_) log_->tickStart();
    for (int ch = 8; ch >= 0; ch--) voice(ch);
    bool alive = false;
    for (int ch = 0; ch < 9; ch++)
      if (w16(voices_[ch] + vSequence) != 0) alive = true;
    // the image holds everything, so an image seen before means the music repeats
    const bool fresh = seen_.insert(hash()).second;
    const bool goesOn = alive && fresh;
    if (log_) log_->tickEnd(goesOn);
    return goesOn;
  }

 private:
  // the image: bytes and little-endian words, addresses modulo 65536
  uint8_t b8(uint32_t a) const { return mem_[a & 0xffff]; }
  uint16_t w16(uint32_t a) const { return static_cast<uint16_t>(b8(a) | b8(a + 1) << 8); }
  void put8(uint32_t a, uint32_t v) { mem_[a & 0xffff] = static_cast<uint8_t>(v); }
  void put16(uint32_t a, uint32_t v) {
    put8(a, v);
    put8(a + 1, v >> 8);
  }
  uint8_t var(uint16_t n) const { return b8(rec_ + n); }
  uint16_t var16(uint16_t n) const { return w16(rec_ + n); }
  void setVar(uint16_t n, uint32_t v) { put8(rec_ + n, v); }
  void setVar16(uint16_t n, uint32_t v) { put16(rec_ + n, v); }

  void out(int reg, int val) {
    opl_.write(reg & 0xff, val & 0xff);
    if (log_) log_->write(reg, val);
  }

  // A 64-bit fingerprint of the whole image, eight bytes at a time. Each step is
  // reversible, so two images that differ in one place never share it, and the rotation
  // carries every bit into the rest; any two images sharing it by chance is as unlikely
  // as with any good 64-bit hash.
  uint64_t hash() const {
    uint64_t h = 0x243f6a8885a308d3ull;
    for (size_t i = 0; i < mem_.size(); i += 8) {
      uint64_t w;
      std::memcpy(&w, &mem_[i], 8);
      h ^= w;
      h = (h << 23 | h >> 41) * 0x9e3779b97f4a7c15ull;
    }
    return h;
  }

  // the operators of the record's channel word
  int carrier() const {
    const uint16_t c = var16(vChannel);
    return c < 9 ? kCarOp[c] : c < 18 ? kModOp[c - 9] : 0;
  }
  int modulator() const {
    const uint16_t c = var16(vChannel);
    return c < 9 ? kModOp[c] : 0;
  }

  // Release both operators at once (the sound dies away).
  void letGo() {
    out(0x60 + carrier(), 0xff);
    out(0x60 + modulator(), 0xff);
    out(0x80 + carrier(), 0xff);
    out(0x80 + modulator(), 0xff);
  }

  // Put a sound's voices in place.
  void setUp(uint16_t list) {
    // a list nobody ends would go round the image for ever: once round is enough
    for (int n = 0; n < 0x8000; n++, list += 10) {
      const uint16_t record = w16(list);
      if (record == 0) return;
      const uint16_t table = w16(list + 2), sequence = w16(list + 4);
      const uint8_t speed = b8(list + 6), channel = b8(list + 7), flags = b8(list + 9);
      if (channel < 9) voices_[channel] = record;
      rec_ = record;
      setVar16(vSequence, sequence);
      if (flags & 0x80) {
        setVar(vSteps, 1);
        continue;
      }
      setVar16(vSteps, 0x7f01);   // one step to go, resting
      setVar(vSpeed, speed);
      setVar(vChannel, channel);
      setVar(vCounter, 1);
      setVar16(vRepeat, 1);
      for (uint16_t v : {vDetune1, vDetune2, vTranspose1, vTranspose2, vPatternPos, vSlide, vProgramsOff})
        setVar16(v, 0);
      for (uint16_t v : {vVolume2, vVolume3, vVolume1}) setVar16(v, 63);
      setVar16(vInstrTable, table);
      letGo();
      out(0xb0 + var(vChannel), 0);
    }
  }

  // Store a command's byte into the variable it names; returns the variable's number.
  int command(uint16_t word) {
    const int n = (word >> 8) & 0x7f;
    setVar(n, word & 0xff);
    return n;
  }

  void frequency(uint32_t f) {
    out(0xa0 + var(vChannel), f & 0xff);
    out(0xb0 + var(vChannel), ((f >> 8) & 0x1f) | var(vKey));
  }

  void voice(int ch) {
    rec_ = voices_[ch];
    int scratch = 2 * (ch + 1);   // the end and the rest write it as a frequency
    if (var16(vSequence) == 0) return;   // the voice has ended

    // the speed: the high nibble counted down by ones, then the low one by sixteens
    bool step = false;
    const uint8_t speed = var(vSpeed);
    if (!(speed & 0x80)) {
      const uint8_t c = var(vCounter) - 1;
      setVar(vCounter, c);
      if (c & 0x80) {
        setVar(vCounter, speed & 0x70);
        setVar(vSpeed, speed ^ 0x80);
        step = true;
      }
    } else {
      const uint8_t c = var(vCounter) - 16;
      setVar(vCounter, c);
      if (c & 0x80) {
        setVar(vCounter, speed & 7);
        setVar(vSpeed, speed ^ 0x80);
        step = true;
      }
    }
    bool between = true;
    if (step) {
      const uint8_t left = (var(vSteps) & 0x7f) - 1;
      setVar(vSteps, left);
      between = left != 0;
    }

    if (between) {
      if (var(vNote) == 0x7f) return;   // resting: nothing to write
      if (var(vNewNote) & 0x80) {
        // the note's second tick: key on, and its envelope comes in
        setVar16(vKey, 0x0020);
        const uint16_t ins = var16(vInstrAddr);
        for (int i = 0; i < 4; i++) setVar(vAttack + 2 * i, b8(ins + 16 + i));
      }
      setVar16(vSlideSum, var16(vSlideSum) + static_cast<int8_t>(var(vSlide)));
      programs();
      registers();
      return;
    }

    // the next event
    uint16_t s = var16(vSequence);
    if (w16(s) == 0x7fff) {   // the voice ends here
      setVar16(vSequence, 0);
      setVar16(vKey, 0);
      setVar16(vEnded, 0);
      frequency(scratch);
      return;
    }
    if (w16(s) == 0x7ffe) {   // the song's loop
      s = w16(s + 2);
      setVar16(vSequence, s);
    }
    // (a voice whose every word is a command would read for ever: once round the image)
    for (int n = 0; n < 0x8000 && (w16(s) & 0x8000); n++) {
      scratch = command(w16(s));
      s += 2;
      setVar16(vSequence, var16(vSequence) + 2);
    }
    uint16_t p = w16(s) + var16(vPatternPos);
    const uint8_t high = b8(p + 1);
    if (high >= 0x80 || high < 0x7d) setVar(vSlide, 0);   // rests and holds keep the slide
    for (int n = 0; n < 0x8000 && (w16(p) & 0x8000); n++) {
      scratch = command(w16(p));
      p += 2;
      setVar16(vPatternPos, var16(vPatternPos) + 2);
    }
    setVar16(vPitchOffset, 0);
    const uint16_t e = w16(p);
    const uint8_t h = e >> 8, l = e & 0xff;
    if (h == 0x7f) {   // a rest
      setVar16(vSteps, e);
      setVar16(vKey, 0);
      letGo();
      advance(p);
      frequency(scratch);
      return;
    }
    if (h == 0x7d || h == 0x7e) {   // a hold, with the key on or off
      setVar(vSteps, l);
      setVar16(vKey, h == 0x7d ? 0x0020 : 0);
    } else {   // a note
      setVar16(vSlideSum, 0);
      setVar(vFixedNote, 0);
      const uint16_t ins = w16(var16(vInstrTable) + 2 * var(vInstrument));
      setVar16(vInstrAddr, ins);
      if (!(l & 0x80)) {   // not tied: the instrument starts again
        setVar16(vKey, 0x8000);
        setVar16(vProgramA, w16(ins));
        setVar16(vProgramA + 2, w16(ins + 4));
        setVar16(vProgramA + 4, w16(ins + 8));
        static const uint8_t kFrom[13] = {2, 3, 6, 7, 10, 11, 12, 13, 14, 15, 20, 21, 22};
        static const uint8_t kTo[13] = {0x24, 0x26, 0x28, 0x2a, 0x2c, 0x2e, 0x30,
                                        0x32, 0x34, 0x36, 0x38, 0x3a, 0x3c};
        for (int i = 0; i < 13; i++) setVar(kTo[i], b8(ins + kFrom[i]));
      }
      setVar16(vSteps, e);
    }
    advance(p);
    programs();
    registers();
  }

  // On to the next event of the pattern; at its end, play it again or go on.
  void advance(uint16_t p) {
    setVar16(vPatternPos, var16(vPatternPos) + 2);
    if (w16(p + 2) != 0) return;
    setVar16(vPatternPos, 0);
    const uint8_t left = var(vRepeat) - 1;
    setVar(vRepeat, left);
    if (left == 0) {
      setVar(vRepeat, 1);
      setVar16(vSequence, var16(vSequence) + 2);
    }
  }

  // The three programs: every so many ticks, one store into a variable.
  void programs() {
    for (int k = 0; k < 3; k++) {
      if (var(vProgramsOff) & (1 << k)) continue;
      const uint16_t count = vCountA + 4 * k, reload = vReloadA + 4 * k, addr = vProgramA + 2 * k;
      const uint8_t c = var(count) - 1;
      setVar(count, c);
      if (!(c & 0x80)) continue;
      uint16_t v = w16(var16(addr));
      if (v == 0x7fff) continue;   // stopped (the count keeps falling)
      if (v == 0x7ffe) {
        setVar16(addr, w16(var16(addr) + 2));
        v = w16(var16(addr));
      }
      command(v);
      setVar16(addr, var16(addr) + 2);
      setVar(count, var(reload));
    }
  }

  uint32_t chain(uint32_t v) const {
    return scale(scale(scale(scale(v, var(vVolume1)), var(vVolume2)), var(vVolume3)), 63);
  }

  // The voice's registers, all of them.
  void registers() {
    const int car = carrier(), mod = modulator();
    out(0x20 + car, var(vMult));
    out(0x20 + mod, var(vMult + 2));
    out(0x40 + car, (63 - chain(var(vLevel))) & 63);
    const uint32_t m = (var(vConnection) & 1) ? chain(var(vLevel + 2)) : var(vLevel + 2);
    out(0x40 + mod, (63 - m) & 63);
    if (!(var(vNewNote) & 0x80)) {
      out(0x60 + car, var(vAttack));
      out(0x60 + mod, var(vAttack + 2));
      out(0x80 + car, var(vSustain));
      out(0x80 + mod, var(vSustain + 2));
    }
    out(0xe0 + car, var(vWave));
    out(0xe0 + mod, var(vWave + 2));
    out(0xc0 + var(vChannel), var(vConnection));

    uint8_t n = var(vFixedNote);
    if (!(n & 0x80)) n = n + var(vNote) + var(vTranspose1) + var(vTranspose2);
    n &= 0x7f;
    // above the eight octaves the table runs on into the file's first bytes
    const uint16_t table = n < 96 ? static_cast<uint16_t>((n / 12) << 10 | kFnum[n % 12]) : w16(2 * (n - 96));
    const uint32_t f = (table + static_cast<int8_t>(var(vDetune1)) + static_cast<int8_t>(var(vDetune2)) +
                        static_cast<int8_t>(var(vPitchOffset)) + var16(vSlideSum)) & 0xffff;
    frequency(f);
  }

  Copl& opl_;
  OplLog* log_;
  std::vector<uint8_t> mem_;
  uint16_t voices_[9] = {};   // the record playing on each channel (0: the file's start)
  uint16_t rec_ = 0;          // the record being worked on
  std::unordered_set<uint64_t> seen_;
};

std::string copyright(const std::vector<uint8_t>& d) {
  for (size_t i = 0; i + 4 < d.size(); i++)
    if (d[i] == '(' && d[i + 1] == 'C' && d[i + 2] == ')') {
      const size_t n = d.size() - i < 64 ? d.size() - i : 64;
      return fromCp437(field(&d[i], n));
    }
  return std::string();
}

}  // namespace

int playVibCr(const char* path, int track, int rate, long long startMs) {
  (void)track;   // one song per file
  const std::vector<uint8_t> d = readFile(path);
  if (!Vib::ok(d)) return fail("not Vibrants' music");
  long long ticks = 0;
  {
    Chip probe(8000);
    Vib p(d, probe, nullptr);
    const long long most = static_cast<long long>(1200 * p.hz());
    while (ticks < most && p.tick()) ticks++;
  }
  Info info;
  info.title = baseName(path);
  info.type = "Vibrants (JO's Ad Lib player)";
  info.facts.emplace_back("player", "JO's, from the Copper demo (1992); played by a replayer written for this player");
  info.facts.emplace_back("timer", "70 Hz");
  info.comment = copyright(d);
  info.durationMs = static_cast<long long>(ticks * 1000 / kVibHz);
  OplLog log("vibrants", path, 0, info, ticks, kVibHz, rate);
  Chip opl(rate);
  Vib song(d, opl, log.on() ? &log : nullptr);
  return play(song, opl, info, rate, startMs);
}

}  // namespace adlib
