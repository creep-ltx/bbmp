// SPDX-License-Identifier: GPL-3.0-or-later
// DigiTrekker modules (.dtm): Horst Beham's DOS tracker, DigiTrekker 3.00-3.10 (1994-95).
//
// Written for this player from the format's document (DTMDOC.TXT, "DigiTrekker Module
// Format rev1.0", in DigiTrekker 3.10's package and in libxmp's docs) and the effect
// list of the tracker's help file (TREKKER.HLP), and compared with what DigiTrekker
// 3.10 itself renders (its FILE8S.DRV driver, run in DOSBox).
//
// The file is IFF-like, little-endian: a SONG chunk holding NAME, INFO (channels,
// sequence length, patterns, tracks, instruments), INIT (sped: speed and tempo;
// vpan: a left and right volume for each channel), PSEQ (the sequence), PATT (for each
// pattern a track number per channel, counted from 0; track 0 is kept empty), INST (samp: name, length, loop,
// C-4 rate, volume, bits; or text), TRAK (trak: rows, then each column of the rows in
// turn: pitch, instrument, volume, effect, parameter high, parameter low) and SAMP
// (the samples, signed PCM, in the order of INST).
//
// Pitch is linear: 128 steps a semitone. A pattern's channels play tracks; a channel
// is heard at its left and right volume (0-64 each), which effects can change.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>

#include "common.h"

