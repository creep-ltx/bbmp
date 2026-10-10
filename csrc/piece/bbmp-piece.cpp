// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-piece: play a song of Aquaplus' P/ECE music driver (.pmd) as raw PCM on stdout.
//
//   bbmp-piece <file> <track> <samplerate> [start-ms]
//
// Same protocol as the other helpers: one JSON line describing the track, then signed
// 16-bit little-endian stereo until the track ends. A song that starts over is played to
// where every part has started over once, then 8 seconds more, fading out.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iconv.h>
#include <string>
#include <vector>

#include "piece.h"

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

// The titles are Shift-JIS (the P/ECE's own); they leave as UTF-8.
std::string fromSjis(const std::string& s) {
  if (s.empty()) return s;
  iconv_t cd = iconv_open("UTF-8", "CP932");
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

int fail(const std::string& why) {
  std::printf("{\"tracks\":0,\"error\":%s}\n", json(why).c_str());
  std::fflush(stdout);
  return 1;
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

constexpr double FADE = 8.0;

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <file> <track> <samplerate> [start-ms]\n", argv[0]);
    return 2;
  }
  const char* path = argv[1];
  int rate = std::atoi(argv[3]);
  if (rate < 8000 || rate > 192000) rate = 48000;
  long long startMs = argc > 4 ? std::atoll(argv[4]) : 0;
  if (startMs < 0) startMs = 0;
  const std::vector<uint8_t> data = readFile(path);
  if (data.empty()) return fail("can't read the file");
  std::string why;
  piece::Song* song = piece::load(data, why);
  if (!song) return fail(why);
  const piece::Length len = piece::measure(data);
  const double total = len.seconds + (len.loops ? FADE : 0);

  std::string t1 = fromSjis(piece::title(song)), t2 = fromSjis(piece::title2(song));
  std::printf("{\"tracks\":1,\"track\":0,\"title\":%s,\"author\":\"\",\"type\":%s,\"duration_ms\":%lld,"
              "\"facts\":{\"parts\":%s},\"comment\":%s,\"names\":[]}\n",
              json(t1).c_str(), json("P/ECE music driver").c_str(), static_cast<long long>(total * 1000),
              json(std::to_string(piece::parts(song))).c_str(), json(t2).c_str());
  std::fflush(stdout);

  // 16 kHz from the replayer, to `rate` by straight lines between samples (as the
  // P/ECE itself doubles its 16 kHz to 32 kHz)
  const long long end = static_cast<long long>(total * rate);
  const long long fadeFrom = len.loops ? static_cast<long long>(len.seconds * rate) : end;
  const double stepIn = 16000.0 / rate;
  std::vector<float> in(4096);
  size_t have = 0, at = 0;
  bool ended = false;
  double pos = 0;             // in input samples, relative to in[at]
  float prev = 0;
  std::vector<int16_t> outBuf;
  outBuf.reserve(2 * 4096);
  const long long skip = startMs * rate / 1000;
  for (long long k = 0; k < end; k++) {
    while (pos >= 1.0 || at >= have) {
      if (at < have && pos >= 1.0) {
        prev = in[at];
        at++;
        pos -= 1.0;
        continue;
      }
      if (ended) break;
      const int got = piece::render16k(song, in.data(), static_cast<int>(in.size()));
      have = static_cast<size_t>(got);
      at = 0;
      if (got < static_cast<int>(in.size())) ended = true;
      if (got == 0) break;
    }
    if (at >= have) break;          // the song has ended
    double v = prev + (in[at] - prev) * pos;
    pos += stepIn;
    if (k < skip) continue;
    if (k > fadeFrom) v *= 1.0 - double(k - fadeFrom) / (end - fadeFrom);
    const long s = std::lround(std::fmax(-1.0, std::fmin(1.0, v)) * 32767.0);
    outBuf.push_back(static_cast<int16_t>(s));
    outBuf.push_back(static_cast<int16_t>(s));
    if (outBuf.size() >= 2 * 4096) {
      if (std::fwrite(outBuf.data(), 4, outBuf.size() / 2, stdout) != outBuf.size() / 2) break;
      outBuf.clear();
    }
  }
  if (!outBuf.empty()) std::fwrite(outBuf.data(), 4, outBuf.size() / 2, stdout);
  piece::destroy(song);
  return 0;
}
