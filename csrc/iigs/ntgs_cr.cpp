// SPDX-License-Identifier: GPL-3.0-or-later
// Noise Tracker GS modules of the Apple IIGS ("FTA MODULEFILE").
//
// Written for this player in a clean room, from a description of the format only.
//
// A module holds up to 15 tracks of 64-row blocks, a position list, and two kinds of
// instruments: "Ensoniq" instruments, copied into the DOC's wave memory once at the start
// and played by a pair of oscillators each, and "memory" instruments, which stay in the
// module and are fed into the wave memory while they play, through a buffer of two halves
// per voice that four oscillators play in turn (swap mode). Every interrupt of oscillator
// 30, the clock, is one row; the tempo is the clock's frequency.

#include "iigs_cr.h"
#include "iigs_cr_tables.h"

#include <algorithm>

namespace {

using namespace iigscr;

constexpr unsigned kBlocks = 606;   // where the blocks start
constexpr int kVoices = 8;

struct Instrument {
  unsigned size, at, volume, page, left, right, stereo;
  unsigned start;   // absolute start page in the module
};

class NoiseTracker : public Song {
 public:
  explicit NoiseTracker(const Bytes& file) : f_(file) {
    tempo_ = word_at(f_, 16) & 0xff;
    raw_ = word_at(f_, 20);
    npos_ = word_at(f_, 22);
    nblocks_ = word_at(f_, 24);
    nE_ = word_at(f_, 26);
    nR_ = word_at(f_, 28);
    big_ = word_at(f_, 30) != 0;
    rpages_ = word_at(f_, 34);
    fed_ = word_at(f_, 36);
    bsize_ = word_at(f_, 38);
    used_ = word_at(f_, 40);
    music_ = word_at(f_, 42) | (uint64_t(word_at(f_, 44)) << 16);
    for (int t = 0; t < 15; ++t) trackStereo_[t] = word_at(f_, 64 + 2 * t);
    const uint64_t records = kBlocks + music_;
    const uint64_t past = records + 32 * uint64_t(nR_ + nE_);
    const uint64_t p0 = (past + 255) / 256;
    for (unsigned i = 0; i < nR_ + nE_; ++i) {
      const uint64_t r = records + 32 * uint64_t(i);
      Instrument& in = (i < nR_ ? mem_[i] : ens_[i - nR_]);
      in.size = word_at(f_, r + 17);
      in.at = word_at(f_, r + 19);
      in.volume = byte_at(f_, r + 21);
      in.page = word_at(f_, r + 23);
      in.left = word_at(f_, r + 25);
      in.right = word_at(f_, r + 27);
      in.stereo = word_at(f_, r + 29);
      in.start = (in.at + p0 + (i < nR_ ? 0 : rpages_)) & 0xffff;
      std::string s;
      for (int j = 0; j < 13; ++j) {
        const unsigned c = byte_at(f_, r + j) & 0x7f;
        s += static_cast<char>(c < 0x20 ? ' ' : c);
      }
      while (!s.empty() && s.back() == ' ') s.pop_back();
      names.push_back(s);
    }
    type = "NoiseTracker GS";
    half_ = big_ ? 8 : 4;
    hsr_ = big_ ? 0x1b : 0x12;
    for (int u = 0; u < kVoices; ++u) base_[u] = ((big_ ? 0xf0 : 0xf8) - 2 * half_ * u) & 0xff;
  }

 protected:
  void begin() override {
    for (int o = 0; o < 32; ++o) wr(0xa0 + o, 0x03);
    for (unsigned i = 0; i < nE_; ++i) {
      const Instrument& in = ens_[i];
      for (unsigned p = 0; p < in.size; ++p) copy_page(in.start + p, (in.page & 0xff) + p);
    }
    wr(0xe1, 0x3c);
    wr(0x1e, tempo_);
    wr(0x3e, 0);
    wr(0x5e, 0);
    wr(0x9e, 0);
    wr(0xde, 0);
    wr(0xbe, 0x08);
    for (int i = 0; i < 32; ++i) { nowMem_[i] = mem_[i].volume; nowEns_[i] = ens_[i].volume; }
    next_block();
  }

  void interrupt(int o) override {
    if (o == kClockOsc) {
      if (!ended_) row();
      return;
    }
    if ((rd(0xa0 + (o ^ 1)) & 6) == 2) { last_round(o); return; }
    const int u = (o & 0x1e) / 4;
    if ((o & 2) || u >= kVoices) return;
    Voice& v = voice_[u];
    if (v.state >= 2) return;
    v.state = 1;
    v.to = ((o & 1 ? half_ : 0) ^ v.first) & 0xffff;
    const unsigned lft = v.left;
    v.left = (v.left - half_) & 0xffff;
    if (lft >= half_ + 1) { v.pages = half_; return; }
    v.pages = lft;
    last_round(o);
  }