namespace pcmusic {

namespace {

uint16_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }
uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

struct Chunk {
  char id[5] = {};
  const uint8_t* data = nullptr;
  size_t size = 0;
};

std::vector<Chunk> chunks(const uint8_t* p, size_t size) {
  std::vector<Chunk> out;
  size_t at = 0;
  while (at + 8 <= size) {
    Chunk c;
    std::memcpy(c.id, p + at, 4);
    const uint32_t len = le32(p + at + 4);
    c.data = p + at + 8;
    c.size = len <= size - at - 8 ? len : size - at - 8;
    out.push_back(c);
    at += 8 + size_t(len);
  }
  return out;
}

struct Sample {
  std::string name;
  bool isSample = false;
  std::vector<float> pcm;          // -1..1
  uint32_t loopStart = 0, loopEnd = 0;
  unsigned c4rate = 8363;
  int volume = 64;
};

struct Cell {
  uint8_t pitch, inst, vol, eff, hi, lo;
};

struct Track {
  int rows = 0;
  std::vector<Cell> cells;
};

const double kVibSine[64] = {
#define S(i) std::sin((i) * 2 * M_PI / 64)
  S(0),  S(1),  S(2),  S(3),  S(4),  S(5),  S(6),  S(7),  S(8),  S(9),  S(10), S(11), S(12),
  S(13), S(14), S(15), S(16), S(17), S(18), S(19), S(20), S(21), S(22), S(23), S(24), S(25),
  S(26), S(27), S(28), S(29), S(30), S(31), S(32), S(33), S(34), S(35), S(36), S(37), S(38),
  S(39), S(40), S(41), S(42), S(43), S(44), S(45), S(46), S(47), S(48), S(49), S(50), S(51),
  S(52), S(53), S(54), S(55), S(56), S(57), S(58), S(59), S(60), S(61), S(62), S(63)
#undef S
};

struct Channel {
  // what plays
  const Sample* smp = nullptr;
  double pos = 0;
  bool playing = false;
  int inst = 0;
  int pitch = 0;             // 1/128 semitones from C-0
  int basePitch = 0;         // without vibrato / arpeggio
  int volume = 0;            // 0..64
  int left = 32, right = 32; // 0..64
  int finetune = 0;          // 1/128 semitones
  // effects
  uint8_t eff = 0, hi = 0, lo = 0;
  int portaTarget = 0, portaSpeed = 0;
  int vibSpeed = 0, vibDepth = 0, vibPos = 0, vibWave = 0;
  int tremSpeed = 0, tremDepth = 0, tremPos = 0, tremWave = 0;
  int volAdd = 0, volSub = 0;          // the last fade
  int tremorOn = 0, tremorOff = 0, tremorCount = 0;
  bool tremorMute = false;
  int volOffset = 0;                   // tremolo
  int arpOffset = 0, vibOffset = 0;
  bool glissando = false;
  int delayFrame = -1, cutFrame = -1;
  Cell delayed{};
  uint32_t lastOffset = 0;
  int loopRow = 0, loopCount = 0;
  int retrigFrames = 0, retrigFn = 0, retrigX = 0, retrigY = 0;
};

class Digitrekker {
 public:
  bool load(const std::vector<uint8_t>& file, std::string& why) {
    if (file.size() < 16 || std::memcmp(file.data(), "SONG", 4) != 0) {
      why = "not a DigiTrekker module";
      return false;
    }
    const std::vector<Chunk> top = chunks(file.data() + 8, std::min<size_t>(le32(&file[4]), file.size() - 8));
    std::vector<Chunk> inst, trak, samp;
    const Chunk* patt = nullptr;
    bool haveInfo = false;
    for (const Chunk& c : top) {
      const std::string id(c.id);
      if (id == "NAME") {
        title_ = fromCp437(field(c.data, c.size));
      } else if (id == "INFO" && c.size >= 10) {
        channels_ = le16(c.data);
        length_ = le16(c.data + 2);
        patterns_ = le16(c.data + 4);
        tracks_ = le16(c.data + 6);
        instruments_ = le16(c.data + 8);
        haveInfo = true;
      } else if (id == "INIT") {
        for (const Chunk& s : chunks(c.data, c.size)) {
          if (!std::memcmp(s.id, "sped", 4) && s.size >= 2) {
            // DigiTrekker 3.00 wrote two bytes and whatever followed them
            if (s.size >= 4 && s.data[1] == 0) {
              speed_ = le16(s.data);
              tempo_ = le16(s.data + 2);
            } else {
              speed_ = s.data[0];
              tempo_ = s.data[1];
            }
          } else if (!std::memcmp(s.id, "vpan", 4)) {
            for (size_t i = 0; 2 * i + 1 < s.size && i < 64; i++) {
              panL_[i] = s.data[2 * i];
              panR_[i] = s.data[2 * i + 1];
            }
          }
        }
      } else if (id == "PSEQ") {
        seq_.assign(c.data, c.data + c.size);
      } else if (id == "PATT") {
        patt = &c;
      } else if (id == "INST") {
        inst = chunks(c.data, c.size);
      } else if (id == "TRAK") {
        trak = chunks(c.data, c.size);
      } else if (id == "SAMP") {
        samp = chunks(c.data, c.size);
      }
    }
    if (!haveInfo || channels_ < 1 || channels_ > 64 || !patt) {
      why = "the DigiTrekker module has no song in it";
      return false;
    }
    if (seq_.size() > size_t(length_)) seq_.resize(length_);
    if (seq_.empty()) {
      why = "the DigiTrekker module's sequence is empty";
      return false;
    }
    pattern_.assign(size_t(patterns_) * channels_, 0);
    for (size_t i = 0; i < pattern_.size() && 2 * i + 1 < patt->size; i++) pattern_[i] = le16(patt->data + 2 * i);

    for (const Chunk& t : trak) {
      Track tr;
      if (!std::memcmp(t.id, "trak", 4) && t.size >= 2) {
        tr.rows = le16(t.data);
        if (size_t(tr.rows) * 6 + 2 > t.size) tr.rows = static_cast<int>((t.size - 2) / 6);
        const uint8_t* d = t.data + 2;
        const int n = tr.rows;
        for (int r = 0; r < n; r++)
          tr.cells.push_back({d[r], d[n + r], d[2 * n + r], d[3 * n + r], d[4 * n + r], d[5 * n + r]});
      }
      trackData_.push_back(tr);
    }

    size_t sampAt = 0;
    for (const Chunk& i : inst) {
      Sample s;
      if (!std::memcmp(i.id, "samp", 4) && i.size >= 48) {
        s.isSample = true;
        s.name = fromCp437(field(i.data, 32));
        const uint32_t len = le32(i.data + 32);
        s.loopStart = le32(i.data + 36);
        s.loopEnd = le32(i.data + 40);
        s.c4rate = le16(i.data + 44);
        s.volume = i.data[46] > 64 ? 64 : i.data[46];
        const int bits = i.data[47] == 16 ? 16 : 8;
        // the sample data: the next chunk of SAMP
        while (sampAt < samp.size() && std::memcmp(samp[sampAt].id, "samp", 4) != 0) sampAt++;
        if (sampAt < samp.size()) {
          const Chunk& d = samp[sampAt++];
          const size_t frames = std::min<size_t>(len, d.size / (bits / 8));
          s.pcm.resize(frames);
          for (size_t k = 0; k < frames; k++)
            s.pcm[k] = bits == 8 ? static_cast<int8_t>(d.data[k]) / 128.0f
                                 : static_cast<int16_t>(le16(d.data + 2 * k)) / 32768.0f;
        }
        if (s.loopEnd > s.pcm.size()) s.loopEnd = static_cast<uint32_t>(s.pcm.size());
        if (s.loopStart >= s.loopEnd) s.loopEnd = 0;
      } else if (!std::memcmp(i.id, "text", 4)) {
        s.name = fromCp437(field(i.data, i.size));
        if (sampAt < samp.size() && !std::memcmp(samp[sampAt].id, "text", 4)) sampAt++;
      }
      samples_.push_back(s);
    }
    if (speed_ < 1) speed_ = 6;
    if (tempo_ < 16) tempo_ = 125;
    return true;
  }

