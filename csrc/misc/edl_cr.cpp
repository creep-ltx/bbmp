// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-misc's engine "edlib": the song files of JCH's EdLib (.edl, Modland's "EdLib
// Packed"), the AdLib music editor's memory written out and packed.
//
// EdLib's own players only play the D00 files the editor exports. This engine unpacks a
// song file (PKWARE's "implode", with zlib's blast), turns the song into that D00 file and
// has AdPlug's D00 player play it on its Nuked OPL3. Written for this player in a clean
// room, from a description of the format only.
//
// An unpacked song is three blocks of 0xFF00 bytes: the tracks of the nine voices, the
// sequences, and a block holding the header, the instruments and the SpFX table. The D00
// gets the header, the tracks (repeated transpositions dropped), the track pointers and
// levels, the SpFX table, the sequences (runs of rest and hold rows folded into one word),
// the instruments in use (numbered again from 1) and an end mark.
//
// With BBMP_EDL_D00=<file> the D00 is also written to that file, right after it is made.
//
// A few damaged songs give a D00 on which AdPlug's player would loop for ever inside one
// tick (a voice whose order list comes back round without a single note, rest or cut in
// between). Before each tick such a voice is looked for; if one is found, the song ends
// there instead. BBMP_EDL_UNGUARDED=1 leaves that check out (for comparisons).

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <adplug/d00.h>
#include <adplug/fprovide.h>
#include <adplug/nemuopl.h>
#include <binstr.h>

extern "C" {
#include "blast.h"
}

#include "common.h"

namespace misc {

namespace {

const unsigned kBlock = 0xFF00;              // one block of the unpacked song
const unsigned kUnpacked = 3 * kBlock;       // the whole unpacked song
const unsigned kTrackBytes = 0xFFFF / 9 - 1; // a voice's region in the track block (7280)
const unsigned kSeqSlot = 0x180;             // bytes per sequence slot
const unsigned kLimit = 0xF000;              // the D00 may grow to here, a little past at most
const uint8_t kId[6] = {0x4A, 0x43, 0x48, 0x26, 0x02, 0x66};

// The speed of songs older than version 4, by their speed word 0..10.
const uint16_t kOldSpeed[11] = {0x40, 0x36, 0x28, 0x20, 0x1A, 0x15, 0x13, 0x10, 0x0E, 0x0D, 0x0C};

// A 64 KB area whose addresses wrap.
struct Area {
  std::vector<uint8_t> m = std::vector<uint8_t>(0x10000, 0);
  uint8_t byte(unsigned a) const { return m[a & 0xFFFF]; }
  void setByte(unsigned a, uint8_t v) { m[a & 0xFFFF] = v; }
  unsigned word(unsigned a) const { return m[a & 0xFFFF] | m[(a + 1) & 0xFFFF] << 8; }
  void setWord(unsigned a, unsigned v) {
    m[a & 0xFFFF] = v & 0xFF;
    m[(a + 1) & 0xFFFF] = (v >> 8) & 0xFF;
  }
};

struct Damaged {};

// ------------------------------------------------------------------- unpacking

struct Input {
  const std::vector<uint8_t>* file;
  bool given;
};

unsigned feed(void* how, unsigned char** buf) {
  Input* in = static_cast<Input*>(how);
  if (in->given) return 0;
  in->given = true;
  *buf = const_cast<unsigned char*>(in->file->data());
  return static_cast<unsigned>(in->file->size());
}

int collect(void* how, unsigned char* buf, unsigned len) {
  std::vector<uint8_t>* out = static_cast<std::vector<uint8_t>*>(how);
  if (out->size() + len > kUnpacked) return 1;
  out->insert(out->end(), buf, buf + len);
  return 0;
}

// The unpacked song, or an empty text and the reason.
std::string unpack(const std::vector<uint8_t>& file, std::vector<uint8_t>& song) {
  if (file.size() == kUnpacked && std::memcmp(&file[2 * kBlock], kId, 6) == 0) {
    song = file;
    return "";
  }
  if (file.size() < 8 || file[0] != 0 || file[1] < 4 || file[1] > 6)
    return "not an EdLib song: it isn't packed the way EdLib packs";
  Input in{&file, false};
  song.clear();
  song.reserve(kUnpacked);
  if (blast(feed, &in, collect, &song, nullptr, nullptr) != 0 || song.size() != kUnpacked)
    return "not an EdLib song: unpacking fails";
  return "";
}

// ------------------------------------------------------------------ the conversion

class Converter {
 public:
  Converter(const std::vector<uint8_t>& song) {
    std::memcpy(t_.m.data(), &song[0], kBlock);
    std::memcpy(s_.m.data(), &song[kBlock], kBlock);
    std::memcpy(i_.m.data(), &song[2 * kBlock], kBlock);
  }
  const Area& block3() const { return i_; }

