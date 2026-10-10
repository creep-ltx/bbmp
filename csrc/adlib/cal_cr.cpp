// SPDX-License-Identifier: GPL-3.0-or-later
// Shadowlands' music (.cal: the game's TUNE.CAL, unpacked; Domark/Teque 1992, music by
// Matt Furniss). Written for this player in a clean room, from a description of the
// format only.
//
// A small tracker module for the Ad Lib in rhythm mode: six melodic channels and the five
// drums. The header holds a title, an order list of 127 entries, a volume for each
// instrument, the number of melodic cells in a row and the number of "sounds"; each sound
// names its first and last order, whether it loops and how many melodic channels it uses
// (a sound with none is a sound effect slot, not a tune). Then a table of where each
// pattern lies (in 16-byte units), the patterns (64 rows: a bit mask, then a 4-byte cell
// for each set bit: note, instrument, effect, argument) and the instruments (64 bytes
// apart: a name, then two 13-byte operator descriptions and two wave bytes).
//
// The tune runs at 50 ticks a second, a row every "speed" ticks; effects: 1/2 slide
// up/down, 3 slide to a note, 6 note off, $0A volume slide, $0B jump to an order, $0C
// volume, $0D next pattern, $0F speed. It ends when it comes to an order it has played
// before (its loop is played once), or at its last order if it does not loop.
//
// The test mode (BBMP_ADLIB_LOG, common.h) lists every chip write.

#include "common.h"

namespace adlib {

namespace {

constexpr double kCalHz = 1193182.0 / 23864;   // the PC timer, divided as the game sets it

// The slots: melodic channels 0..5, then bass drum, snare, tom, cymbal, hihat.
constexpr int kSlots = 11;
constexpr uint8_t kCarOp[kSlots] = {0x03, 0x04, 0x05, 0x0b, 0x0c, 0x0d, 0x13, 0x14, 0x12, 0x15, 0x11};
constexpr uint8_t kModOp[7] = {0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10};
constexpr uint8_t kDrumBit[kSlots] = {0, 0, 0, 0, 0, 0, 0x10, 0x08, 0x04, 0x02, 0x01};
constexpr uint16_t kFnum[12] = {0x157, 0x16c, 0x181, 0x198, 0x1b1, 0x1cb,
                                0x1e6, 0x203, 0x222, 0x243, 0x266, 0x28a};

uint16_t tableValue(uint8_t k) {
  if (k < 96) return static_cast<uint16_t>((k / 12) << 12 | kFnum[k % 12]);
  if (k < 128) return 0xffff;
  return k == 129 ? 0x004b : 0;
}

uint8_t level(uint32_t loud, uint32_t v) { return static_cast<uint8_t>(63 - ((loud * v) & 0xffff) / 63); }

// The module's layout, found once.
struct Layout {
  size_t sounds = 0;      // S
  size_t patterns = 0;    // P
  size_t table = 0;       // T: the pattern table
  uint32_t area = 0;      // A: where the patterns start (16-bit)
  uint16_t first = 0;     // W(0)
  uint16_t instr = 0;     // G: the instruments' place, from W(0)
};

class Cal : public Song {
 public:
  Cal(const std::vector<uint8_t>& d, const Layout& lay, int sound, Copl& opl, OplLog* log)
      : d_(d), lay_(lay), opl_(opl), log_(log) {
    opl.init();
    const size_t s = 0x158 + 16 * static_cast<size_t>(sound);
    channels_ = at(s + 13);
    cells_ = at(0x118) ? at(0x118) : 16;

    // every register with its start value: rhythm mode, all drums off, operators quiet
    for (int r = 0; r < 256; r++) {
      int v = 0;
      if (r == 0x01 || r == 0xbd) {
        v = 0x20;
      } else if (r >= 0x20 && r < 0xa0 && (r & 0x1f) < 0x16 && (r & 7) < 6) {
        v = r < 0x40 ? 0x01 : r < 0x60 ? 0x3f : 0xf0;
      }
      out(r, v);
    }
    setFrequency(7, tableValue(31));
    for (int k = 0; k < kSlots; k++) {
      slot_[k].loaded = 0;
      keyOff(k);
      setVolume(k, 0);
    }
    for (int k = 0; k < channels_; k++) keyOff(k + 1);
    speed_ = 6;
    order_ = start_ = at(s + 10);
    end_ = static_cast<uint8_t>(at(s + 11) + 1);
    loops_ = at(s + 12) != 0;
    rowPos_ = lay_.area;
    counter_ = 0;
    break_ = false;
    row_ = 0;
    seen_[start_] = true;
    for (int k = 0; k < 8; k++) slot_[k].loaded = 0xff;
    on_ = true;
  }

