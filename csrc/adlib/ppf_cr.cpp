// SPDX-License-Identifier: GPL-3.0-or-later
// Pixel Painters' music: .fmf (Electranoid, Laser Light) and .ftf (Xatax), as the games'
// own timer routines play it on a chip set up by Creative's FM library. Written for this
// player in a clean room, from a description of the formats only.
//
// FMF: one instrument and velocity per channel, then per channel a list of events, each a
// length and the two frequency registers to write (both 0 for a rest). FTF: a song length,
// instruments, and per channel a list of events with a note, a level, a panning, an
// instrument and a length; Xatax played them on a Sound Blaster Pro 2, so the chip is put in
// OPL3 mode and the panning sounds.
//
// The test mode (BBMP_ADLIB_LOG, common.h) lists every chip write as it is made.

#include "common.h"

namespace adlib {

namespace {

constexpr double kFmfHz = 1193182.0 / 1171 / 56;
constexpr double kFtfHz = 1193182.0 / 2342 / 31;

constexpr uint8_t kModOp[9] = {0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12};
constexpr uint8_t kCarOp[9] = {0x03, 0x04, 0x05, 0x0b, 0x0c, 0x0d, 0x13, 0x14, 0x15};

// What both formats share: the chip, its set-up and the instrument layout.
class Pp : public Song {
 protected:
  Pp(const std::vector<uint8_t>& d, Copl& opl, OplLog* log) : d_(d), opl_(opl), log_(log) {}

  void out(int reg, int val) {
    opl_.write(reg & 0xff, val & 0xff);
    if (log_) log_->write(reg, val);
  }

  void setUp() {
    opl_.init();
    out(0x01, 0x20);
    out(0x08, 0x40);
    out(0xbd, 0xc0);
    for (int v = 0; v < 9; v++) {   // the library's default instrument
      out(0x20 + kModOp[v], 0x01);
      out(0x20 + kCarOp[v], 0x11);
      out(0x40 + kModOp[v], 0x4f);
      out(0x40 + kCarOp[v], 0x00);
      out(0x60 + kModOp[v], 0xf1);
      out(0x60 + kCarOp[v], 0xf2);
      out(0x80 + kModOp[v], 0x53);
      out(0x80 + kCarOp[v], 0x74);
    }
  }

  void load(int v, size_t at, int extra) {
    const uint8_t* b = &d_[at];
    const int mod = kModOp[v], car = kCarOp[v];
    out(0x20 + mod, b[0]);
    out(0x20 + car, b[1]);
    out(0x40 + mod, b[2]);
    out(0x40 + car, b[3]);
    out(0x60 + mod, b[4]);
    out(0x60 + car, b[5]);
    out(0x80 + mod, b[6]);
    out(0x80 + car, b[7]);
    out(0xe0 + mod, b[8]);
    out(0xe0 + car, b[9]);
    out(0xc0 + v, b[10] | extra);
  }

  const std::vector<uint8_t>& d_;
  Copl& opl_;
  OplLog* log_;
};

// ---- FMF ----

struct FmfLayout {
  int channels = 0;
  size_t events[9] = {};   // where each channel's events start
  size_t count[9] = {};    // and how many there are
};

bool fmfLayout(const std::vector<uint8_t>& d, FmfLayout& l) {
  const size_t size = d.size();
  if (size < 2) return false;
  const size_t n = d[0] | d[1] << 8;
  if (n < 1 || n > 9 || 2 + 17 * n > size) return false;
  size_t at = 2 + 17 * n;
  for (size_t c = 0; c < n; c++) {
    if (at + 2 > size) return false;
    const size_t len = d[at] | d[at + 1] << 8;
    if (len % 4 != 0 || at + 2 + len > size) return false;
    l.events[c] = at + 2;
    l.count[c] = len / 4;
    at += 2 + len;
  }
  if (at != size || l.count[0] == 0) return false;
  l.channels = static_cast<int>(n);
  return true;
}

class Fmf : public Pp {
 public:
  Fmf(const std::vector<uint8_t>& d, const FmfLayout& l, Copl& opl, OplLog* log) : Pp(d, opl, log), l_(l) {
    setUp();
    for (int c = 0; c < l.channels; c++) {
      const int w = d[2 + 16 * l.channels + c];
      if (w == 0) continue;   // the channel keeps the default instrument
      const size_t ins = 2 + 16 * c;
      load(c, ins, 0);
      const int t = d[ins + 3];
      const int loud = (63 - (t & 63)) & 0xff;
      const int s = (loud * (w | 0x80)) >> 8;
      out(0x40 + kCarOp[c], ((63 - s) & 63) | (t & 0xc0));
      out(0xa0 + c, 0x57);
      out(0xb0 + c, 0x01);
    }
  }