  // The D00; throws Damaged.
  std::vector<uint8_t> run() {
    speed();
    copyHeader();
    tracks();
    pointersAndSpfx();
    sequences();
    instruments();
    d_.setWord(0x71, d);
    put(0xFFFF);
    return std::vector<uint8_t>(d_.m.begin(), d_.m.begin() + d);
  }

 private:
  Area t_, s_, i_, d_;
  unsigned d = 0;                 // where the next byte of the D00 goes
  unsigned trackAt_[9] = {};
  unsigned sequenceCount_ = 0;
  unsigned seqTable_ = 0;         // the D00 offset of the sequence pointers

  void put(unsigned w) {
    d_.setWord(d, w);
    d += 2;
  }

  // All voices get voice 0's speed; old songs have theirs translated.
  void speed() {
    unsigned sp = t_.word(2);
    if (i_.byte(7) < 4) {
      if (sp >= 11) throw Damaged();
      sp = kOldSpeed[sp];
      i_.setByte(7, 4);
    }
    for (unsigned v = 0; v < 9; v++) t_.setWord(2 + kTrackBytes * v, sp);
  }

  // The header ends at the first FF FF followed by a zero word.
  void copyHeader() {
    unsigned pos = 0, h = 0;
    for (;;) {
      while (pos < 0xFFFC && i_.byte(pos) != 0xFF) pos++;
      if (pos >= 0xFFFC) throw Damaged();
      pos++;
      if (i_.byte(pos) != 0xFF) {
        pos++;
        continue;
      }
      pos++;
      const unsigned w = i_.word(pos);
      pos += 2;
      if (w == 0) {
        h = pos - 2;
        break;
      }
    }
    if (h < 0x77) throw Damaged();
    for (unsigned a = 0; a < h; a++) d_.setByte(a, i_.byte(a));
    d = h;
  }

  void tracks() {
    unsigned highest = 0, loopBytes = 0;
    for (unsigned v = 0; v < 9; v++) {
      const unsigned r = 2 + kTrackBytes * v, e = r + 2;
      trackAt_[v] = d;
      put(t_.word(r));
      // where the track ends, and where it loops to
      unsigned target = 0xFFFF;
      for (unsigned pos = e;; pos += 2) {
        if (pos >= e + kTrackBytes) throw Damaged();
        const unsigned w = t_.word(pos);
        if (w == 0xFFFE) break;
        if (w == 0xFFFF) {
          target = t_.word(pos + 2);
          loopBytes = (2 * target) & 0xFFFF;
          break;
        }
      }
      unsigned last = 0xFFFF;
      for (unsigned pos = e;;) {
        if (d > kLimit) throw Damaged();
        const unsigned w = t_.word(pos);
        pos += 2;
        const unsigned j = pos - e;
        if (w <= 0x7FFE) {
          put(w);
          if (w > highest) highest = w;
        } else if (w == 0xFFFE) {
          put(0xFFFE);
          break;
        } else if (w == 0xFFFF) {
          if (j == 2) {           // loops to itself at once: it stops
            put(0xFFFE);
          } else {
            put(0xFFFF);
            put(target);
          }
          break;
        } else if (w != last) {   // a transposition
          put(w);
          last = w;
        } else if (j <= loopBytes) {
          target = (target - 1) & 0xFFFF;   // a dropped entry before the loop target
        }
      }
    }
    sequenceCount_ = highest + 1;
    if (sequenceCount_ * kSeqSlot > kBlock) throw Damaged();
  }

