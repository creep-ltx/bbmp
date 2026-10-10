// SPDX-License-Identifier: MIT
// bbmp-snsf: play a Super Nintendo song (SNSF, MINISNSF) with snsf9x, as raw PCM on stdout.
//
//   bbmp-snsf <file> <track> <samplerate> [start-ms]
//
// snsf9x is Snes9x trimmed for sound, and Snes9x may be passed on for non-commercial
// purposes only. Those terms can't be combined with the GPL, so this is a program of its
// own with no GPL code in it: the files written for it are under the MIT licence
// (LICENSE), and README.txt says what comes from where. It was part of bbmp-chip until
// 7 October 2026 and plays exactly as it did there.
//
// The same protocol as the player's other helpers: one JSON line describing the track,
// then signed 16-bit little-endian stereo until the track ends.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "snsf_engine.h"

namespace {

struct Info {
  int tracks = 1;
  int track = 0;
  std::string title, author, type;
  long long durationMs = 0;
  std::vector<std::pair<std::string, std::string>> facts;
  std::string comment;
};

std::string json(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

void header(const Info& i) {
  std::string facts;
  for (const auto& f : i.facts)
    if (!f.second.empty()) facts += (facts.empty() ? "" : ",") + json(f.first) + ":" + json(f.second);
  std::printf("{\"tracks\":%d,\"track\":%d,\"title\":%s,\"author\":%s,\"type\":%s,\"duration_ms\":%lld,"
              "\"facts\":{%s},\"comment\":%s,\"names\":[]}\n",
              i.tracks, i.track, json(i.title).c_str(), json(i.author).c_str(), json(i.type).c_str(),
              i.durationMs, facts.c_str(), json(i.comment).c_str());
  std::fflush(stdout);
}

int fail(const std::string& why) {
  std::printf("{\"tracks\":0,\"error\":%s}\n", json(why).c_str());
  return 1;
}

bool emit(const int16_t* stereo, size_t frames) {
  return std::fwrite(stereo, 4, frames, stdout) == frames;
}

// Skip `ms` of audio by rendering and discarding it (snes9x can't seek).
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

int playSnsf(const char* path, int rate, long long startMs) {
  snsfw_info si{};
  if (const char* err = snsfw_open(path, rate, &si)) return fail(err);
  Info info;
  info.type = "SNSF";
  info.title = si.title ? si.title : "";
  info.author = si.author ? si.author : "";
  info.facts = {{"Game", si.game}, {"Year", si.year}, {"Copyright", si.copyright}, {"Ripped by", si.ripper}};
  info.comment = si.comment;
  const long long lengthMs = si.length_ms > 0 ? si.length_ms : (2 * 60 + 50) * 1000;
  const long long fadeMs = si.fade_ms >= 0 ? si.fade_ms : 10000;
  info.durationMs = lengthMs + fadeMs;
  const long long playFrames = lengthMs * rate / 1000, fadeFrames = fadeMs * rate / 1000;
  long long pos = 0;
  auto render = [&](int16_t* out, int frames) {
    if (snsfw_render(out, frames) < 0) return false;
    for (int i = 0; i < frames; i++) {
      const long long f = pos + i - playFrames;
      if (f > 0) {
        const float g = f >= fadeFrames ? 0.0f : 1.0f - static_cast<float>(f) / fadeFrames;
        out[2 * i] = static_cast<int16_t>(out[2 * i] * g);
        out[2 * i + 1] = static_cast<int16_t>(out[2 * i + 1] * g);
      }
    }
    pos += frames;
    return pos < playFrames + fadeFrames;
  };
  if (startMs) skip(startMs, rate, render);
  header(info);
  std::vector<int16_t> stereo(2 * 4096);
  while (render(stereo.data(), 4096) && emit(stereo.data(), 4096)) {
  }
  snsfw_close();
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <file> <track> <samplerate> [start-ms]\n", argv[0]);
    return 2;
  }
  return playSnsf(argv[1], std::atoi(argv[3]), argc > 4 ? std::atoll(argv[4]) : 0);
}
