// SPDX-License-Identifier: GPL-3.0-or-later
// shroom-samples: builds the note recordings the "shroom" engine of bbmp-retro plays.
//
//   shroom-samples <notes.wav> <shroom-notes.bin>
//
// notes.wav is data/notes.wav of Mariopants 1.1 by Brad Smith (rainwarrior): every
// instrument of Mario Paint's music editor at each of its 13 pitches, recorded from
// SNES9X at 32000 Hz, one after the other with silence between them. Mariopants cuts
// the recording into single notes with its data.py; this does the same cutting (the
// same thresholds, the same 400 samples of fade at the end), so the notes come out
// as Mariopants plays them. Only the 15 x 13 notes are kept, not the sounds of the
// editor that follow them.
//
// The file written: "SHRONOTE", then little-endian u32 version (1), rate (32000) and
// count (195), then for each note u32 offset and length in samples, then the samples
// as signed 16 bit. Note n of instrument i is entry i * 13 + n.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

const int kInstruments = 15, kPitches = 13;
const int kQuiet = 26;        // below this a sample counts as silence
const int kGap = 3200;        // this much silence ends a note
const int kTail = 400;        // faded out after the end of a note

bool quiet(int s) { return s < kQuiet && s > -kQuiet; }

void put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; i++) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <notes.wav> <shroom-notes.bin>\n", argv[0]);
    return 2;
  }
  std::vector<uint8_t> wav;
  if (FILE* f = std::fopen(argv[1], "rb")) {
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) wav.insert(wav.end(), buf, buf + n);
    std::fclose(f);
  }
  auto u16 = [&](size_t at) { return static_cast<unsigned>(wav[at] | wav[at + 1] << 8); };
  auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
  // data.py takes the header to be 44 bytes; so it must be exactly that WAV
  if (wav.size() < 44 || std::memcmp(wav.data(), "RIFF", 4) != 0 ||
      std::memcmp(wav.data() + 8, "WAVEfmt ", 8) != 0 || u32(16) != 16 || u16(20) != 1 ||
      u16(22) != 1 || u32(24) != 32000 || u16(34) != 16 ||
      std::memcmp(wav.data() + 36, "data", 4) != 0) {
    std::fprintf(stderr, "%s: not the expected WAV (PCM, mono, 16 bit, 32000 Hz)\n", argv[1]);
    return 1;
  }
  std::vector<int> s;
  for (size_t at = 44; at + 1 < wav.size(); at += 2) s.push_back(static_cast<int16_t>(u16(at)));

  struct Note { size_t offset, length; };
  std::vector<Note> notes;
  std::vector<int16_t> data;
  size_t start = 0;
  for (;;) {
    while (start < s.size() && quiet(s[start])) start++;
    if (start >= s.size()) break;
    size_t end = start + 1;
    int gap = 0;
    while (end < s.size() && gap < kGap) {
      gap = quiet(s[end]) ? gap + 1 : 0;
      end++;
    }
    end -= gap;
    if (end - start <= 5) {   // a spike of noise
      start = end;
      continue;
    }
    end += kTail;
    if (end > s.size()) end = s.size();
    const size_t length = end - start;
    notes.push_back({data.size(), length});
    for (size_t i = 0; i < length; i++) {
      int v = s[start + i];
      const size_t fromEnd = length - 1 - i;
      if (fromEnd < static_cast<size_t>(kTail)) v = static_cast<int>(fromEnd) * v / kTail;
      data.push_back(static_cast<int16_t>(v));
    }
    start = end;
  }
  const size_t wanted = kInstruments * kPitches;
  if (notes.size() < wanted) {
    std::fprintf(stderr, "%s: %zu notes found, %zu expected\n", argv[1], notes.size(), wanted);
    return 1;
  }
  notes.resize(wanted);
  data.resize(notes.back().offset + notes.back().length);

  std::vector<uint8_t> out(8);
  std::memcpy(out.data(), "SHRONOTE", 8);
  put32(out, 1);
  put32(out, 32000);
  put32(out, static_cast<uint32_t>(wanted));
  for (const Note& n : notes) {
    put32(out, static_cast<uint32_t>(n.offset));
    put32(out, static_cast<uint32_t>(n.length));
  }
  for (int16_t v : data) {
    out.push_back(static_cast<uint8_t>(v & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
  }
  FILE* f = std::fopen(argv[2], "wb");
  if (!f || std::fwrite(out.data(), 1, out.size(), f) != out.size() || std::fclose(f) != 0) {
    std::fprintf(stderr, "can't write %s\n", argv[2]);
    return 1;
  }
  std::fprintf(stderr, "%s: %zu notes, %zu samples\n", argv[2], notes.size(), data.size());
  return 0;
}