  void pointersAndSpfx() {
    d_.setWord(0x6B, d);
    for (unsigned v = 0; v < 9; v++) put(trackAt_[v]);
    for (unsigned v = 0; v < 9; v++) d_.setByte(d++, i_.byte(0x8B + v));
    d += 5;
    put(0xFFFF);
    const unsigned fx = i_.word(0x73);
    unsigned length = 0;
    for (unsigned k = 0x3FFF; k >= 1; k--) {
      if (i_.byte(fx + k) != 0) {
        length = k + 7;
        break;
      }
    }
    d_.setWord(0x73, d);
    if (length > 0 && d + length > kLimit) throw Damaged();
    for (unsigned k = 0; k < length; k++) d_.setByte(d + k, i_.byte(fx + k));
    d += length;
  }

  static bool restRow(unsigned w) { return w == 0x0000 || w == 0x2000; }
  static bool holdRow(unsigned w) { return w == 0x007E || w == 0x207E; }

  // Rows of one kind that follow, up to 31; reading stops at <end>.
  unsigned count(unsigned& pos, unsigned end, bool hold) {
    unsigned n = 0;
    while (n < 31) {
      if (pos > end) throw Damaged();
      const unsigned w = s_.word(pos);
      if (hold ? holdRow(w) : restRow(w)) {
        n++;
        pos += 2;
      } else if (w == 0xF000) {
        pos += 2;
      } else {
        break;
      }
    }
    return n;
  }

  void sequences() {
    seqTable_ = d;
    d_.setWord(0x6D, d);
    d += 2 * sequenceCount_;
    for (unsigned s = 0; s < sequenceCount_; s++) {
      d_.setWord(seqTable_ + 2 * s, d);
      const unsigned start = 2 + kSeqSlot * s, end = start + 2 * kSeqSlot;
      unsigned pos = start;
      for (;;) {
        if (d > kLimit || pos > end) throw Damaged();
        const unsigned w = s_.word(pos);
        pos += 2;
        if (w == 0xFFFF) {
          put(w);
          break;
        }
        if (w == 0xF000) continue;
        if ((w >> 12) == 6) {          // a cut; the word after it says what it counts
          const unsigned next = s_.word(pos);
          pos += 2;
          put(0x6000 + count(pos, end, holdRow(next)));
        } else if (w >= 0x7000) {      // a command
          put(w);
        } else if ((w & 0xFF) == 0x00) {
          put(count(pos, end, false) << 8);
        } else if ((w & 0xFF) == 0x7E) {
          put((count(pos, end, true) << 8) + 0x7E);
        } else {                        // a note, and how long it is held
          put((w + (count(pos, end, true) << 8)) & 0xFFFF);
        }
      }
    }
  }