  void start(int rate) {
    rate_ = rate;
    speed = speed_;
    tempo = tempo_;
    order_ = 0;
    row_ = 0;
    frame_ = 0;
    ended_ = false;
    patternDelay_ = 0;
    breakRow_ = -1;
    jumpTo_ = -1;
    visited_.clear();
    ch_.assign(channels_, Channel());
    for (int c = 0; c < channels_; c++) {
      ch_[c].left = c < 64 ? panL_[c] : 32;
      ch_[c].right = c < 64 ? panR_[c] : 32;
    }
    frameLeft_ = 0;
  }

  // Renders up to `frames` stereo frames; fewer when the song has ended.
  int render(int16_t* out, int frames) {
    int n = 0;
    while (n < frames) {
      if (frameLeft_ <= 0) {
        if (!nextFrame()) break;
        frameLeft_ += rate_ * 5.0 / (tempo * 2.0);      // frames/s = BPM * 2 / 5
      }
      int todo = static_cast<int>(std::ceil(frameLeft_));
      if (todo > frames - n) todo = frames - n;
      if (todo < 1) todo = 1;
      mix(out + 2 * n, todo);
      n += todo;
      frameLeft_ -= todo;
    }
    return n;
  }

  bool ended() const { return ended_; }

  std::string title_;
  int channels_ = 0, length_ = 0, patterns_ = 0, tracks_ = 0, instruments_ = 0;
  std::vector<Sample> samples_;

 private:
  int speed = 6, tempo = 125;

  const Cell* cellAt(int pattern, int c, int row) const {
    static const Cell empty{};
    if (pattern >= patterns_) return &empty;
    const int t = pattern_[size_t(pattern) * channels_ + c];
    if (t >= int(trackData_.size())) return &empty;
    const Track& tr = trackData_[t];
    return row < tr.rows ? &tr.cells[row] : &empty;
  }

  int patternRows(int pattern) const {
    int rows = 0;
    for (int c = 0; c < channels_; c++) {
      if (pattern >= patterns_) break;
      const int t = pattern_[size_t(pattern) * channels_ + c];
      if (t < int(trackData_.size())) rows = std::max(rows, trackData_[t].rows);
    }
    return rows > 0 ? rows : 64;
  }

  static int param16(const Channel& c) { return (c.hi << 8) | c.lo; }

