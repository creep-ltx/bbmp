// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-synth: replayers of small synthesizer and tracker formats in one helper.
//
//   bbmp-synth <engine> <file> <track> <samplerate> [start-ms]
//
// Engines: protrekkr (ProTrekkr, NoiseTrekker: .ptk .ntk), kdm (Ken's Digital Music
// .kdm), skale (Skale Tracker: .skm), buz (Buzzic 1 and 2: .buz .buz2), jxs (JayTrax
// .jxs), sbstudio (.pac). kdm, buz and jxs are written for this player (clean room).
//
// On stdout: one line of JSON about the song, then s16le stereo at the rate asked
// for until the song ends. Everything else goes to stderr.

#include "engine.h"

#include <samplerate.h>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <initializer_list>

#include <unistd.h>

namespace {

struct Engine {
  const char* name;
  int (*open)(const char*, const uint8_t*, size_t, int, synth_info*);
  int (*render)(float*, int);
  void (*close)(void);
};

#define ENGINE(name) {#name, synthw_##name##_open, synthw_##name##_render, synthw_##name##_close},
const Engine kEngines[] = {
#ifdef WITH_PROTREKKR
  ENGINE(protrekkr)
#endif
#ifdef WITH_KDM
  ENGINE(kdm)
#endif
#ifdef WITH_SKALE
  ENGINE(skale)
#endif
#ifdef WITH_BUZ
  ENGINE(buz)
#endif
#ifdef WITH_JXS
  ENGINE(jxs)
#endif
#ifdef WITH_SBSTUDIO
  ENGINE(sbstudio)
#endif
};

// Songs of endless formats stop by themselves; this only catches a replayer that
// never reports the end.
const double kLongestSeconds = 3600.0;

std::string json(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
    else if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
    else out += static_cast<char>(c);
  }
  return out + "\"";
}

// Text is handed over as the file has it. What isn't UTF-8 is read as Latin-1.
std::string utf8(const char* text) {
  const std::string s = text ? text : "";
  bool ok = true;
  for (size_t i = 0; i < s.size() && ok;) {
    const unsigned char c = s[i];
    const int more = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
    if (more < 0 || i + more >= s.size() + (more ? 0 : 1)) { ok = false; break; }
    for (int k = 1; k <= more; ++k) ok = ok && (static_cast<unsigned char>(s[i + k]) >> 6) == 2;
    i += more + 1;
  }
  if (ok) return s;
  std::string out;
  for (unsigned char c : s) {
    if (c < 0x80) out += static_cast<char>(c);
    else { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
  }
  return out;
}

// The helper's real stdout. File descriptor 1 is pointed at stderr in main(), so
// that whatever a replayer prints can't get into the header or the sound.
FILE* g_out = stdout;

// A replayer that crashes on a file, or doesn't come back from loading it, must not
// leave the player without an answer: before the header is out, the answer is an
// error; after it, the sound ends there.
int g_out_fd = STDOUT_FILENO;
volatile sig_atomic_t g_header_out = 0;

void on_crash(int sig) {
  if (!g_header_out) {
    const char* why = sig == SIGALRM
        ? "{\"tracks\":0,\"error\":\"the replayer doesn't come to an end loading this file\"}\n"
        : "{\"tracks\":0,\"error\":\"the replayer crashes on this file\"}\n";
    ssize_t n = write(g_out_fd, why, std::strlen(why));
    (void)n;
  }
  _exit(1);
}

int fail(const std::string& why) {
  std::fprintf(g_out, "{\"tracks\":0,\"error\":%s}\n", json(utf8(why.c_str())).c_str());
  std::fflush(g_out);
  return 1;
}

bool read_file(const char* path, std::vector<uint8_t>& data) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    data.insert(data.end(), buf, buf + n);
    if (data.size() > (512u << 20)) break;
  }
  std::fclose(f);
  return true;
}