  // The instruments in use, numbered again from 1 in the order of their old numbers.
  void instruments() {
    const unsigned table = i_.word(0x6F);
    d_.setWord(0x6F, d);
    for (unsigned k = 0; k < 16; k++) d_.setByte(d + k, i_.byte(table + k));
    d += 16;
    unsigned f = d;
    const unsigned first = d_.word(seqTable_);
    const unsigned words = (d - 16 - first) & 0xFFFF;
    const unsigned fx = d_.word(0x73);
    const bool spfx = fx != seqTable_;
    std::vector<unsigned> entries;            // where the SpFX entries are
    if (spfx) {
      unsigned pos = fx;
      do {
        entries.push_back(pos);
        pos += 8;
      } while (pos < seqTable_);
    }
    unsigned top = 0xC001;
    for (unsigned i = 0; i < words; i++) {
      const unsigned w = d_.word(first + 2 * i);
      if ((w >> 12) == 0xC && w > top) top = w;
    }
    if (spfx) {
      unsigned z = top & 0xFFF;
      for (unsigned e : entries)
        if ((d_.word(e) & 0xFFF) > z) z = d_.word(e) & 0xFFF;
      top = 0xC000 + z;
    }
    unsigned k = 1;
    for (unsigned c = 0xC001; c <= top; c++) {
      bool used = false;
      for (unsigned i = 0; i < words && !used; i++) used = d_.word(first + 2 * i) == c;
      if (spfx)
        for (unsigned e : entries)
          if ((d_.word(e) & 0xFFF) == (c & 0xFFF)) used = true;
      if (!used) continue;
      if (f > kLimit) throw Damaged();
      const unsigned from = ((16 * c) & 0xFFFF) + table;
      for (unsigned b = 0; b < 16; b++) d_.setByte(f + b, i_.byte(from + b));
      f += 16;
      for (unsigned i = 0; i < words; i++)
        if (d_.word(first + 2 * i) == c) d_.setWord(first + 2 * i, 0xC000 + k);
      if (spfx)
        for (unsigned e : entries) {
          const unsigned w = d_.word(e);
          if ((w & 0xFFF) == (c & 0xFFF)) d_.setWord(e, (w & 0xF000) | k);
        }
      k++;
    }
    d = f + 16;
  }
};

// ------------------------------------------------------------------ playing the D00

// AdPlug reads the D00 from memory, under a name that ends in .d00.
class MemoryProvider : public CFileProvider {
 public:
  explicit MemoryProvider(std::vector<uint8_t>& data) : data_(data) {}
  binistream* open(std::string) const override {
    binisstream* f = new binisstream(data_.data(), data_.size());
    f->setFlag(binio::BigEndian, false);
    f->setFlag(binio::FloatIEEE, true);
    return f;
  }
  void close(binistream* f) const override { delete f; }

 private:
  std::vector<uint8_t>& data_;
};

// AdPlug's D00 player, with a look before each tick for a voice that would make it loop
// for ever.
class GuardedD00 : public Cd00Player {
 public:
  GuardedD00(Copl* opl, bool guard) : Cd00Player(opl), guard_(guard) {}
  bool update() override {
    if (guard_ && wouldLoop()) {
      songend = 1;
      return false;
    }
    return Cd00Player::update();
  }

 private:
  bool guard_;
  std::vector<uint32_t> seen_ = std::vector<uint32_t>(0x10000, 0);   // order entries met, by walk
  uint32_t walk_ = 0;

  // Whether word <index> after <p> is inside the file: AdPlug's own bounds test.
  bool inside(const void* p, unsigned long index) const {
    return (static_cast<const char*>(p) - filedata) + (index + 1) * 2 <= filesize;
  }
  unsigned wordAt(const unsigned short* p, unsigned long index) const {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(p + index);
    return b[0] | b[1] << 8;
  }

  // Follows, for each voice that reads its sequence in the coming tick, the steps the
  // player takes through its order list and sequence, without playing anything. A voice
  // that comes back to the start of the same order entry without reaching a note, rest,
  // hold or cut would loop for ever.
  bool wouldLoop() {
    for (int c = 0; c < 9; c++) {
      const auto& ch = channel[c];
      if (version < 3 ? ch.del != 0 : ch.del <= 0x7f) continue;
      if (!ch.speed || ch.rhcnt) continue;
      if (voiceLoops(ch.order, ch.ordpos, ch.pattpos)) return true;
    }
    return false;
  }

  bool voiceLoops(const unsigned short* order, unsigned short ordpos, unsigned short pattpos) {
    if (++walk_ == 0) {
      seen_.assign(0x10000, 0);
      walk_ = 1;
    }
    const unsigned short* patt = nullptr;
    for (;;) {
      // the order list
      if (pattpos == 0) {
        if (seen_[ordpos] == walk_) return true;
        seen_[ordpos] = walk_;
      }
      if (!order || !inside(order, ordpos)) return false;
      unsigned ord = wordAt(order, ordpos);
      if (ord == 0xFFFE) return false;
      if (ord == 0xFFFF) {
        if (!inside(order, ordpos + 1UL)) return false;
        ordpos = wordAt(order, ordpos + 1UL);
        continue;
      }
      if (ord >= 0x9000) {
        ord = ordpos > 0 ? wordAt(order, ordpos - 1UL) : 0;
        ordpos++;
      } else if (ord >= 0x8000) {
        if (!inside(order, ordpos + 1UL)) return false;
        ord = wordAt(order, ++ordpos);
      }
      if (!inside(seqptr, ord) || wordAt(seqptr, ord) + 2UL > filesize) return false;
      patt = reinterpret_cast<const unsigned short*>(filedata + wordAt(seqptr, ord));
      // the sequence
      for (;;) {
        const unsigned w = inside(patt, pattpos) ? wordAt(patt, pattpos) : 0xFFFF;
        if (w == 0xFFFF) {
          pattpos = 0;
          ordpos++;
          break;
        }
        pattpos++;
        const unsigned cnt = w >> 8, fx = w >> 12;
        if (version ? cnt < 0x40 : !fx) return false;   // a note, rest or hold
        if (fx == 6) return false;                      // a cut
      }
    }
  }
};

struct Output {
  GuardedD00* player;
  CNemuopl* opl;
  int rate;
  double pending = 0;
  bool playing = true;
  long long ticks = 0;