  void trigger(Channel& c, const Cell& cell, bool noteToo) {
    if (cell.inst && cell.inst <= samples_.size()) {
      c.inst = cell.inst;
      const Sample& s = samples_[cell.inst - 1];
      c.volume = s.volume;
    }
    if (noteToo && cell.pitch == 0x80) {
      c.playing = false;
    } else if (noteToo && cell.pitch >= 1 && cell.pitch <= 96) {
      if (c.inst && samples_[c.inst - 1].isSample) {
        c.smp = &samples_[c.inst - 1];
        c.pos = 0;
        c.playing = !c.smp->pcm.empty();
        c.basePitch = (cell.pitch - 1) * 128;
        c.vibPos = (c.vibWave & 4) ? c.vibPos : 0;
        c.tremPos = (c.tremWave & 4) ? c.tremPos : 0;
        c.tremorCount = 0;
        c.tremorMute = false;
      }
    }
    if (cell.vol) c.volume = std::min(cell.vol - 1, 64);
  }

  void newRow() {
    if (order_ >= int(seq_.size())) {
      ended_ = true;
      return;
    }
    if (!visited_.insert(order_ * 65536 + row_).second) {     // the song repeats
      ended_ = true;
      return;
    }
    const int pat = seq_[order_];
    for (int ci = 0; ci < channels_; ci++) {
      Channel& c = ch_[ci];
      const Cell& cell = *cellAt(pat, ci, row_);
      c.eff = cell.eff ? cell.eff : 0;
      c.hi = cell.hi;
      c.lo = cell.lo;
      c.arpOffset = c.vibOffset = c.volOffset = 0;
      c.delayFrame = c.cutFrame = -1;
      c.retrigFrames = 0;
      const int e = c.eff;
      const bool porta = e == 4 || e == 6;
      if (e == 15 && c.hi == 13 && c.lo > 0) {         // delay sample
        c.delayFrame = c.lo;
        c.delayed = cell;
      } else if (porta && cell.pitch >= 1 && cell.pitch <= 96) {
        Cell noNote = cell;
        noNote.pitch = 0;
        trigger(c, noNote, false);
        c.portaTarget = (cell.pitch - 1) * 128;
      } else {
        trigger(c, cell, true);
      }
      rowEffect(ci, c);
    }
  }

  void rowEffect(int ci, Channel& c) {
    (void)ci;
    const int p = param16(c);
    switch (c.eff) {
      case 2:
        if (c.hi == 0xFF) c.basePitch += c.lo;
        break;
      case 3:
        if (c.hi == 0xFF) c.basePitch -= c.lo;
        break;
      case 4:
        if (p) c.portaSpeed = p;
        break;
      case 5:
        if (c.hi) c.vibSpeed = c.hi;
        if (c.lo) c.vibDepth = c.lo;
        break;
      case 6:
      case 7:
      case 11:
        fadeParams(c);
        break;
      case 8:
        if (c.hi) c.tremSpeed = c.hi;
        if (c.lo) c.tremDepth = c.lo;
        break;
      case 10: {
        const uint32_t off = uint32_t(p ? p : c.lastOffset) * 16;
        if (p) c.lastOffset = p;
        if (c.smp) c.pos = off;
        break;
      }
      case 12:
        jumpTo_ = p;
        break;
      case 13:
        c.volume = std::min(p, 64);
        break;
      case 14:
        breakRow_ = p;
        break;
      case 15:
        extended(c);
        break;
      case 16:
        if (c.hi) speed = c.hi;
        if (c.lo) tempo = c.lo * 125 / 50;
        break;
      case 17:
        if (p >= 16) tempo = p;
        break;
      case 18:
        c.left = std::min<int>(c.hi, 64);
        c.right = std::min<int>(c.lo, 64);
        break;
      case 20:
        if (c.smp) c.pos += double(p) * 16;
        break;
      case 22: {
        const int d = c.hi >> 4;
        c.retrigFrames = d;
        c.retrigFn = c.hi & 15;
        c.retrigX = c.lo >> 4;
        c.retrigY = c.lo & 15;
        break;
      }
      case 23:
        if (c.hi || c.lo) {
          c.tremorOn = c.hi;
          c.tremorOff = c.lo;
          c.tremorCount = 0;
        }
        break;
    }
    // fine fades happen at the start of the row
    if (c.eff == 6 || c.eff == 7 || c.eff == 11) {
      if (c.hi == 0xFF) c.volume = std::max(0, c.volume - c.lo);
      if (c.lo == 0xFF) c.volume = std::min(64, c.volume + c.hi);
    }
  }