  double hz() const override { return kCalHz; }

  bool tick() override {
    if (log_) log_->tickStart();
    if (on_) {
      counter_++;
      if (counter_ < speed_) {
        forPlayed([&](int k) { tickEffect(k); });
      } else {
        counter_ = 0;
        readRow();
        forPlayed([&](int k) { rowWork(k); });
        if (row_ == 64) nextPattern();
      }
      if (on_ && break_) nextPattern();
    }
    const bool goesOn = on_ && !looped_;
    if (log_) log_->tickEnd(goesOn);
    return goesOn;
  }

 private:
  struct Slot {
    uint8_t cell[4] = {};   // note, instrument, effect, argument
    uint8_t volume = 0, loaded = 0, loud1 = 0, loud2 = 0;
    uint16_t freq = 0, fnum = 0;
    uint8_t octave = 0;
    uint16_t target = 0;
    uint8_t up = 0, portaSpeed = 0, portaNote = 0;
    uint8_t cache = 0;   // the volume last written
  };

  uint8_t at(size_t o) const { return o < d_.size() ? d_[o] : 0; }
  uint16_t word(size_t o) const { return static_cast<uint16_t>(at(o) | at(o + 1) << 8); }
  uint16_t w(size_t k) const { return word(lay_.table + 2 * k); }

  void out(int reg, int val) {
    reg &= 0xff;
    val &= 0xff;
    copy_[reg] = static_cast<uint8_t>(val);
    opl_.write(reg, val);
    if (log_) log_->write(reg, val);
  }

  template <class F>
  void forPlayed(F f) {
    for (int k = 0; k < channels_; k++) f(k);
    for (int k = 6; k < kSlots; k++) f(k);
  }

  void keyOff(int s) {
    if (s < 6) out(0xb0 + s, copy_[0xb0 + s] & 0xdf);
    else out(0xbd, copy_[0xbd] & ~kDrumBit[s]);
  }
  void keyOn(int s) {
    if (s < 6) out(0xb0 + s, copy_[0xb0 + s] | 0x20);
    else out(0xbd, copy_[0xbd] | kDrumBit[s]);
  }
  void setFrequency(int s, uint16_t f) {
    if (s >= 8) return;
    const int hi = f >> 8;
    out(0xb0 + s, (copy_[0xb0 + s] & 0x20) | ((hi & 0x70) >> 2) | (hi & 3));
    out(0xa0 + s, f & 0xff);
  }

  void setVolume(int s, uint8_t v) {
    if (v >= 64) v = 63;
    Slot& sl = slot_[s];
    if (v == sl.cache) return;
    sl.cache = v;
    sl.volume = v;
    const int c = 0x40 + kCarOp[s];
    out(c, (copy_[c] & 0xc0) | level(s <= 6 ? sl.loud2 : sl.loud1, v));
  }

  // An operator's group of registers from its description o, with wave byte wave.
  void group(const uint8_t* o, int op, uint8_t wave) {
    out(0xe0 + op, wave & 3);
    out(0x20 + op, (o[9] ? 0x80 : 0) | (o[10] ? 0x40 : 0) | (o[5] ? 0x20 : 0) | (o[11] ? 0x10 : 0) | (o[1] & 15));
    out(0x60 + op, ((o[3] * 16) & 0xff) | (o[6] & 15));
    out(0x80 + op, (((15 - o[4]) & 15) * 16) | (o[7] & 15));
  }

