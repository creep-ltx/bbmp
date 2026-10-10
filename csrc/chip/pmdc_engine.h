// SPDX-License-Identifier: GPL-3.0-or-later
//
// pmdc: PMD songs (.m) through the player's own PMD driver (pmdc_driver.cpp, written from
// a description of PMD's behaviour) and Aaron Giles' ymfm as the YM2608.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pmdc {

struct SongInfo {
  std::string title, composer;     // Shift-JIS, as the song has them
  std::vector<std::pair<std::string, std::string>> facts;   // Shift-JIS values
  std::string comment;             // UTF-8 (the player's own words)
  long lengthMs = 0, loopMs = 0;   // from the length scan
  long long playFrames = 0;        // before the 4-second fade
  long long totalFrames = 0;       // with the fade
};

class Player {
 public:
  Player();
  ~Player();
  // "" or why the song can't be played. rom: the YM2608 rhythm ROM (8 KB) or empty.
  std::string open(const std::vector<uint8_t>& file, int rate, const std::vector<uint8_t>& rom,
                   SongInfo& info);
  // Fills `frames` stereo frames; returns how many (fewer at the end).
  long render(int16_t* out, long frames);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The register-write log of a song, in the format of the clean-room test material
// (refs-regs/), as the old player's helper would have played it at 48 kHz. "" or an error.
std::string writeLog(const std::vector<uint8_t>& file, const std::string& logPath);

}  // namespace pmdc
