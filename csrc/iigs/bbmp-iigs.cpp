// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-iigs: plays music of the Apple IIGS for Modland Player.
//
//   bbmp-iigs soundsmith   <song> <track> <samplerate> [start-ms]
//   bbmp-iigs noisetracker <module> <track> <samplerate> [start-ms]
//
// A SoundSmith song needs its wave bank "<song>.W" next to it. On stdout: one line of
// JSON about the song, then signed 16-bit little-endian stereo until the song ends.
//
// The songs are played by engines written for this player in a clean room, from a
// description of the formats only (ss_cr.cpp, ntgs_cr.cpp), on an emulation of the IIGS's
// sound chip (doc5503.h). The chip makes 27118 samples a second with the 31 oscillators
// these songs switch on; that is brought to the rate asked for with a windowed sinc.
//
// Test mode: with BBMP_IIGS_LOG=<file>, every register write and wave-memory page copy of
// the played run is listed in that file, with the clock tick, interrupt or after-sample
// step that made it (the lines are described in iigs_cr.h).

#include "iigs_cr.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

std::string json(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
    else if (c < 0x20 || c == 0x7f) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
    else if (c < 0x80) out += static_cast<char>(c);
    else { out += static_cast<char>(0xc0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3f)); }
  }
  return out + "\"";
}

int fail(const std::string& why) {
  std::printf("{\"tracks\":0,\"error\":%s}\n", json(why).c_str());
  std::fflush(stdout);
  return 1;
}

// From the chip's rate to the one asked for: 32 samples of the input under a Kaiser
// window, in 512 positions between two samples.
class Resampler {
 public:
  Resampler(double from, double to) : step_(from / to) {
    const double cutoff = to < from ? to / from : 1.0;
    for (int p = 0; p <= kPhases; ++p) {
      double sum = 0;
      for (int k = 0; k < kTaps; ++k) {
        const double x = k - (kTaps / 2 - 1) - static_cast<double>(p) / kPhases;
        const double w = 2.0 * x / kTaps;
        const double win = std::fabs(w) < 1 ? bessel(9.0 * std::sqrt(1 - w * w)) / bessel(9.0) : 0;
        const double a = M_PI * x * cutoff;
        const double v = (std::fabs(a) < 1e-9 ? 1.0 : std::sin(a) / a) * cutoff * win;
        table_[p][k] = static_cast<float>(v);
        sum += v;
      }
      for (int k = 0; k < kTaps; ++k) table_[p][k] = static_cast<float>(table_[p][k] / sum);
    }
    std::memset(l_, 0, sizeof l_);
    std::memset(r_, 0, sizeof r_);
  }

  void push(float l, float r) {
    l_[count_ & kMask] = l;
    r_[count_ & kMask] = r;
    ++count_;
  }

  // The next sample of the output, when the input has come far enough for it.
  bool pull(float& l, float& r) {
    const long long base = static_cast<long long>(at_);
    if (base + kTaps / 2 >= count_) return false;
    const double f = (at_ - base) * kPhases;
    const int p = static_cast<int>(f);
    const float mix = static_cast<float>(f - p);
    float a = 0, b = 0;
    for (int k = 0; k < kTaps; ++k) {
      const long long i = base + k - (kTaps / 2 - 1);
      if (i < 0) continue;
      const float c = table_[p][k] + (table_[p + 1][k] - table_[p][k]) * mix;
      a += l_[i & kMask] * c;
      b += r_[i & kMask] * c;
    }
    l = a;
    r = b;
    at_ += step_;
    return true;
  }

 private:
  enum { kTaps = 32, kPhases = 512, kMask = 63 };

  static double bessel(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 40; ++k) {
      term *= (x / 2 / k) * (x / 2 / k);
      sum += term;
    }
    return sum;
  }

  const double step_;
  double at_ = 0;
  long long count_ = 0;
  float table_[kPhases + 1][kTaps];
  float l_[kMask + 1], r_[kMask + 1];
};

int16_t bent(float x) {
  const float knee = 24000, room = 32767 - knee;
  const float a = std::fabs(x);
  const float y = a <= knee ? a : knee + room * std::tanh((a - knee) / room);
  return static_cast<int16_t>(std::lrintf(x < 0 ? -y : y));
}

Song* load(const std::string& engine, const std::string& path, std::string& why) {
  Bytes file;
  if (!read_file(path, file)) { why = "can't read the file"; return nullptr; }
  if (engine == "soundsmith") return load_soundsmith(path, file, why);
  if (engine == "noisetracker") return load_noisetracker(path, file, why);
  why = engine == "megatracker"
      ? "Modland's Megatracker songs are of the Atari Falcon, not the Apple IIGS: libxmp plays them"
      : "no such engine: " + engine;
  return nullptr;
}

// Songs end by themselves; this is for a file that is broken in a way nobody foresaw.
const double kLongest = 3600;

}  // namespace

