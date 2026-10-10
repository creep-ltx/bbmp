// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-pcmusic: play one song of an old PC music program's format, as raw PCM on
// stdout.
//
//   bbmp-pcmusic <engine> <file> <track> <samplerate> [start-ms]
//
// engine: monotone     MONOTONE songs (.mon), Jim Leonard's PC speaker tracker
//         digitrekker  DigiTrekker modules (.dtm), Horst Beham's DOS tracker
//
// Same protocol as the other helpers: one JSON line describing the track, then
// signed 16-bit little-endian stereo until the track ends.

#include <cstdlib>
#include <cstring>
#include <iconv.h>

#include "common.h"

namespace pcmusic {

namespace {

std::string json(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20 || c == 0x7f) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

}  // namespace

std::string fromCp437(const std::string& s) {
  iconv_t cd = iconv_open("UTF-8", "CP437");
  if (cd == (iconv_t)-1) return std::string();
  std::string in(s), out(in.size() * 4 + 4, '\0');
  char* ip = in.data();
  char* op = out.data();
  size_t il = in.size(), ol = out.size();
  const bool ok = iconv(cd, &ip, &il, &op, &ol) != (size_t)-1;
  iconv_close(cd);
  if (!ok) return std::string();
  out.resize(out.size() - ol);
  return out;
}

std::string field(const void* p, size_t size) {
  const char* c = static_cast<const char*>(p);
  size_t n = 0;
  while (n < size && c[n]) n++;
  while (n > 0 && (c[n - 1] == ' ' || c[n - 1] == '\t')) n--;
  return std::string(c, n);
}

void header(const Info& i) {
  std::string facts, names;
  for (const auto& f : i.facts)
    if (!f.second.empty()) facts += (facts.empty() ? "" : ",") + json(f.first) + ":" + json(f.second);
  for (const auto& n : i.names) names += (names.empty() ? "" : ",") + json(n);
  std::printf("{\"tracks\":%d,\"track\":%d,\"title\":%s,\"author\":%s,\"type\":%s,\"duration_ms\":%lld,"
              "\"facts\":{%s},\"comment\":%s,\"names\":[%s]}\n",
              i.tracks, i.track, json(i.title).c_str(), json(i.author).c_str(), json(i.type).c_str(),
              i.durationMs, facts.c_str(), json(i.comment).c_str(), names.c_str());
  std::fflush(stdout);
}

int fail(const std::string& why) {
  std::printf("{\"tracks\":0,\"error\":%s}\n", json(why).c_str());
  std::fflush(stdout);
  return 1;
}

bool emit(const int16_t* stereo, size_t frames) {
  return std::fwrite(stereo, 4, frames, stdout) == frames;
}

std::vector<uint8_t> readFile(const char* path) {
  std::vector<uint8_t> data;
  if (FILE* f = std::fopen(path, "rb")) {
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
  }
  return data;
}

}  // namespace pcmusic

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: %s <monotone|digitrekker> <file> <track> <samplerate> [start-ms]\n",
                 argv[0]);
    return 2;
  }
  const std::string engine = argv[1];
  const char* path = argv[2];
  const int track = std::atoi(argv[3]);
  int rate = std::atoi(argv[4]);
  if (rate < 8000 || rate > 192000) rate = 48000;
  long long startMs = argc > 5 ? std::atoll(argv[5]) : 0;
  if (startMs < 0) startMs = 0;
  if (FILE* f = std::fopen(path, "rb")) std::fclose(f);
  else return pcmusic::fail("can't read the file");
  if (engine == "monotone") return pcmusic::playMonotone(path, track, rate, startMs);
  if (engine == "digitrekker") return pcmusic::playDigitrekker(path, track, rate, startMs);
  return pcmusic::fail("unknown engine " + engine);
}
