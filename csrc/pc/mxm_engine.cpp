// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-pc, engine "mxm": Cubic Tiny XM, the packed form of a FastTracker 2 module
// that Cubic Team's MXMPlay plays. The file is turned back into an ordinary XM
// module in memory and handed to libopenmpt, which plays it.
//
// Written for bbmp as a clean-room rewrite, from a description of the format and
// of the module to be built; the older engine was not looked at.
//
// Test hook: when BBMP_MXM_XM names a file, the XM built is written there.

#include "bytes.h"
#include "engine.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using pcbytes::Bad;
using pcbytes::In;
using pcbytes::Out;

constexpr size_t kFixed = 1872;            // the fixed part of every file
constexpr size_t kWinTable = 1872;         // Win32 files: 2048 sample offsets here
constexpr uint64_t kWinFirst = 10064;      // first object at or past this: a Win32 file
constexpr unsigned kMaxSmp = 16;

// One sample as the converter sees it.
struct Smp {
  bool wide = false;      // 16-bit
  uint32_t start = 0;     // in its memory: bytes (8-bit) or words (16-bit)
  uint32_t lstart = 0;    // loop, relative to the start
  uint32_t lend = 0;
  uint32_t len = 0;
  uint8_t mode = 0;
  uint8_t vol = 0;
  uint8_t pan = 0;
  int16_t pitch = 0;
};

struct Ins {
  size_t at = 0;          // where it lies in the file
  uint32_t n = 0;         // samples
  std::vector<Smp> smp;
};

// the XM's vibrato number for each of the MXM's
uint8_t vibrato(uint8_t t) {
  static const uint8_t map[4] = {0, 3, 1, 2};
  return t < 4 ? map[t] : 0;
}

struct Env {
  uint16_t x[12] = {}, y[12] = {};
  uint8_t points = 0, sustain = 0, lstart = 0, lend = 0, type = 0;
};

Env envelope(const In& f, size_t at) {
  Env e;
  const unsigned segs = f.u8(at);
  if (segs == 0 || segs > 11) return e;
  const unsigned sus = f.u8(at + 1), ls = f.u8(at + 2), le = f.u8(at + 3);
  e.points = static_cast<uint8_t>(segs + 1);
  uint32_t x = 0;
  for (unsigned k = 0; k <= segs; ++k) {
    const size_t entry = at + 4 + 4 * k;
    e.x[k] = static_cast<uint16_t>(x);
    e.y[k] = std::min<unsigned>(f.le16(entry + 2), 64);
    x += f.le16(entry);
  }
  e.type = 1;
  if (sus < e.points) { e.type += 2; e.sustain = static_cast<uint8_t>(sus); }
  if (le < e.points && ls <= le) {
    e.type += 4;
    e.lstart = static_cast<uint8_t>(ls);
    e.lend = static_cast<uint8_t>(le);
  }
  return e;
}