bool read_file(const std::string& path, Bytes& into, size_t limit) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  into.clear();
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && into.size() < limit)
    into.insert(into.end(), buf, buf + n);
  std::fclose(f);
  return !into.empty();
}

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: %s soundsmith|noisetracker <file> <track> <samplerate> [start-ms]\n", argv[0]);
    return fail("wrong arguments");
  }
  const std::string engine = argv[1], path = argv[2];
  const int track = std::atoi(argv[3]);
  const int rate = std::atoi(argv[4]);
  const long long start_ms = argc > 5 ? std::atoll(argv[5]) : 0;
  if (rate < 8000 || rate > 192000) return fail("no such sample rate");

  std::string why;
  std::unique_ptr<Song> song(load(engine, path, why));
  if (!song) return fail(why);
  if (track != 0) return fail("the song has one track");

  // How long it is: the song once through with only the chip's clock running.
  double seconds = 0;
  int sides = 0;
  long long dry_samples = 0;
  bool dry_limit = false;
  {
    std::unique_ptr<Song> dry(load(engine, path, why));
    Doc5503* doc = new Doc5503;
    std::unique_ptr<Doc5503> keep(doc);
    dry->start(*doc);
    const double per = 1.0 / doc->rate();
    long long n = 0;
    const long long most = static_cast<long long>(kLongest * doc->rate());
    while (!dry->ended() && n < most) { doc->step_clock(); dry->idle(); ++n; }
    seconds = n * per;
    if (n >= most) seconds = 0;
    sides = doc->sides();
    dry_samples = n;
    dry_limit = n >= most;
  }

  std::string names;
  for (size_t i = 0; i < song->names.size(); ++i) names += (i ? "," : "") + json(song->names[i]);
  char ms[32];
  std::snprintf(ms, sizeof ms, "%lld", static_cast<long long>(seconds * 1000 + 0.5));
  const std::string header = "{\"tracks\":1,\"track\":0,\"title\":" + json(song->title) +
                             ",\"author\":" + json(song->author) + ",\"type\":" + json(song->type) +
                             ",\"duration_ms\":" + ms + ",\"comment\":" + json(song->comment) +
                             ",\"names\":[" + names + "]}\n";
  std::fputs(header.c_str(), stdout);
  std::fflush(stdout);

  RegLog reglog;
  if (const char* lp = std::getenv("BBMP_IIGS_LOG")) {
    reglog.f = std::fopen(lp, "w");
    if (reglog.f) {
      std::fprintf(reglog.f, "# iigs log 1: %s %s\n# dry-run samples %lld sides %d%s\nH %s",
                   engine.c_str(), path.c_str(), dry_samples, sides, dry_limit ? " (limit)" : "",
                   header.c_str());
      song->log = &reglog;
    }
  }

  std::unique_ptr<Doc5503> doc(new Doc5503);
  song->start(*doc);
  Resampler out(doc->rate(), rate);
  long long skip = start_ms > 0 ? start_ms * rate / 1000 : 0;
  // The chip's sum is wave times volume; an eighth of it is what MAME takes for full
  // scale, and few songs ever go over that. The chip itself can't: it plays its
  // oscillators one after the other. So what is over 24000 here is bent, not cut.
  const float gain = 1.0f / 8;
  // The IIGS puts every voice fully on one side. As for the Amiga's modules, the
  // player narrows that to 60 %; and a song that has all its voices on one side is
  // played in the middle, as a IIGS without a stereo card plays it.
  const float near = sides == 3 ? 0.8f : 0.5f, far = 1 - near;
  static int16_t pcm[2 * 4096];
  size_t fill = 0;
  bool ok = true;
  const long long most = static_cast<long long>(kLongest * doc->rate());
  // After the end the chip is silent; a little of it lets the filter run out.
  int tail = 64;
  long long steps = 0;
  for (long long n = 0; ok && n < most && tail > 0; ++n) {
    int32_t l = 0, r = 0;
    if (song->ended()) --tail;
    else {
      reglog.sample = n;
      ++steps;
      doc->step(l, r);
      song->idle();
    }
    out.push((l * near + r * far) * gain, (r * near + l * far) * gain);
    float a, b;
    while (out.pull(a, b)) {
      if (skip > 0) { --skip; continue; }
      pcm[fill++] = bent(a);
      pcm[fill++] = bent(b);
      if (fill == sizeof pcm / sizeof pcm[0]) {
        ok = std::fwrite(pcm, sizeof pcm[0], fill, stdout) == fill;
        fill = 0;
      }
    }
  }
  if (ok && fill) std::fwrite(pcm, sizeof pcm[0], fill, stdout);
  std::fflush(stdout);
  if (reglog.f) {
    std::fprintf(reglog.f, "END %lld\nTAIL %d\n", steps, 64 - tail);
    std::fclose(reglog.f);
  }
  return 0;
}
