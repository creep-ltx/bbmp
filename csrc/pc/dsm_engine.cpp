// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-pc, engine "dsm": the first module format of the Digital Sound Interface
// Kit (DSIK, by Carlos Hasan), "DSM" + 0x10, which DSIK's converter made of MOD, S3M
// and other modules. DSIK's later format (RIFF DSMF) is played by libopenmpt, and
// holds the same things in another order, so the file is rewritten as RIFF DSMF in
// memory and handed to libopenmpt, which plays it by DSIK's rules.
//
// The old format, worked out from the files and their S3M originals, and the same
// as MilkyTracker's loader (LoaderDSM.cpp) reads it:
//   0x00 "DSM" 0x10   0x04 title[28]   0x24 0x1A
//   0x25 channels  0x26 orders  0x27 instruments  0x28 patterns
//   0x29 speed  0x2A tempo  0x2B global volume  0x2C.. 4 bytes not understood (20 00 30 00)
//   0x30 orders[128], 0xFF after the end
//   0xB0 words: instruments, then patterns, each the file offset / 16
//   instrument, 64 bytes: "DSI" 0x10, name[30], 0, sample data offset / 16 as byte (high)
//     and word, length, loop start, loop end (0: no loop), rate of the middle C,
//     volume 0..64. Samples are 8 bit signed.
//   pattern: word length (itself included), then 64 rows: a row is a list of events
//     and ends with 0. An event begins with a byte of flags: 0x80 a note follows,
//     0x40 an instrument, 0x20 a volume, 0x10 a command and its value; the low four
//     bits are the channel. Every byte of flags (the 0 too) is stored XOR the low
//     byte of its own offset in the pattern + 1. Notes: 25 is the middle C.
//     Commands are those of a MOD, with 8 as panning (0..0x80, 0xA4 surround).

#include "bytes.h"
#include "engine.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using pcbytes::Bad;
using pcbytes::In;
using pcbytes::Out;

void chunk(Out& out, const char* name, const Out& body) {
  out.bytes(name, 4);
  out.le32(static_cast<uint32_t>(body.size()));
  out.bytes(body.v.data(), body.size());
}

