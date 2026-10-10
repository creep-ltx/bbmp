// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-misc: play one song of a format that needed its own replayer, as raw PCM on stdout.
//
//   bbmp-misc <engine> <file> <track> <samplerate> [start-ms]
//
// engine: edlib       EdLib's own songs (.edl): unpacked, converted to D00 the way
//                     EdLib does it (edl_cr.cpp), and played by AdPlug
//         goattracker GoatTracker songs (.sng, GTS! GTS2 GTS3 GTS4 GTS5), played by
//                     GoatTracker's own play routines and reSID
//         soundclub   Sound Club 1 and 2 songs (.sn, .sn2)
//
// Same protocol as the other helpers: one JSON line describing the track, then
// signed 16-bit little-endian stereo until the track ends.

#include <cstdlib>
#include <cstring>
#include <iconv.h>

#include "common.h"

namespace misc {

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

std::string convert(const char* from, const std::string& s) {
  iconv_t cd = iconv_open("UTF-8", from);
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

}  // namespace

std::string fromCp437(const std::string& s) { return convert("CP437", s); }
std::string fromLatin1(const std::string& s) {
  const std::string out = convert("CP1252", s);      // has holes, Latin-1 has none
  return out.empty() && !s.empty() ? convert("ISO-8859-1", s) : out;
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

}  // namespace misc

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: %s <edlib|goattracker|soundclub> <file> <track> <samplerate> [start-ms]\n",
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
  else return misc::fail("can't read the file");
#ifdef BBMP_EDLIB
  if (engine == "edlib") return misc::playEdlib(path, track, rate, startMs);
#endif
  if (engine == "goattracker") return misc::playGoatTracker(path, track, rate, startMs);
  if (engine == "soundclub") return misc::playSoundClub(path, track, rate, startMs);
  return misc::fail("unknown engine " + engine);
}
