// SPDX-License-Identifier: MIT
// ixs_engine: a player for iXalance songs (.ixs), the music of Shortcut Software's games.
//
// Written for bbmp from a plain description of the format and its sound (a clean-room
// rewrite); see README.txt. Under the MIT licence (LICENSE), like the rest of bbmp-rv.
//
// The engine works at 44100 Hz and makes one block of 16-bit stereo per tick.

#ifndef BBMP_IXS_ENGINE_H
#define BBMP_IXS_ENGINE_H

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ixs {

constexpr int kRate = 44100;

struct Options {
  // The folder with the three library samples (909OH.WAV, 909SD.WAV, CRASH.WAV); empty
  // when there is none. Songs that need none of them play without it.
  std::string library_dir;
  // When set, one line per tick and per sounding channel is written here.
  FILE* ticks = nullptr;
};

class Song;

class Player {
 public:
  ~Player();
  // The next block (BLK stereo frames, interleaved); false when the song is over.
  bool next(std::vector<int16_t>& block);
  // The made samples, in the store's order: u32 name length, name, u32 data length, data.
  void write_samples(FILE* to) const;

 private:
  friend std::unique_ptr<Player> open(const std::vector<uint8_t>&, const Options&, std::string&);
  Player();
  std::unique_ptr<Song> song_;
};

// Loads a song; nullptr with the reason in `error` when it is refused.
std::unique_ptr<Player> open(const std::vector<uint8_t>& file, const Options& options,
                             std::string& error);

// The title kept in the file's header (raw bytes, before any change of encoding).
std::string title(const std::vector<uint8_t>& file);

}  // namespace ixs

#endif