  void loadInstrument(int s, uint8_t i) {
    const size_t base = 16 * static_cast<size_t>(lay_.instr) + ((lay_.area + 10 + 64 * i) & 0xffff);
    uint8_t b[28];
    for (int k = 0; k < 28; k++) b[k] = at(base + k);
    const uint8_t* op1 = b;
    const uint8_t* op2 = b + 13;
    Slot& sl = slot_[s];
    sl.loud1 = op1[8];
    sl.loud2 = op2[8];
    out(0xc0 + s, (op1[2] & 7) * 2);
    if (s <= 6) {
      const int m = kModOp[s], c = kCarOp[s];
      out(0x40 + m, ((op1[0] * 64) & 0xff) | ((63 - op1[8]) & 0xff));
      group(op1, m, b[26]);
      out(0x40 + c, ((op2[0] * 64) & 0xff) | level(sl.loud2, sl.volume));
      group(op2, c, b[27]);
    } else {
      const int c = kCarOp[s];
      out(0x40 + c, ((op1[0] * 64) & 0xff) | level(op1[8], sl.volume));
      group(op1, c, b[26]);
    }
  }

  void setNote(int s, uint8_t n) {
    Slot& sl = slot_[s];
    const uint16_t f = tableValue(static_cast<uint8_t>(n - 12));
    sl.freq = f;
    sl.fnum = f & 0x3ff;
    sl.octave = static_cast<uint8_t>(f >> 12);
    setFrequency(s, f);
  }

  void noteOn(int s, uint8_t n) {
    keyOff(s);
    if (s <= 6) setNote(s, n);
    else if (s == 8) setFrequency(7, tableValue(static_cast<uint8_t>(n + 7)));   // the tom
    keyOn(s);
  }

  static uint16_t pitch(const Slot& sl) {
    return static_cast<uint16_t>(sl.fnum | (((sl.octave * 16) & 0xff) << 8));
  }
  static void slideUp(Slot& sl, uint16_t u) {
    sl.fnum += u;
    while (sl.fnum >= 0x2ae) {
      sl.fnum = sl.fnum - 0x2ae + 0x157;
      sl.octave++;
    }
  }
  static void slideDown(Slot& sl, uint16_t u) {
    sl.fnum -= u;
    while (sl.fnum <= 0x142) {
      sl.fnum = 0x3cc - sl.fnum;
      sl.octave--;
    }
  }

  void readRow() {
    const uint8_t pattern = at(0x18 + order_);
    const size_t b = 16 * static_cast<size_t>(static_cast<uint16_t>(w(pattern) - lay_.first));
    auto byte = [&](uint32_t q) { return at(b + (q & 0xffff)); };
    uint32_t q = rowPos_;
    const uint16_t mask = static_cast<uint16_t>(byte(q) | byte(q + 1) << 8);
    q = (q + 2) & 0xffff;
    int bit = 15;
    auto next = [&]() { return bit >= 0 ? (mask >> bit--) & 1 : (bit--, 0); };
    for (int k = 0; k < channels_; k++) {
      if (next()) {
        for (int j = 0; j < 4; j++) slot_[k].cell[j] = byte(q + j);
        q = (q + 4) & 0xffff;
      } else {
        for (int j = 0; j < 4; j++) slot_[k].cell[j] = 0;
      }
    }
    // the drums: found by the file's count of melodic cells
    q = (rowPos_ + 2) & 0xffff;
    bit = 15;
    for (int k = 0; k < cells_ && k < 16; k++)
      if (next()) q = (q + 4) & 0xffff;
    for (int k = 6; k < kSlots; k++) {
      if (next()) {
        for (int j = 0; j < 4; j++) slot_[k].cell[j] = byte(q + j);
        q = (q + 4) & 0xffff;
      } else {
        for (int j = 0; j < 4; j++) slot_[k].cell[j] = 0;
      }
    }
    rowPos_ = q;
    row_++;
  }