std::vector<uint8_t> convert(const In& f) {
  const size_t size = f.size();
  if (size < kFixed || f.u8(0) != 'M' || f.u8(1) != 'X' || f.u8(2) != 'M' || f.u8(3) != 0)
    throw Bad("not a Cubic Tiny XM module");

  const uint32_t orders = f.le32(4), restart = f.le32(8), chans = f.le32(12),
                 pats = f.le32(16), inss = f.le32(20);
  const unsigned speed = f.u8(24), bpm = f.u8(25), opts = f.le16(26);
  const uint32_t smpAt = f.le32(28), count8 = f.le32(32), count16 = f.le32(36);
  if (orders < 1 || orders > 256 || chans < 1 || chans > 32 || pats < 1 || pats > 256 ||
      inss < 1 || inss > 128 || speed == 0 || bpm < 32)
    throw Bad("the MXM module's header makes no sense");
  if (opts & 2) throw Bad("the MXM module pans by channel, which an XM module can't say");
  if (smpAt > size) throw Bad("the MXM module's samples are missing");

  // DOS or Win32: by where the first instrument or pattern lies
  uint64_t lowest = size;
  for (uint32_t i = 0; i < inss; ++i) lowest = std::min<uint64_t>(lowest, f.le32(336 + 4 * i));
  for (uint32_t p = 0; p < pats; ++p) lowest = std::min<uint64_t>(lowest, f.le32(848 + 4 * p));
  if (lowest < kFixed) throw Bad("the MXM module is damaged");
  const bool win = lowest >= kWinFirst;

  // the sample memories
  const uint64_t have = size - smpAt;
  const uint64_t size8 = std::min<uint64_t>(count8, have + 65536);
  std::vector<uint8_t> mem8(size8, 0);
  {
    const uint64_t n = std::min<uint64_t>(std::min<uint64_t>(count8, have), size8);
    for (uint64_t i = 0; i < n; ++i) mem8[i] = f.u8(smpAt + i);
  }
  const uint64_t have16 = have > count8 ? (have - count8) / 2 : 0;
  const uint64_t size16 = std::min<uint64_t>(count16, have16 + 32768);
  std::vector<uint16_t> mem16(size16, 0);
  for (uint64_t i = 0; i < size16; ++i) {
    const uint64_t at = static_cast<uint64_t>(smpAt) + count8 + 2 * i;
    if (at + 2 > size) break;
    mem16[i] = f.le16(at);
  }
  if (opts & 4) {
    uint8_t s8 = 0;
    for (auto& b : mem8) { s8 = static_cast<uint8_t>(s8 + b); b = s8; }
    uint16_t s16 = 0;
    for (auto& w : mem16) { s16 = static_cast<uint16_t>(s16 + w); w = s16; }
  }

  // the instruments and their sample headers
  std::vector<Ins> ins(inss);
  bool haveBase = false;
  uint32_t base = 0;
  for (uint32_t i = 0; i < inss; ++i) {
    Ins& in = ins[i];
    in.at = f.le32(336 + 4 * i);
    in.n = f.le32(in.at);
    if (in.n > kMaxSmp) throw Bad("an instrument of the MXM module is damaged");
    for (uint32_t j = 0; j < in.n; ++j) {
      const size_t h = in.at + 256 + 16 * j;
      Smp s;
      if (win) {
        s.lstart = f.le32(h);
        s.lend = f.le32(h + 4);
        const unsigned idx = f.le16(h + 13);
        if (idx >= 2048) throw Bad("a sample of the MXM module is damaged");
        const uint32_t off = f.le32(kWinTable + 4 * idx);
        s.mode = f.u8(h + 8);
        if (off < smpAt) throw Bad("a sample of the MXM module is damaged");
        s.wide = s.mode & 4;
        s.start = s.wide ? (off - smpAt - count8) / 2 : off - smpAt;
        s.vol = std::min<uint8_t>(f.u8(h + 9), 64);
        s.pan = f.u8(h + 10);
        s.pitch = static_cast<int16_t>(f.le16(h + 11));
      } else {
        auto u24 = [&](size_t at) {
          return static_cast<uint32_t>(f.u8(at) | f.u8(at + 1) << 8 | f.u8(at + 2) << 16);
        };
        const uint32_t a = u24(h), ls = u24(h + 3), le = u24(h + 6);
        s.mode = f.u8(h + 9);
        s.wide = s.mode & 4;
        if (s.wide && !haveBase) { haveBase = true; base = a; }
        if ((s.wide && a < base) || ls < a || le < ls)
          throw Bad("a sample of the MXM module is damaged");
        s.lstart = ls - a;
        s.lend = le - a;
        s.start = s.wide ? a - base : a;
        s.vol = std::min<uint8_t>(f.u8(h + 10), 64);
        s.pan = f.u8(h + 11);
        s.pitch = static_cast<int16_t>(f.le16(h + 12));
      }
      in.smp.push_back(s);
    }
  }

  // lengths and loops
  std::vector<uint32_t> starts[2];
  for (const Ins& in : ins)
    for (const Smp& s : in.smp) starts[s.wide].push_back(s.start);
  for (auto& v : starts) std::sort(v.begin(), v.end());
  const uint64_t limit = 8 * (size8 + 2 * size16) + 1048576;
  uint64_t used = 0;
  for (Ins& in : ins) {
    for (Smp& s : in.smp) {
      const uint64_t total = s.wide ? size16 : size8;
      const auto& v = starts[s.wide];
      const auto next = std::upper_bound(v.begin(), v.end(), s.start);
      const bool hasNext = next != v.end();
      const uint64_t end = hasNext ? std::min<uint64_t>(*next, total) : total;
      const uint64_t room = end > s.start ? end - s.start : 0;
      const bool looped = s.mode & 0x18;
      uint64_t len, lend = s.lend;
      if (win) {
        len = room > 0 ? room - 1 : 0;
        if (!hasNext) len = std::min<uint64_t>(len, std::max<uint64_t>(s.lend, 1));
        if (looped && s.lend < len && s.lend > s.lstart) lend = s.lend - 1;
      } else {
        len = s.lend;
      }
      len = std::min(len, room);
      lend = std::min(lend, len);
      s.len = static_cast<uint32_t>(len);
      s.lend = static_cast<uint32_t>(lend);
      if (!looped || s.lstart >= s.lend) {
        s.lstart = s.lend = 0;
        s.mode &= ~0x18;
      }
      used += s.wide ? 2 * len : len;
      if (used > limit) throw Bad("the samples of the MXM module overlap");
    }
  }

  // the XM's header
  Out x;
  const unsigned xc = chans + (chans & 1);
  x.text("Extended Module: ", 17);
  x.text("", 20, ' ');
  x.u8(0x1A);
  x.text("FastTracker v2.00", 20, ' ');
  x.le16(0x0104);
  x.le32(276);
  x.le16(orders);
  x.le16(restart < orders ? restart : 0);
  x.le16(xc);
  x.le16(pats);
  x.le16(inss);
  x.le16(opts & 1);
  x.le16(speed);
  x.le16(bpm);
  x.bytes(f.at(80, 256), 256);

  // the patterns
  for (uint32_t p = 0; p < pats; ++p) {
    size_t at = f.le32(848 + 4 * p);
    const uint32_t rows = f.le32(at);
    at += 4;
    if (rows == 0 || rows > 256) throw Bad("a pattern of the MXM module is damaged");
    Out packed;
    std::vector<uint8_t> cells(5 * xc);
    for (uint32_t r = 0; r < rows; ++r) {
      std::fill(cells.begin(), cells.end(), 0);
      for (;;) {
        const unsigned lead = f.u8(at++);
        if (lead == 0) break;
        uint8_t c[5] = {0, 0, 0, 0, 0};
        if (lead & 0x20) { c[0] = f.u8(at); c[1] = f.u8(at + 1); at += 2; }
        if (lead & 0x40) { c[2] = f.u8(at); at += 1; }
        if (lead & 0x80) { c[3] = f.u8(at); c[4] = f.u8(at + 1); at += 2; }
        if (c[3] >= 36) {
          if (c[3] > 51) throw Bad("the MXM module has an effect that isn't known");
          c[4] = static_cast<uint8_t>((c[3] - 36) * 16 + (c[4] & 15));
          c[3] = 14;
        }
        if (c[0] > 97) throw Bad("a pattern of the MXM module is damaged");
        const unsigned ch = lead & 31;
        if (ch < chans) std::copy(c, c + 5, cells.begin() + 5 * ch);
      }
      for (unsigned ch = 0; ch < xc; ++ch) {
        const uint8_t* c = &cells[5 * ch];
        if (c[0] && c[1] && c[2] && c[3] && c[4]) { packed.bytes(c, 5); continue; }
        unsigned mask = 0;
        for (int k = 0; k < 5; ++k) if (c[k]) mask |= 1u << k;
        packed.u8(0x80 | mask);
        for (int k = 0; k < 5; ++k) if (c[k]) packed.u8(c[k]);
      }
    }
    if (packed.size() > 65535) throw Bad("a pattern of the MXM module is too long");
    x.le32(9);
    x.u8(0);
    x.le16(rows);
    x.le16(static_cast<unsigned>(packed.size()));
    x.bytes(packed.v.data(), packed.size());
  }

  // the instruments and their samples
  for (uint32_t i = 0; i < inss; ++i) {
    const Ins& in = ins[i];
    const std::string name = "instrument " + std::to_string(i + 1);
    if (in.n == 0) {
      x.le32(29);
      x.text(name, 22);
      x.u8(0);
      x.le16(0);
      continue;
    }
    const uint8_t* keys = f.at(in.at + 4, 96);
    bool orphan = false;
    for (int k = 0; k < 96; ++k) if (keys[k] >= in.n) orphan = true;
    const unsigned stored = orphan && in.n < 16 ? in.n + 1 : in.n;
    const Env ve = envelope(f, in.at + 106), pe = envelope(f, in.at + 158);

    x.le32(263);
    x.text(name, 22);
    x.u8(0);
    x.le16(stored);
    x.le32(40);
    for (int k = 0; k < 96; ++k) x.u8(keys[k] < in.n ? keys[k] : stored - 1);
    for (const Env* e : {&ve, &pe})
      for (int k = 0; k < 12; ++k) { x.le16(e->x[k]); x.le16(e->y[k]); }
    x.u8(ve.points);
    x.u8(pe.points);
    x.u8(ve.sustain); x.u8(ve.lstart); x.u8(ve.lend);
    x.u8(pe.sustain); x.u8(pe.lstart); x.u8(pe.lend);
    x.u8(ve.type);
    x.u8(pe.type);
    x.u8(vibrato(f.u8(in.at + 102)));
    x.u8(f.u8(in.at + 103));
    x.u8(f.u8(in.at + 104));
    x.u8(f.u8(in.at + 105));
    x.le16(ve.type ? std::min<unsigned>(f.le16(in.at + 100), 0xFFF) : 0);
    x.zeros(22);

    for (const Smp& s : in.smp) {
      const unsigned w = s.wide ? 1 : 0;
      int note = s.pitch >= 0 ? (s.pitch + 128) / 256 : (s.pitch - 128) / 256;
      note = std::clamp(note, -96, 95);
      int fine = (s.pitch - note * 256) / 2;
      fine = std::clamp(fine, -128, 127);
      unsigned type = (s.mode & 16) ? 2 : (s.mode & 8) ? 1 : 0;
      if (s.wide) type += 16;
      x.le32(s.len << w);
      x.le32(s.lstart << w);
      x.le32((s.lend - s.lstart) << w);
      x.u8(s.vol);
      x.u8(static_cast<uint8_t>(fine));
      x.u8(type);
      x.u8(s.pan);
      x.u8(static_cast<uint8_t>(note));
      x.u8(0);
      x.zeros(22);
    }
    if (stored > in.n) {   // the empty sample that silences keys without one
      x.zeros(12);
      x.u8(0); x.u8(0); x.u8(0); x.u8(128); x.u8(0); x.u8(0);
      x.zeros(22);
    }
    for (const Smp& s : in.smp) {
      if (s.wide) {
        uint16_t prev = 0;
        for (uint32_t k = 0; k < s.len; ++k) {
          const uint16_t v = mem16[s.start + k];
          x.le16(static_cast<uint16_t>(v - prev));
          prev = v;
        }
      } else {
        uint8_t prev = 0;
        for (uint32_t k = 0; k < s.len; ++k) {
          const uint8_t v = mem8[s.start + k];
          x.u8(static_cast<uint8_t>(v - prev));
          prev = v;
        }
      }
    }
  }
  return x.v;
}

}  // namespace

extern "C" int pcw_mxm_open(const char*, const uint8_t* data, size_t size, int track,
                            pc_info* info) {
  try {
    const std::vector<uint8_t> xm = convert(In(data, size));
    if (const char* dump = std::getenv("BBMP_MXM_XM"); dump && *dump) {
      if (FILE* out = std::fopen(dump, "wb")) {
        std::fwrite(xm.data(), 1, xm.size(), out);
        std::fclose(out);
      }
    }
    if (!pcw_openmpt_open(xm.data(), xm.size(), track, "Cubic Tiny XM (MXM)", info)) return 0;
    info->n_names = 0;
    return 1;
  } catch (const std::exception& e) {
    info->n_names = 0;
    std::snprintf(info->error, sizeof(info->error), "%s", e.what());
    return 0;
  }
}

extern "C" int pcw_mxm_render(float* out, int frames) { return pcw_openmpt_render(out, frames); }
extern "C" void pcw_mxm_close(void) { pcw_openmpt_close(); }