  static long long length(const std::vector<uint8_t>& d, const FmfLayout& l) {
    long long n = 1;
    for (size_t e = 0; e < l.count[0]; e++) n += d[l.events[0] + 4 * e];
    return n;
  }

  double hz() const override { return kFmfHz; }

  bool tick() override {
    if (log_) log_->tickStart();
    const bool goesOn = step();
    if (log_) log_->tickEnd(goesOn);
    return goesOn;
  }

 private:
  const uint8_t* event(int c, size_t e) const { return &d_[l_.events[c] + 4 * e]; }

  bool step() {
    const size_t total = l_.count[0];
    if (pos_[0] > total) return false;   // the song has ended
    const bool last = pos_[0] == total;
    for (int c = 0; c < l_.channels; c++) {
      if (wait_[c] != 0) {
        if (--wait_[c] != 0) continue;
        if (pos_[c] > 0 && pos_[c] <= l_.count[c]) {
          const uint8_t* prev = event(c, pos_[c] - 1);
          if (prev[2] || prev[3]) out(0xb0 + c, prev[3] & 0x1f);   // key off
        }
        if (last) {
          wait_[c] = 1;
          pos_[c]++;
          continue;
        }
      }
      if (pos_[c] >= l_.count[c]) {
        wait_[c] = 65535;
        continue;
      }
      const uint8_t* e = event(c, pos_[c]);
      if (e[2] || e[3]) {
        out(0xa0 + c, e[2]);
        out(0xb0 + c, e[3] | 0x20);
      }
      wait_[c] = e[0];
      pos_[c]++;
    }
    return true;
  }

  const FmfLayout& l_;
  size_t pos_[9] = {};
  unsigned wait_[9] = {};
};

// ---- FTF ----

struct FtfLayout {
  int instruments = 0, channels = 0, ticks = 0;
  size_t events[9] = {}, count[9] = {};
};

bool ftfLayout(const std::vector<uint8_t>& d, FtfLayout& l) {
  const size_t size = d.size();
  if (size < 0x24) return false;
  const size_t ni = d[0], nc = d[1], total = d[2] | d[3] << 8;
  if (nc < 1 || nc > 9 || ni < 1 || total < 1) return false;
  const size_t first = 0x20 + 16 * ni;
  if (first > size) return false;
  for (size_t c = 0; c < nc; c++) {
    const size_t a = d[4 + 2 * c] | d[5 + 2 * c] << 8;
    const size_t z = c + 1 < nc ? static_cast<size_t>(d[6 + 2 * c] | d[7 + 2 * c] << 8) : size;
    if (a < first || a > z || z > size || (z - a) % 4 != 0) return false;
    size_t sum = 0;
    for (size_t e = a; e < z; e += 4) {
      if (d[e + 2] >= ni) return false;
      sum += d[e + 3];
    }
    if (sum != total) return false;
    l.events[c] = a;
    l.count[c] = (z - a) / 4;
  }
  l.instruments = static_cast<int>(ni);
  l.channels = static_cast<int>(nc);
  l.ticks = static_cast<int>(total);
  return true;
}

// F-numbers from C (the top of the block below) to B
constexpr uint16_t kFtfF[12] = {0x2ae, 0x16b, 0x181, 0x198, 0x1b0, 0x1ca,
                                0x1e5, 0x202, 0x220, 0x241, 0x263, 0x287};

int ftfFreq(int k) {
  if (k < 25 || k > 119) return 0;
  const int s = k % 12;
  const int block = k / 12 - (s == 0 ? 3 : 2);
  return kFtfF[s] | block * 1024;
}

class Ftf : public Pp {
 public:
  Ftf(const std::vector<uint8_t>& d, const FtfLayout& l, Copl& opl, OplLog* log) : Pp(d, opl, log), l_(l) {
    setUp();
    // OPL3 mode, so that the panning works as on a Sound Blaster Pro 2
    opl.setchip(1);
    opl.write(0x05, 0x01);
    opl.setchip(0);
    if (log_) log_->line("105 01");
    for (int c = 0; c < l.channels; c++) {
      ch_[c].active = true;
      ch_[c].total = static_cast<uint16_t>(l.ticks);
      ch_[c].wait = 1;
    }
  }

  double hz() const override { return kFtfHz; }

  bool tick() override {
    if (log_) log_->tickStart();
    bool goesOn = false;
    for (int c = 0; c < l_.channels; c++)
      if (ch_[c].active) {
        goesOn = true;
        channel(c);
      }
    if (log_) log_->tickEnd(goesOn);
    return goesOn;
  }

