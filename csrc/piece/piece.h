// SPDX-License-Identifier: GPL-3.0-or-later
// A replayer for songs of Aquaplus' P/ECE music driver (muslib), written anew from the
// format's documentation and from how the driver sounds. See piece.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace piece {

struct Song;

// Load a song (the whole file). Returns nullptr and sets `why` if it isn't one.
Song* load(const std::vector<uint8_t>& data, std::string& why);
void destroy(Song* s);

std::string title(const Song* s);    // the short title, as it is in the file (Shift-JIS)
std::string title2(const Song* s);   // the long one (composer, copyright)
int parts(const Song* s);            // parts with data

// The song's length: to where it ends, or to where every part has started over once
// (then `loops` is true).
struct Length {
  double seconds = 0;
  bool loops = false;
};
Length measure(const std::vector<uint8_t>& data);

// Render mono samples at 16 kHz, the driver's rate. Returns how many were made; fewer
// than asked means the song has ended.
int render16k(Song* s, float* out, int n);

}  // namespace piece