  void after_sample() override {
    if (stalled_ && !ended_) {
      if (++stall_ > 2 * doc_->rate()) { end_song(); return; }
    }
    if (raw_) {
      for (int u = kVoices - 1; u >= 0; --u) {
        if (voice_[u].state) {
          feed(u);
          if (voice_[u].state == 0xfffe) start_voice(u);
        }
      }
    } else {
      for (int u = kVoices - 1; u >= 0; --u)
        if (voice_[u].state) feed(u);
      for (int u = kVoices - 1; u >= 0; --u)
        if (voice_[u].state == 0xfffe) start_voice(u);
    }
  }

 private:
  struct Voice {
    unsigned state = 0, to = 0, first = 0, from = 0, loopFrom = 0, loopAt = 0;
    unsigned freq = 0, vol = 0, stereo = 0, pages = 0, left = 0;
  };

  // A page of the module into a page of the wave memory.
  void copy_page(uint64_t from, unsigned to) {
    if (log) log->page(to, from * 256);
    const uint64_t at = from * 256;
    uint8_t* dst = doc_->ram + ((to & 0xff) << 8);
    for (int i = 0; i < 256; ++i) dst[i] = static_cast<uint8_t>(byte_at(f_, at + i));
  }

  void end_song() {
    for (int o = 30; o >= 0; --o) wr(0xa0 + o, 0x03);
    ended_ = true;
  }

  void next_block() {
    if (posi_ >= npos_) { end_song(); return; }
    const unsigned q = word_at(f_, 94 + 2 * posi_);
    ++posi_;
    cell_ = kBlocks + (q < nblocks_ && q < 72 ? uint64_t(q) * bsize_ : 0);
    row_ = 0;
  }

  void row() {
    for (unsigned t = 0; t < used_; ++t) {
      track(t, cell_);
      cell_ += 3;
    }
    if (++row_ >= 64) next_block();
  }

  void set_tempo(int x) {
    wr(0x1e, x);
    stalled_ = x == 0;
    stall_ = 0;
  }

  void track(unsigned t, uint64_t at) {
    const int n = byte_at(f_, at) & 0x7f;
    const int s = byte_at(f_, at + 1);
    const int e = s & 0xc0;
    const int x = byte_at(f_, at + 2);
    const bool fed = t < fed_;
    const int first = fed ? 4 * t : 2 * (t - fed_) + 4 * fed_;
    const int count = fed ? 4 : 2;
    Instrument* in = nullptr;
    unsigned* now = nullptr;
    if (ti_[t]) {
      const unsigned i = ti_[t] - 1;
      if (fed && i < nR_) { in = &mem_[i]; now = &nowMem_[i]; }
      if (!fed && i < nE_) { in = &ens_[i]; now = &nowEns_[i]; }
    }
    if (n == 0) {
      if (e == 0x80) set_tempo(x);
      if (e == 0x40 && in) {
        *now = x;
        for (int k = 0; k < count; ++k) wr(0x40 + first + k, kVolume[*now / 2]);
      }
      return;
    }
    if (n == 0x54) { row_ = 63; return; }
    if (n == 0x55) {
      for (int k = 0; k < count; ++k) wr(0xa0 + first + k, 0x03);
      return;
    }
    if (s & 0x3f) ti_[t] = s & 0x3f;
    if (!ti_[t]) return;
    if (e == 0x80) set_tempo(x);
    // The instrument may have changed with this cell.
    in = nullptr;
    {
      const unsigned i = ti_[t] - 1;
      if (fed && i < nR_) { in = &mem_[i]; now = &nowMem_[i]; }
      if (!fed && i < nE_) { in = &ens_[i]; now = &nowEns_[i]; }
    }
    if (!in) return;
    *now = e == 0x40 ? unsigned(x) : in->volume;
    const unsigned freq = n < 108 ? kNoteFreq[n] : 0;
    if (fed) { prepare(t, *in, *now, freq); return; }
    const unsigned ts = trackStereo_[t];
    const unsigned sA = in->stereo & 0x80 ? ts : in->stereo;
    const unsigned sB = in->stereo & 0x8000 ? ts : in->stereo;
    const int vol = kVolume[*now / 2];
    const int second = first + 1;
    wr(0xa0 + first, 0x03); wr(0xa0 + second, 0x03);
    wr(0x00 + first, freq & 0xff); wr(0x00 + second, freq & 0xff);
    wr(0x20 + first, freq >> 8); wr(0x20 + second, freq >> 8);
    wr(0x40 + first, vol); wr(0x40 + second, vol);
    wr(0x80 + first, in->page & 0xff); wr(0x80 + second, in->page & 0xff);
    wr(0xc0 + first, in->left & 0xff);
    wr(0xc0 + second, in->right & 0xff);
    wr(0xa0 + first, ((sA & 0xff) | (in->left >> 8)) & 0xff);
    wr(0xa0 + second, ((sB >> 8) | (in->right >> 8)) & 0xff);
  }

