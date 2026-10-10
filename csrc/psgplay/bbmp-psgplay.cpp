// SPDX-License-Identifier: GPL-2.0-only
// bbmp-psgplay: play one tune of an Atari ST SNDH file with psgplay, as raw PCM on stdout.
//
//   bbmp-psgplay <file> <track> <samplerate> [start-ms]
//
// psgplay (Fredrik Noring) is under the GPL version 2 only, so it can't be part of the
// player's own GPL 3 code: this is a program of its own, under the GPL version 2 only
// (README.txt says what comes from where). It was part of bbmp-chip until 7 October 2026
// and plays exactly as it did there.
//
// The same protocol as the player's other helpers: one JSON line describing the track,
// then signed 16-bit little-endian stereo until the track ends.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <sys/types.h>

extern "C" {
#include "ice/ice.h"
#include "psgplay/psgplay.h"
#include "psgplay/sndh.h"
#include "psgplay/stereo.h"
}

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

std::vector<char> readFile(const char* path) {
  std::vector<char> data;
  if (FILE* f = std::fopen(path, "rb")) {
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
  }
  return data;
}

// Skip `ms` of audio by rendering and discarding it (psgplay can't seek).
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

int playSndh(const char* path, int track, int rate, long long startMs) {
  std::vector<char> file = readFile(path);
  if (file.empty()) return fail("can't read file");
  if (ice_identify(file.data(), file.size())) {
    // psgplay's unpacker doesn't check where it reads: a damaged file makes it read
    // before the packed data (from the end towards the start) and past the unpacked
    // data (a copy from up to 5.5 KB further on). So it is given room on both sides.
    // In front, zeros, as many as it can read before it has made the whole unpacked
    // size (it stops there): a literal run takes one byte for each byte it makes, and
    // each turn of its loop (at least 2 bytes made) reads at most 60 bits of codes, so
    // never more than 5 bytes for each byte made. The size comes from the file: SNDH
    // files are far smaller than 16 MiB (the largest unpacked here: 215 KiB).
    const size_t size = ice_decrunched_size(file.data(), file.size());
    if (size == 0 || size > (16u << 20)) return fail("not an SNDH file (ICE packed, size " + std::to_string(size) + ")");
    const size_t front = 5 * size + 64;
    std::vector<char> in(front + file.size()), out(size + 8192);
    std::memcpy(in.data() + front, file.data(), file.size());
    if (ice_decrunch(out.data(), in.data() + front, file.size()) == -1) return fail("ICE decrunch failed");
    out.resize(size);
    file.swap(out);
  }
  if (!sndh_identify(file.data(), file.size())) return fail("not an SNDH file");
  Info info;
  info.type = "SNDH";
  int count = 1;
  sndh_tag_subtune_count(&count, file.data(), file.size());
  info.tracks = count > 0 ? count : 1;
  info.track = track;
  char buf[256] = "";
  if (sndh_tag_title(buf, sizeof(buf), file.data(), file.size())) info.title = buf;
  buf[0] = 0;
  if (sndh_tag_composer(buf, sizeof(buf), file.data(), file.size())) info.author = buf;
  buf[0] = 0;
  if (sndh_tag_year(buf, sizeof(buf), file.data(), file.size())) info.facts.push_back({"Year", buf});
  float seconds = 0;
  if (sndh_tag_subtune_time(&seconds, track + 1, file.data(), file.size())) info.durationMs = seconds * 1000;
  struct psgplay* pp = psgplay_init(file.data(), file.size(), track + 1, rate);
  if (!pp) return fail("psgplay can't start the tune");
  auto render = [&](int16_t* out, int frames) {
    return psgplay_read_stereo(pp, reinterpret_cast<psgplay_stereo*>(out), frames) > 0;
  };
  if (startMs) skip(startMs, rate, render);
  header(info);
  std::vector<int16_t> stereo(2 * 4096);
  for (;;) {
    const ssize_t n = psgplay_read_stereo(pp, reinterpret_cast<psgplay_stereo*>(stereo.data()), 4096);
    if (n <= 0) break;
    for (ssize_t i = 0; i < 2 * n; i++) stereo[i] = stereo[i] / 4;  // psgplay runs about 12 dB hotter
    if (!emit(stereo.data(), n)) break;
  }
  psgplay_free(pp);
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <file> <track> <samplerate> [start-ms]\n", argv[0]);
    return 2;
  }
  return playSndh(argv[1], std::atoi(argv[2]), std::atoi(argv[3]), argc > 4 ? std::atoll(argv[4]) : 0);
}
