// SPDX-License-Identifier: GPL-3.0-or-later
// What libsidplayfp's C64 writes to the SID, as changes of its registers, cycle by cycle:
// the player's side of the C64 check (tools/verify/sid.py, against VICE's vsid).
//
//   sidwrites <file.sid> <tune (1-based, 0 = the file's start tune)> <cycles> [kernal basic chargen]
//
// With the three ROM files the C64 has its own ROMs (as VICE has, and the player when
// the user gives them); without, libsidplayfp's small stand-ins.
//
// Out (stdout): a line a change, "<cycle> <register> <value>", the first SID's
// registers 0-24. libsidplayfp's play(n) runs n of its scheduler's events, not n cycles;
// the time is counted by the SID's own output instead, at the highest sampling rate it
// allows (192 kHz), so <cycle> is C64 cycles, give or take five. Only changes are seen (the engine is asked for the registers after
// every cycle), so a write of the value a register already has is not, nor are two
// writes of one register in one cycle: neither changes the sound.
#include <sidplayfp/SidConfig.h>
#include <sidplayfp/SidTune.h>
#include <sidplayfp/SidTuneInfo.h>
#include <sidplayfp/builders/residfp.h>
#include <sidplayfp/sidplayfp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static std::vector<uint8_t> load(const char* path, std::size_t size)
{
  std::vector<uint8_t> out(size);
  FILE* f = std::fopen(path, "rb");
  if (!f || std::fread(out.data(), 1, size, f) != size) out.clear();
  if (f) std::fclose(f);
  return out;
}

int main(int argc, char** argv)
{
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s file.sid tune cycles\n", argv[0]);
    return 2;
  }
  SidTune tune(argv[1]);
  if (!tune.getStatus()) {
    std::fprintf(stderr, "%s\n", tune.statusString());
    return 1;
  }
  tune.selectSong(std::atoi(argv[2]));
  const unsigned long total = std::strtoul(argv[3], nullptr, 10);

  ReSIDfpBuilder builder("sidwrites");     // declared first so it outlives the engine
  sidplayfp engine;
  SidConfig cfg = engine.config();
  cfg.sidEmulation = &builder;
  // PAL's clock, or NTSC's for a tune that asks for it
  const bool ntsc = tune.getInfo()->clockSpeed() == SidTuneInfo::CLOCK_NTSC;
  const double clock = ntsc ? 1022727.0 : 985248.0;
  const double rate = 192000.0;
  cfg.frequency = static_cast<uint_least32_t>(rate);
  cfg.defaultC64Model = ntsc ? SidConfig::NTSC : SidConfig::PAL;
  cfg.powerOnDelay = 0;
  std::vector<uint8_t> kernal, basic, chargen;
  if (argc >= 7) {
    kernal = load(argv[4], 8192);
    basic = load(argv[5], 8192);
    chargen = load(argv[6], 4096);
    if (kernal.empty() || basic.empty() || chargen.empty()) {
      std::fprintf(stderr, "can't read the ROMs\n");
      return 1;
    }
    engine.setRoms(kernal.data(), basic.data(), chargen.data());
  }
  if (!engine.config(cfg) || !engine.load(&tune)) {
    std::fprintf(stderr, "%s\n", engine.error());
    return 1;
  }
  uint8_t was[32], now[32];
  std::memset(was, 0, sizeof was);
  unsigned long samples = 0, cycle = 0;
  while (cycle < total) {
    const int got = engine.play(1);
    if (got < 0) {
      std::fprintf(stderr, "%s\n", engine.error());
      return 1;
    }
    samples += got;
    cycle = static_cast<unsigned long>(samples * clock / rate + 0.5);
    if (!engine.getSidStatus(0, now)) {
      std::fprintf(stderr, "no SID status\n");
      return 1;
    }
    for (int r = 0; r < 25; r++) {
      if (now[r] != was[r]) {
        std::printf("%lu %d %d\n", cycle, r, now[r]);
        was[r] = now[r];
      }
    }
  }
  return 0;
}