  void fadeParams(Channel& c) {
    c.volAdd = c.volSub = 0;
    if (c.hi != 0 && c.hi != 0xFF && c.lo != 0xFF) c.volAdd = c.hi;
    if (c.lo != 0 && c.lo != 0xFF && c.hi != 0xFF) c.volSub = c.lo;
  }

  void extended(Channel& c) {
    const int x = c.lo;
    switch (c.hi) {
      case 1: c.basePitch += x; break;
      case 2: c.basePitch -= x; break;
      case 3: c.glissando = x != 0; break;
      case 4: c.vibWave = x; break;
      case 5: c.finetune = static_cast<int8_t>(x) * 2; break;   // x/64 semitones
      case 6:
        if (x == 0) {
          c.loopRow = row_;
        } else if (c.loopCount == 0) {
          c.loopCount = x;
          loopJump_ = c.loopRow;
        } else if (--c.loopCount > 0) {
          loopJump_ = c.loopRow;
        }
        break;
      case 7: c.tremWave = x; break;
      case 8: {                  // "E080x: set stereo panning" (3.10); 0 left .. 15 right, a guess
        const int p = std::min(x, 15);
        c.left = (15 - p) * 64 / 15;
        c.right = p * 64 / 15;
        break;
      }
      case 9: c.retrigFrames = x; c.retrigFn = 0; break;
      case 10: c.volume = std::min(64, c.volume + x); break;
      case 11: c.volume = std::max(0, c.volume - x); break;
      case 12: c.cutFrame = x; break;
      case 14: patternDelay_ = x; break;
    }
  }

  static double wave(int kind, int pos) {
    pos &= 63;
    switch (kind & 3) {
      case 1: return 1.0 - pos / 32.0;
      case 2: return pos < 32 ? 1.0 : -1.0;
      case 3: return (std::rand() % 2001 - 1000) / 1000.0;
      default: return kVibSine[pos];
    }
  }

  // One frame (tick): a new row on frame 0, else the effects that run in frames.
  bool nextFrame() {
    if (ended_) return false;
    if (frame_ == 0) {
      newRow();
      if (ended_) return false;
    } else {
      for (auto& c : ch_) frameEffect(c);
    }
    for (auto& c : ch_) {
      c.pitch = c.basePitch + c.arpOffset + c.vibOffset + c.finetune;
      // tremor, only on rows that have it (0,0 goes on with the last): x frames on,
      // y frames off
      c.tremorMute = false;
      if (c.eff == 23 && (c.tremorOn || c.tremorOff)) {
        const int cycle = c.tremorOn + c.tremorOff;
        c.tremorMute = (c.tremorCount % cycle) >= c.tremorOn;
        c.tremorCount++;
      }
    }
    frame_++;
    if (frame_ >= speed * (1 + patternDelay_)) {
      frame_ = 0;
      patternDelay_ = 0;
      advance();
    }
    return true;
  }

  void advance() {
    const int rows = patternRows(seq_[order_]);
    if (loopJump_ >= 0) {
      row_ = loopJump_;
      loopJump_ = -1;
      visited_.clear();                    // a pattern loop is not the song repeating
      breakRow_ = jumpTo_ = -1;
      return;
    }
    if (jumpTo_ >= 0 || breakRow_ >= 0) {
      order_ = jumpTo_ >= 0 ? jumpTo_ : order_ + 1;
      row_ = breakRow_ >= 0 ? breakRow_ : 0;
      jumpTo_ = breakRow_ = -1;
      if (order_ < int(seq_.size()) && row_ >= patternRows(seq_[order_])) row_ = 0;
      return;
    }
    if (++row_ >= rows) {
      row_ = 0;
      order_++;
    }
  }