  void rowEffect(int s) {
    Slot& sl = slot_[s];
    uint8_t& arg = sl.cell[3];
    switch (sl.cell[2] & 0x1f) {
      case 0x06:
        sl.cell[2] = 0;
        keyOff(s);
        break;
      case 0x0b:
        order_ = static_cast<uint8_t>(arg - 1);
        break_ = !break_;
        break;
      case 0x0c:
        if (arg >= 64) arg = 64;
        sl.volume = arg;
        setVolume(s, arg);
        break;
      case 0x0d:
        break_ = !break_;
        break;
      case 0x0f:
        if (arg & 31) {
          counter_ = 0;
          speed_ = arg & 31;
        }
        break;
      default:
        break;
    }
  }

  void rowWork(int s) {
    Slot& sl = slot_[s];
    const uint8_t note = sl.cell[0], ins = sl.cell[1];
    if (ins && note) {
      uint8_t v = at(0x99 + 2 * static_cast<uint8_t>(ins - 1));
      if (sl.cell[2] == 0x0c) {
        v = sl.cell[3];
        sl.cell[2] = sl.cell[3] = 0;
      }
      setVolume(s, v);
      if (ins != sl.loaded) {
        sl.loaded = ins;
        loadInstrument(s, static_cast<uint8_t>(ins - 1));
      }
    }
    if (note == 0) {
      rowEffect(s);
      return;
    }
    if (sl.cell[2] == 3) {   // slide to this note, from the one playing
      sl.portaNote = note;
      sl.target = tableValue(static_cast<uint8_t>(note - 12));
      sl.up = 0;
      if (sl.target == sl.freq) sl.target = 0;
      else if (sl.target > sl.freq) sl.up = 1;
      return;
    }
    rowEffect(s);
    noteOn(s, note);
  }

  void tickEffect(int s) {
    Slot& sl = slot_[s];
    uint8_t& arg = sl.cell[3];
    if (sl.cell[2] == 0 && arg == 0) return;
    switch (sl.cell[2] & 0x1f) {
      case 0x01:
        slideUp(sl, static_cast<uint16_t>(2 * arg));
        setFrequency(s, pitch(sl));
        break;
      case 0x02:
        slideDown(sl, static_cast<uint16_t>(2 * arg));
        setFrequency(s, pitch(sl));
        break;
      case 0x03:
        if (arg) {
          sl.portaSpeed = arg;
          arg = 0;
        }
        if (sl.target == 0) break;
        if (sl.up) slideUp(sl, static_cast<uint16_t>(2 * sl.portaSpeed));
        else slideDown(sl, static_cast<uint16_t>(2 * sl.portaSpeed));
        if (sl.up ? pitch(sl) < sl.target : pitch(sl) > sl.target) {
          setFrequency(s, pitch(sl));
        } else {
          sl.target = 0;
          setNote(s, sl.portaNote);
        }
        break;
      case 0x0a: {
        const int h = arg >> 4;
        int v = sl.volume;
        if (h) {
          v = (v + h) & 0xff;
          if (v > 64) v = 64;
        } else {
          v -= arg & 15;
          if (v < 0) v = 0;
        }
        sl.volume = static_cast<uint8_t>(v);
        setVolume(s, sl.volume);
        break;
      }
      default:
        break;
    }
  }

  void nextPattern() {
    counter_ = 0;
    break_ = false;
    row_ = 0;
    rowPos_ = lay_.area;
    order_++;
    if (order_ != end_) {
      if (seen_[order_]) looped_ = true;
      seen_[order_] = true;
    } else if (!loops_) {   // the end of a sound that does not loop
      on_ = false;
      for (int k = 0; k < channels_; k++) {
        keyOff(k);
        if (slot_[k].cache != 0) {
          slot_[k].cache = 0;
          const int c = 0x40 + kCarOp[k];
          out(c, (copy_[c] & 0xc0) | 0x3f);
        }
      }
    } else {
      order_ = start_;
      looped_ = true;
    }
  }