std::vector<uint8_t> convert(const In& in, pc_info* info) {
  if (in.size() < 0xB0 || std::memcmp(in.at(0, 4), "DSM\x10", 4) != 0)
    throw Bad("not a module of the old DSIK format");
  const unsigned channels = in.u8(0x25), orders = in.u8(0x26), instruments = in.u8(0x27);
  const unsigned patterns = in.u8(0x28), speed = in.u8(0x29), tempo = in.u8(0x2A);
  const unsigned volume = in.u8(0x2B);
  if (channels < 1 || channels > 16 || orders > 128 || speed == 0 || tempo < 32)
    throw Bad("the DSIK module's header makes no sense");
  const std::string title = in.text(4, 28);
  std::snprintf(info->title, sizeof(info->title), "%s", title.c_str());

  Out riff;
  riff.bytes("RIFF", 4);
  riff.le32(0);
  riff.bytes("DSMF", 4);

  Out song;
  song.text(title, 28);
  song.le16(0);                       // version
  song.le16(0);                       // flags
  song.le16(0);                       // order position
  song.le16(0);                       // restart position
  song.le16(orders);
  song.le16(instruments);
  song.le16(patterns);
  song.le16(channels);
  song.u8(volume > 64 ? 64 : volume);
  song.u8(0x80 | 0x30);               // stereo, and ScreamTracker's usual master volume
  song.u8(speed);
  song.u8(tempo);
  // The old format has no panning in its header; DSIK's converter gave the later
  // format left right right left. The songs set their panning themselves (command 8).
  for (unsigned c = 0; c < 16; ++c) song.u8((c & 3) == 1 || (c & 3) == 2 ? 0x80 : 0x00);
  for (unsigned i = 0; i < 128; ++i) song.u8(i < orders ? in.u8(0x30 + i) : 0xFF);
  chunk(riff, "SONG", song);

  for (unsigned p = 0; p < patterns; ++p) {
    const size_t at = static_cast<size_t>(in.le16(0xB0 + 2 * (instruments + p))) * 16;
    Out patt;
    patt.le16(0);
    unsigned rows = 0;
    if (at) {
      const size_t len = in.le16(at);
      if (len < 2) throw Bad("a pattern of the DSIK module is damaged");
      const uint8_t* d = in.at(at, len);
      size_t i = 2;
      while (i < len && rows < 64) {
        const unsigned flags = d[i] ^ ((i + 1) & 0xFF);
        ++i;
        patt.u8(flags);
        if (!flags) { ++rows; continue; }
        const size_t more = ((flags & 0x80) ? 1 : 0) + ((flags & 0x40) ? 1 : 0) +
                            ((flags & 0x20) ? 1 : 0) + ((flags & 0x10) ? 2 : 0);
        if (i + more > len) throw Bad("a pattern of the DSIK module is damaged");
        if (flags & 0x80) {
          const unsigned note = d[i++];
          // 25 is the middle C here, 49 in the later format
          patt.u8(note ? (note + 24 > 108 ? 108 : note + 24) : 0);
        }
        if (flags & 0x40) patt.u8(d[i++]);
        if (flags & 0x20) patt.u8(d[i++]);
        if (flags & 0x10) { patt.u8(d[i++]); patt.u8(d[i++]); }
      }
    }
    for (; rows < 64; ++rows) patt.u8(0);
    patt.put16(0, static_cast<unsigned>(patt.size()));
    chunk(riff, "PATT", patt);
  }

  for (unsigned n = 0; n < instruments; ++n) {
    const size_t at = static_cast<size_t>(in.le16(0xB0 + 2 * n)) * 16;
    Out inst;
    std::string name;
    uint32_t length = 0, loop_start = 0, loop_end = 0, rate = 8363, vol = 0;
    size_t data = 0;
    if (at) {
      if (std::memcmp(in.at(at, 4), "DSI\x10", 4) != 0)
        throw Bad("an instrument of the DSIK module is damaged");
      name = in.text(at + 4, 28);
      data = (static_cast<size_t>(in.u8(at + 0x23)) << 16 | in.le16(at + 0x24)) * 16;
      length = in.le16(at + 0x26);
      loop_start = in.le16(at + 0x28);
      loop_end = in.le16(at + 0x2A);
      rate = in.le16(at + 0x2C);
      vol = in.u8(at + 0x2E);
      if (!data) length = 0;
      if (length && data + length > in.size()) {
        if (data >= in.size()) throw Bad("the samples of the DSIK module are missing");
        length = static_cast<uint32_t>(in.size() - data);
      }
      if (loop_end > length) loop_end = length;
      if (loop_start >= loop_end) loop_start = loop_end = 0;
    }
    if (info->n_names < PC_MAX_NAMES)
      std::snprintf(info->names[info->n_names++], sizeof(info->names[0]), "%s", name.c_str());
    inst.text("", 13);
    inst.le16((loop_end ? 1 : 0) | 2);        // looped, signed
    inst.u8(vol > 64 ? 64 : vol);
    inst.le32(length);
    inst.le32(loop_start);
    inst.le32(loop_end);
    inst.le32(0);
    inst.le32(rate);
    inst.text(name, 28);
    if (length) inst.bytes(in.at(data, length), length);
    chunk(riff, "INST", inst);
  }
  riff.put32(4, static_cast<uint32_t>(riff.size() - 8));
  return riff.v;
}

}  // namespace

extern "C" int pcw_dsm_open(const char*, const uint8_t* data, size_t size, int track,
                            pc_info* info) {
  try {
    const std::vector<uint8_t> module = convert(In(data, size), info);
    return pcw_openmpt_open(module.data(), module.size(), track,
                            "Digital Sound Interface Kit (DSM 1)", info);
  } catch (const std::exception& e) {
    info->n_names = 0;
    std::snprintf(info->error, sizeof(info->error), "%s", e.what());
    return 0;
  }
}

extern "C" int pcw_dsm_render(float* out, int frames) { return pcw_openmpt_render(out, frames); }
extern "C" void pcw_dsm_close(void) { pcw_openmpt_close(); }