  void frameEffect(Channel& c) {
    const int p = param16(c);
    switch (c.eff) {
      case 1: {
        const int step = frame_ % 3;
        c.arpOffset = step == 0 ? 0 : (step == 1 ? c.hi : c.lo) * 128;
        break;
      }
      case 2:
        if (c.hi != 0xFF) c.basePitch += p;
        break;
      case 3:
        if (c.hi != 0xFF) c.basePitch -= p;
        break;
      case 4:
      case 6:
        if (c.basePitch < c.portaTarget) c.basePitch = std::min(c.portaTarget, c.basePitch + c.portaSpeed);
        else if (c.basePitch > c.portaTarget) c.basePitch = std::max(c.portaTarget, c.basePitch - c.portaSpeed);
        break;
      case 5:
      case 7:
        c.vibPos += c.vibSpeed;
        c.vibOffset = static_cast<int>(std::lround(wave(c.vibWave, c.vibPos) * c.vibDepth));
        break;
      case 8:
        c.tremPos += c.tremSpeed;
        c.volOffset = static_cast<int>(std::lround(wave(c.tremWave, c.tremPos) * c.tremDepth));
        break;
      case 19: {
        c.left = std::clamp(c.left + static_cast<int8_t>(c.hi), 0, 64);
        c.right = std::clamp(c.right + static_cast<int8_t>(c.lo), 0, 64);
        break;
      }
      case 22:
        if (c.retrigFrames && frame_ % c.retrigFrames == 0) {
          c.pos = 0;
          c.playing = c.smp && !c.smp->pcm.empty();
          const int xy = (c.retrigX << 4) | c.retrigY;
          switch (c.retrigFn) {
            case 1: c.volume = std::max(0, c.volume - xy); break;
            case 2: c.volume = std::min(64, c.volume + xy); break;
            case 3: if (c.retrigY) c.volume = std::min(64, c.volume * c.retrigX / c.retrigY); break;
          }
        }
        break;
    }
    if (c.eff == 6 || c.eff == 7 || c.eff == 11) {
      c.volume = std::clamp(c.volume + c.volAdd - c.volSub, 0, 64);
    }
    if (c.eff == 15 && c.hi == 9 && c.retrigFrames && frame_ % c.retrigFrames == 0) {
      c.pos = 0;
      c.playing = c.smp && !c.smp->pcm.empty();
    }
    if (c.cutFrame >= 0 && frame_ == c.cutFrame) c.volume = 0;
    if (c.delayFrame >= 0 && frame_ == c.delayFrame) {
      trigger(c, c.delayed, true);
      c.delayFrame = -1;
    }
  }

  void mix(int16_t* out, int frames) {
    std::vector<float> l(frames, 0.0f), r(frames, 0.0f);
    for (auto& c : ch_) {
      if (!c.playing || !c.smp || c.tremorMute) continue;
      const Sample& s = *c.smp;
      const int vol = std::clamp(c.volume + c.volOffset, 0, 64);
      if (vol == 0) {
        advanceOnly(c, frames);
        continue;
      }
      const double hz = s.c4rate * std::exp2((c.pitch - 48 * 128) / 1536.0);
      const double step = hz / rate_;
      const float gl = float(vol * c.left) / (64.0f * 64.0f), gr = float(vol * c.right) / (64.0f * 64.0f);
      const size_t len = s.pcm.size();
      for (int i = 0; i < frames; i++) {
        if (s.loopEnd) {
          if (!wrapLoop(c, s)) break;
        } else if (c.pos >= len) {
          c.playing = false;
          break;
        }
        const size_t k = static_cast<size_t>(c.pos);
        const double f = c.pos - k;
        size_t k1 = k + 1;
        if (s.loopEnd && k1 >= s.loopEnd) k1 = s.loopStart;
        const float a = s.pcm[k], b = k1 < len ? s.pcm[k1] : 0.0f;
        const float v = a + float(f) * (b - a);
        l[i] += v * gl;
        r[i] += v * gr;
        c.pos += step;
      }
    }
    for (int i = 0; i < frames; i++) {
      out[2 * i] = clip16(l[i] * kGain);
      out[2 * i + 1] = clip16(r[i] * kGain);
    }
  }