// Writes frames of floats as s16le, leaving out the first `skip` frames.
struct Output {
  long long skip = 0;
  std::vector<int16_t> pcm;
  bool write(const float* in, long frames) {
    if (skip >= frames) { skip -= frames; return true; }
    in += skip * 2;
    frames -= skip;
    skip = 0;
    pcm.resize(frames * 2);
    for (long i = 0; i < frames * 2; ++i) {
      float v = in[i];
      if (!(v == v)) v = 0;                       // NaN
      const long s = std::lrintf(v * 32767.0f);
      pcm[i] = static_cast<int16_t>(std::clamp(s, -32768L, 32767L));
    }
    return std::fwrite(pcm.data(), 4, frames, g_out) == static_cast<size_t>(frames);
  }
};

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  std::fflush(stdout);
  const int real_stdout = dup(STDOUT_FILENO);
  if (real_stdout >= 0 && dup2(STDERR_FILENO, STDOUT_FILENO) >= 0) {
    if (FILE* f = fdopen(real_stdout, "wb")) { g_out = f; g_out_fd = real_stdout; }
  }
  for (int sig : {SIGSEGV, SIGFPE, SIGBUS, SIGILL, SIGABRT, SIGALRM}) std::signal(sig, on_crash);
  if (argc < 5) {
    std::fprintf(stderr, "usage: bbmp-synth <engine> <file> <track> <samplerate> [start-ms]\n"
                         "engines:");
    for (const Engine& e : kEngines) std::fprintf(stderr, " %s", e.name);
    std::fprintf(stderr, "\n");
    return fail("usage: bbmp-synth <engine> <file> <track> <samplerate> [start-ms]");
  }
  const Engine* engine = nullptr;
  for (const Engine& e : kEngines)
    if (std::strcmp(e.name, argv[1]) == 0) engine = &e;
  if (!engine) return fail(std::string("no engine named ") + argv[1]);
  const char* path = argv[2];
  const int track = std::atoi(argv[3]);
  const int rate = std::atoi(argv[4]);
  const long long start_ms = argc > 5 ? std::atoll(argv[5]) : 0;
  if (rate < 8000 || rate > 192000) return fail("the sample rate must be 8000 to 192000");
  if (track < 0) return fail("no such track");

  std::vector<uint8_t> data;
  if (!read_file(path, data)) return fail("the file can't be read");
  if (data.empty()) return fail("the file is empty");

  auto info = std::make_unique<synth_info>();
  std::memset(info.get(), 0, sizeof(synth_info));
  alarm(60);
  const int ok = engine->open(path, data.data(), data.size(), track, info.get());
  alarm(0);

  if (!ok) return fail(info->error[0] ? info->error : "the file can't be played");
  if (info->rate <= 0 || info->tracks < 1) return fail("the replayer gave no sample rate");

  std::string names;
  for (int i = 0; i < info->n_names; ++i) {
    if (i) names += ',';
    names += json(utf8(info->names[i]));
  }
  std::fprintf(g_out, "{\"tracks\":%d,\"track\":%d,\"title\":%s,\"author\":%s,\"type\":%s,"
              "\"duration_ms\":%ld,\"comment\":%s,\"names\":[%s]}\n",
              info->tracks, track, json(utf8(info->title)).c_str(), json(utf8(info->author)).c_str(),
              json(utf8(info->type)).c_str(), std::max(0L, info->duration_ms),
              json(utf8(info->comment)).c_str(), names.c_str());
  std::fflush(g_out);
  g_header_out = 1;

  Output out;
  out.skip = start_ms > 0 ? start_ms * rate / 1000 : 0;

  const int kBlock = 4096;
  std::vector<float> in(kBlock * 2);
  const long long longest = static_cast<long long>(kLongestSeconds * info->rate);
  long long rendered = 0;
  bool alive = true;

  if (info->rate == rate) {
    while (alive && rendered < longest) {
      const int got = engine->render(in.data(), kBlock);
      if (got > 0) alive = out.write(in.data(), got);
      rendered += got;
      if (got < kBlock) break;
    }
  } else {
    int error = 0;
    SRC_STATE* src = src_new(SRC_SINC_MEDIUM_QUALITY, 2, &error);
    if (!src) { std::fprintf(stderr, "bbmp-synth: %s\n", src_strerror(error)); return 1; }
    const double ratio = static_cast<double>(rate) / info->rate;
    std::vector<float> res(static_cast<size_t>(kBlock * ratio + 64) * 2);
    bool ended = false;
    while (alive) {
      int got = 0;
      if (!ended) {
        got = engine->render(in.data(), kBlock);
        rendered += got;
        if (got < kBlock || rendered >= longest) ended = true;
      }
      SRC_DATA d{};
      d.data_in = in.data();
      d.input_frames = got;
      d.data_out = res.data();
      d.output_frames = static_cast<long>(res.size() / 2);
      d.src_ratio = ratio;
      d.end_of_input = ended ? 1 : 0;
      long used = 0;
      for (;;) {
        d.data_in = in.data() + used * 2;
        d.input_frames = got - used;
        if (src_process(src, &d) != 0) { alive = false; break; }
        used += d.input_frames_used;
        if (d.output_frames_gen > 0 && !out.write(res.data(), d.output_frames_gen)) { alive = false; break; }
        if (used >= got && d.output_frames_gen == 0) break;
      }
      if (ended) break;
    }
    src_delete(src);
  }
  std::fflush(g_out);
  engine->close();
  return 0;
}
