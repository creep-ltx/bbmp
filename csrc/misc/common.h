// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-misc: what the engines share: the JSON header line and the PCM output.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace misc {

struct Info {
  int tracks = 1;
  int track = 0;
  std::string title, author, type;
  long long durationMs = 0;
  std::vector<std::pair<std::string, std::string>> facts;   // what else the file says
  std::string comment;
  std::vector<std::string> names;                           // instrument names
};

// Text from old PC programs is CP437 or Latin-1; everything leaves as UTF-8.
std::string fromCp437(const std::string& s);
std::string fromLatin1(const std::string& s);
// A fixed-size text field: up to the first NUL, without trailing blanks.
std::string field(const void* p, size_t size);

void header(const Info& info);
int fail(const std::string& why);
bool emit(const int16_t* stereo, size_t frames);
std::vector<uint8_t> readFile(const char* path);

// Skip `ms` of audio by rendering and discarding it.
template <class Render>
bool skip(long long ms, int rate, Render render) {
  long long frames = ms * rate / 1000;
  std::vector<int16_t> buf(2 * 4096);
  while (frames > 0) {
    const int n = frames > 4096 ? 4096 : static_cast<int>(frames);
    if (!render(buf.data(), n)) return false;
    frames -= n;
  }
  return true;
}

int playEdlib(const char* path, int track, int rate, long long startMs);
int playGoatTracker(const char* path, int track, int rate, long long startMs);
int playSoundClub(const char* path, int track, int rate, long long startMs);

}  // namespace misc
