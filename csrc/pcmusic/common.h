// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-pcmusic: what the engines share: the JSON header line and the PCM output.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace pcmusic {

struct Info {
  int tracks = 1;
  int track = 0;
  std::string title, author, type;
  long long durationMs = 0;
  std::vector<std::pair<std::string, std::string>> facts;   // what else the file says
  std::string comment;
  std::vector<std::string> names;                           // instrument names
};

// Text from old PC programs is CP437; everything leaves as UTF-8.
std::string fromCp437(const std::string& s);
// A fixed-size text field: up to the first NUL, without trailing blanks.
std::string field(const void* p, size_t size);

void header(const Info& info);
int fail(const std::string& why);
bool emit(const int16_t* stereo, size_t frames);
std::vector<uint8_t> readFile(const char* path);

inline int16_t clip16(double v) {
  return static_cast<int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

int playMonotone(const char* path, int track, int rate, long long startMs);
int playDigitrekker(const char* path, int track, int rate, long long startMs);

}  // namespace pcmusic