  const std::vector<uint8_t>& d_;
  const Layout lay_;
  Copl& opl_;
  OplLog* log_;
  uint8_t copy_[256] = {};
  Slot slot_[kSlots];
  int channels_ = 0, cells_ = 16;
  uint8_t speed_ = 6, counter_ = 0, order_ = 0, start_ = 0, end_ = 0, row_ = 0;
  uint32_t rowPos_ = 0;
  bool loops_ = false, break_ = false, on_ = false, looped_ = false;
  bool seen_[256] = {};
};

// Checks the module and finds its layout; false for a damaged file.
bool layout(const std::vector<uint8_t>& d, Layout& lay) {
  const size_t size = d.size();
  if (size < 0x168 || size > 0x40000) return false;
  lay.sounds = d[0x11b];
  if (lay.sounds < 1 || lay.sounds > 32 || d[0x118] > 6) return false;
  int most = 0;
  for (int i = 0; i < 127; i++)
    if (d[0x18 + i] > most) most = d[0x18 + i];
  lay.patterns = most + 1;
  lay.table = 0x158 + 16 * lay.sounds;
  if (lay.table + 2 * (lay.patterns + 1) > size) return false;
  auto w = [&](size_t k) { return static_cast<uint32_t>(d[lay.table + 2 * k] | d[lay.table + 2 * k + 1] << 8); };
  for (size_t k = 1; k <= lay.patterns; k++)
    if (w(k) < w(k - 1)) return false;
  if ((w(lay.patterns) - w(0)) * 16 + lay.table + ((2 * lay.patterns) & 0xfff0) + 16 > size) return false;
  for (size_t k = 0; k < lay.sounds; k++) {
    const size_t s = 0x158 + 16 * k;
    if (d[s + 13] != 0 && (d[s + 13] > 6 || d[s + 11] < d[s + 10])) return false;
  }
  lay.area = (lay.table + ((2 * lay.patterns) & 0xfff0) + 16) & 0xffff;
  lay.first = static_cast<uint16_t>(w(0));
  lay.instr = static_cast<uint16_t>(w(lay.patterns) - w(0));
  return true;
}

}  // namespace

int playCalCr(const char* path, int track, int rate, long long startMs) {
  const std::vector<uint8_t> d = readFile(path);
  Layout lay;
  if (!layout(d, lay)) return fail("not Shadowlands' music");
  std::vector<int> tunes;   // the sounds that are tunes
  for (size_t k = 0; k < lay.sounds; k++)
    if (d[0x158 + 16 * k + 13] != 0) tunes.push_back(static_cast<int>(k));
  if (tunes.empty()) return fail("no tune in the file");
  if (track < 0 || track >= static_cast<int>(tunes.size())) track = 0;
  const int sound = tunes[track];
  const size_t s = 0x158 + 16 * static_cast<size_t>(sound);

  long long ticks = 0;
  {
    Chip probe(8000);
    Cal p(d, lay, sound, probe, nullptr);
    const long long most = static_cast<long long>(1200 * p.hz());
    while (ticks < most && p.tick()) ticks++;
  }
  Info info;
  info.tracks = static_cast<int>(tunes.size());
  info.track = track;
  info.title = fromCp437(field(&d[0x003], 21));
  if (info.title.empty()) info.title = baseName(path);
  info.type = "Shadowlands (Ad Lib driver)";
  info.facts.emplace_back("tune", fromCp437(field(&d[s], 10)));
  info.facts.emplace_back("driver", "the game's ADLIB.OVR; played by a replayer written for this player");
  info.facts.emplace_back("timer", "50 Hz");
  if (d[s + 12]) info.facts.emplace_back("loops", "yes (played once)");
  for (size_t i = 0;; i++) {
    const size_t o = 16 * static_cast<size_t>(lay.instr) + lay.area + 64 * i;
    if (o + 40 > d.size()) break;
    info.names.push_back(fromCp437(field(&d[o], 10)));
  }
  info.durationMs = static_cast<long long>(ticks * 1000 / kCalHz);
  OplLog log("shadowlands", path, track, info, ticks, kCalHz, rate);
  Chip opl(rate);
  Cal song(d, lay, sound, opl, log.on() ? &log : nullptr);
  return play(song, opl, info, rate, startMs);
}

}  // namespace adlib