  // Brings a looped sample's position back into its loop; false (and the voice stopped)
  // when there is no position any more: a slide can take the pitch out of range. Far
  // past the loop's end, one step instead of a subtraction per loop.
  static bool wrapLoop(Channel& c, const Sample& s) {
    if (!std::isfinite(c.pos)) {
      c.playing = false;
      return false;
    }
    const double loop = s.loopEnd - s.loopStart;
    if (c.pos >= s.loopEnd + 64 * loop) c.pos = s.loopStart + std::fmod(c.pos - s.loopStart, loop);
    while (c.pos >= s.loopEnd) c.pos -= loop;
    return true;
  }

  void advanceOnly(Channel& c, int frames) {
    const Sample& s = *c.smp;
    const double hz = s.c4rate * std::exp2((c.pitch - 48 * 128) / 1536.0);
    c.pos += hz / rate_ * frames;
    if (s.loopEnd) {
      wrapLoop(c, s);
    } else if (c.pos >= s.pcm.size()) {
      c.playing = false;
    }
  }

  // DigiTrekker 3.10's own rendering is about twice as loud and clips (4% of the
  // samples of keepcool.dtm); this is about a third of that, which clips neither song
  static constexpr float kGain = 32767.0f * 0.28f;

  int speed_ = 6, tempo_ = 125;
  uint8_t panL_[64], panR_[64];
  std::vector<uint8_t> seq_;
  std::vector<uint16_t> pattern_;
  std::vector<Track> trackData_;
  std::vector<Channel> ch_;
  int rate_ = 48000;
  int order_ = 0, row_ = 0, frame_ = 0;
  int patternDelay_ = 0, breakRow_ = -1, jumpTo_ = -1, loopJump_ = -1;
  bool ended_ = false;
  double frameLeft_ = 0;
  std::set<int> visited_;

 public:
  Digitrekker() {
    std::memset(panL_, 32, sizeof panL_);
    std::memset(panR_, 32, sizeof panR_);
  }
};

}  // namespace

int playDigitrekker(const char* path, int track, int rate, long long startMs) {
  (void)track;
  const std::vector<uint8_t> file = readFile(path);
  Digitrekker song;
  std::string why;
  if (!song.load(file, why)) return fail(why);

  // the length: play it through once without sound
  std::vector<int16_t> buf(2 * 4096);
  song.start(8000);
  long long frames = 0;
  while (frames < 8000LL * 3600) {
    const int got = song.render(buf.data(), 4096);
    frames += got;
    if (got < 4096) break;
  }

  Info info;
  info.type = "DigiTrekker";
  info.title = song.title_;
  info.durationMs = frames * 1000 / 8000;
  info.facts.emplace_back("channels", std::to_string(song.channels_));
  info.facts.emplace_back("patterns", std::to_string(song.patterns_) + ", of " +
                                          std::to_string(song.tracks_) + " tracks");
  for (const auto& s : song.samples_) info.names.push_back(s.name);

  song.start(rate);
  long long skip = startMs * rate / 1000;
  while (skip > 0) {
    const int n = skip > 4096 ? 4096 : static_cast<int>(skip);
    const int got = song.render(buf.data(), n);
    skip -= n;
    if (got < n) break;
  }
  header(info);
  while (true) {
    const int got = song.render(buf.data(), 4096);
    if (got > 0 && !emit(buf.data(), static_cast<size_t>(got))) break;
    if (got < 4096) break;
  }
  return 0;
}

}  // namespace pcmusic