  // A memory instrument's note: the first half of its buffer is filled, page by page after
  // the samples that follow, and then the voice starts.
  void prepare(unsigned t, const Instrument& in, unsigned now, unsigned freq) {
    int last = 0;
    for (int k = 0; k < 4; ++k) {
      last = rd(0xa0 + 4 * t + k) & 0xf3;
      wr(0xa0 + 4 * t + k, last);
    }
    const int pl = rd(0x80 + ((2 * t + (last & 1 ? 0 : 1)) & 31));
    Voice& v = voice_[t];
    v.first = v.to = (((pl & half_) ^ half_) + base_[t]) & 0xffff;
    v.freq = freq;
    v.from = in.start;
    v.loopFrom = (in.start + in.right) & 0xffff;
    v.loopAt = (in.start + in.size) & 0xffff;
    v.left = in.left ? 0xffff : in.size;
    v.pages = std::min(v.left, half_);
    v.vol = now / 2;
    v.stereo = in.stereo & 0x8000 ? trackStereo_[t] : in.stereo;
    v.state = 0xffff;
  }

  void feed(int u) {
    Voice& v = voice_[u];
    copy_page(v.from, v.to & 0xff);
    v.to = (v.to + 1) & 0xffff;
    v.from = (v.from + 1) & 0xffff;
    if (v.from == v.loopAt) v.from = v.loopFrom;
    v.pages = (v.pages - 1) & 0xffff;
    if (v.pages) return;
    v.state = v.state == 2 ? 0 : (v.state - 1) & 0xffff;
  }

  void start_voice(int u) {
    Voice& v = voice_[u];
    const int o = 4 * u;
    const int sa = v.stereo & 0xff, sb = v.stereo >> 8;
    const int h1 = v.first & 0xff, h2 = (v.first ^ half_) & 0xff;
    for (int k = 0; k < 4; ++k) wr(0xa0 + o + k, 0x03);
    for (int k = 0; k < 4; ++k) wr(0x00 + o + k, v.freq & 0xff);
    for (int k = 0; k < 4; ++k) wr(0x20 + o + k, v.freq >> 8);
    for (int k = 0; k < 4; ++k) wr(0x40 + o + k, kVolume[v.vol]);
    if (v.left < half_ + 1) {
      wr(0x80 + o, h1);
      wr(0x81 + o, h1);
      wr(0xc0 + o, hsr_);
      wr(0xc1 + o, hsr_);
      wr(0xa0 + o, sa | 2);
      wr(0xa1 + o, sb | 2);
      v.state = 0;
      return;
    }
    for (int k = 0; k < 4; ++k) wr(0xc0 + o + k, hsr_);
    wr(0x80 + o, h1);
    wr(0x82 + o, h1);
    wr(0x81 + o, h2);
    wr(0x83 + o, h2);
    if (v.left < 2 * half_ + 1) {
      wr(0xa0 + o, sa | 6);
      wr(0xa1 + o, sa | 3);
      wr(0xa2 + o, sb | 6);
      wr(0xa3 + o, sb | 3);
      v.pages = v.left - half_;
    } else {
      v.left -= 2 * half_;
      wr(0xa0 + o, sa | 0x0e);
      wr(0xa1 + o, sa | 0x0f);
      wr(0xa2 + o, sb | 6);
      wr(0xa3 + o, sb | 7);
      v.pages = half_;
    }
    v.to = h2;
    v.state = 2;
  }

  void last_round(int o) {
    wr(0xa0 + o, rd(0xa0 + o) & 0xf3);
    const int p = (o + 2) & 31;
    wr(0xa0 + p, rd(0xa0 + p) & 0xf3);
  }

  const Bytes f_;
  unsigned tempo_, raw_, npos_, nblocks_, nE_, nR_, rpages_, fed_, bsize_, used_;
  bool big_;
  uint64_t music_;
  unsigned trackStereo_[15];
  Instrument mem_[32] = {}, ens_[32] = {};
  unsigned nowMem_[32] = {}, nowEns_[32] = {};
  unsigned half_, hsr_, base_[kVoices];

  Voice voice_[kVoices];
  unsigned ti_[15] = {};
  unsigned posi_ = 0, row_ = 0;
  uint64_t cell_ = kBlocks;
  bool stalled_ = false;
  long long stall_ = 0;
};

}  // namespace

Song* load_noisetracker(const std::string&, const Bytes& f, std::string& why) {
  static const char kMagic[] = "FTA MODULEFILE";
  if (f.size() < 16 || !std::equal(kMagic, kMagic + 14, f.begin() + 2)) {
    why = "not a NoiseTracker GS module";
    return nullptr;
  }
  if (f.size() < kBlocks) { why = "the module is cut off in its header"; return nullptr; }
  const unsigned npos = word_at(f, 22), nE = word_at(f, 26), nR = word_at(f, 28);
  const unsigned fed = word_at(f, 36), bsize = word_at(f, 38), used = word_at(f, 40);
  if (!npos || npos > 256 || !used || used > 15 || fed > 7 || fed > used || nE > 32 || nR > 32 ||
      bsize != 192 * used) {
    why = "a NoiseTracker GS module this player doesn't understand";
    return nullptr;
  }
  if (!(word_at(f, 16) & 0xff)) { why = "the module has no tempo"; return nullptr; }
  const uint64_t music = word_at(f, 42) | (uint64_t(word_at(f, 44)) << 16);
  if (f.size() < kBlocks + music + 32 * uint64_t(nE + nR)) {
    why = "the module is cut off before its instruments";
    return nullptr;
  }
  return new NoiseTracker(f);
}