  // Up to <frames> frames; fewer when the song is over.
  size_t render(int16_t* out, size_t frames) {
    size_t made = 0;
    while (made < frames) {
      if (pending < 1) {
        if (!playing) break;
        playing = player->update();
        const float h = player->getrefresh();
        const float hz = h > 1 ? h : 70.0f;
        pending += static_cast<float>(rate) / hz;
        ticks++;
        if (ticks * 1000.0 / hz > 900000) playing = false;
      }
      size_t n = static_cast<size_t>(pending);
      if (n > frames - made) n = frames - made;
      opl->update(out + 2 * made, static_cast<int>(n));
      pending -= n;
      made += n;
    }
    return made;
  }
};

}  // namespace

int playEdlib(const char* path, int /*track*/, int rate, long long startMs) {
  const std::vector<uint8_t> file = readFile(path);
  std::vector<uint8_t> song;
  const std::string why = unpack(file, song);
  if (!why.empty()) return fail(why);
  std::vector<uint8_t> d00;
  {
    Converter conv(song);
    const Area& i = conv.block3();
    if (std::memcmp(i.m.data(), kId, 6) != 0) return fail("not an EdLib song: no JCH header inside");
    if (i.byte(6) != 0 || i.byte(0x0A) != 0) return fail("an EdLib song for another sound card than AdLib");
    try {
      d00 = conv.run();
    } catch (const Damaged&) {
      return fail("the EdLib song is damaged");
    }
  }
  if (const char* dump = std::getenv("BBMP_EDL_D00")) {
    if (*dump) {
      if (FILE* f = std::fopen(dump, "wb")) {
        std::fwrite(d00.data(), 1, d00.size(), f);
        std::fclose(f);
      }
    }
  }

  const char* unguarded = std::getenv("BBMP_EDL_UNGUARDED");
  const bool guard = !(unguarded && *unguarded && std::strcmp(unguarded, "0") != 0);
  CNemuopl opl(rate);
  GuardedD00 player(&opl, guard);
  MemoryProvider provider(d00);
  if (!player.load("song.d00", provider)) return fail("AdPlug doesn't take the converted song");

  Info info;
  info.title = fromCp437(field(&d00[0x0B], 32));
  info.author = fromCp437(field(&d00[0x2B], 32));
  info.type = "EdLib song";
  info.durationMs = static_cast<long long>(player.songlength(0));
  player.rewind(0);
  info.facts.push_back({"player", player.gettype()});
  info.facts.push_back({"timer", std::to_string(d00[8]) + " Hz"});

  Output out{&player, &opl, rate};
  std::vector<int16_t> buf(2 * 4096);
  long long skip = startMs * rate / 1000;
  while (skip > 0) {
    const size_t want = skip > 4096 ? 4096 : static_cast<size_t>(skip);
    const size_t got = out.render(buf.data(), want);
    skip -= static_cast<long long>(want);
    if (got < want) break;
  }
  header(info);
  for (;;) {
    const size_t got = out.render(buf.data(), 4096);
    if (got && !emit(buf.data(), got)) break;
    if (got < 4096) break;
  }
  std::fflush(stdout);
  return 0;
}

}  // namespace misc