 private:
  struct Channel {
    bool active = false, keyed = false;
    uint16_t total = 0, wait = 0;
    size_t pos = 0;
    int instrument = 0xff, note = 0, level = 63, panning = 0;
    uint8_t loud = 0, c0 = 0;
  };

  void instrument(int v, int i) {
    Channel& k = ch_[v];
    const size_t at = 0x20 + 16 * static_cast<size_t>(i);
    k.loud = static_cast<uint8_t>(63 - d_[at + 3]);
    k.c0 = d_[at + 10];
    load(v, at, (x_ & 3) * 16);
    k.keyed = false;
    k.level = 63;
    k.panning = x_ & 3;
  }

  void level(int v, int l) {
    Channel& k = ch_[v];
    k.level = l;
    out(0x40 + kCarOp[v], (((((k.loud & 63) * (64 - l)) * 4) >> 8) & 63) | (k.loud & 0xc0));
  }

  void panning(int v, int p) {
    Channel& k = ch_[v];
    k.panning = p & 3;
    out(0xc0 + v, k.c0 | k.panning * 16);
  }

  void keyOff(int v) {
    Channel& k = ch_[v];
    if (!k.keyed) return;
    out(0xb0 + v, (ftfFreq(k.note) >> 8) & 0x1f);
    k.keyed = false;
  }

  void noteOn(int v, int note, int l, int p) {
    Channel& k = ch_[v];
    if (k.keyed) keyOff(v);
    if (p != k.panning) panning(v, p);
    if (l != k.level) level(v, l);
    k.note = note;
    k.keyed = true;
    const int f = ftfFreq(note);
    out(0xa0 + v, f & 0xff);
    out(0xb0 + v, (f >> 8) | 0x20);
  }

  void channel(int v) {
    Channel& k = ch_[v];
    if (--k.total == 0) {
      keyOff(v);
      k.active = false;
      return;
    }
    if (--k.wait != 0) return;
    if (k.pos >= l_.count[v]) {
      k.wait = 65535;
      return;
    }
    const uint8_t* e = &d_[l_.events[v] + 4 * k.pos];
    if (e[2] != k.instrument) {
      k.instrument = e[2];
      instrument(v, e[2]);
    }
    k.wait = e[3];
    const int p = e[1] >> 6, l = e[1] & 63;
    x_ = l;
    const int note = e[0] & 0x7f;
    if (note == 0) {
      if (e[0] & 0x80) {   // a hold: only the level and the panning change
        if (l != k.level) level(v, l);
        if (p != k.panning) panning(v, p);
      } else {
        keyOff(v);
      }
    } else {
      noteOn(v, note, l, p);
    }
    k.pos++;
  }

  const FtfLayout& l_;
  Channel ch_[9];
  int x_ = 0;   // the level of the last event read: also the panning of an instrument change
};

}  // namespace

int playPpfCr(const char* path, int track, int rate, long long startMs) {
  (void)track;   // one song per file
  const std::vector<uint8_t> d = readFile(path);
  Info info;
  info.title = baseName(path);
  FmfLayout fmf;
  FtfLayout ftf;
  if (fmfLayout(d, fmf)) {
    const long long n = Fmf::length(d, fmf);
    info.type = "Pixel Painters FMF";
    info.facts.emplace_back("channels", std::to_string(fmf.channels));
    info.facts.emplace_back("timer", "18.2 Hz");
    info.durationMs = static_cast<long long>(n * 1000 / kFmfHz);
    OplLog log("pixelpainters", path, 0, info, n, kFmfHz, rate);
    Chip opl(rate);
    Fmf song(d, fmf, opl, log.on() ? &log : nullptr);
    return play(song, opl, info, rate, startMs);
  }
  if (ftfLayout(d, ftf)) {
    info.type = "Pixel Painters FTF";
    info.facts.emplace_back("channels", std::to_string(ftf.channels));
    info.facts.emplace_back("instruments", std::to_string(ftf.instruments));
    info.facts.emplace_back("timer", "16.4 Hz");
    info.durationMs = static_cast<long long>(ftf.ticks * 1000LL / kFtfHz);
    OplLog log("pixelpainters", path, 0, info, ftf.ticks, kFtfHz, rate);
    Chip opl(rate);
    Ftf song(d, ftf, opl, log.on() ? &log : nullptr);
    return play(song, opl, info, rate, startMs);
  }
  return fail("not a Pixel Painters FMF or FTF song");
}

}  // namespace adlib
