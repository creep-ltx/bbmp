// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-pc: replayers of formats from PC trackers in one helper.
//
//   bbmp-pc <engine> <file> <track> <samplerate> [start-ms]
//
// The engines are listed in kEngines below; which of them are built in is decided
// by tools/build-pc.sh (-DWITH_<ENGINE>).
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

#include <unistd.h>

namespace {

struct Engine {
  const char* name;
  int (*open)(const char*, const uint8_t*, size_t, int, pc_info*);
  int (*render)(float*, int);
  void (*close)(void);
};

#define ENGINE(name) {#name, pcw_##name##_open, pcw_##name##_render, pcw_##name##_close},
const Engine kEngines[] = {
#include "engines.inc"
};

// Songs stop by themselves; this only catches a replayer that never reports the end.
const double kLongestSeconds = 3600.0;

std::string json(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
    else if (c < 0x20 || c == 0x7F) { char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
    else out += static_cast<char>(c);
  }
  return out + "\"";
}

// CP437's upper half; its lower half is ASCII.
const char16_t kCp437[128] = {
  0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF,
  0x00EE, 0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
  0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA,
  0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
  0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557,
  0x255D, 0x255C, 0x255B, 0x2510, 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
  0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559,
  0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
  0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4,
  0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
  0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};

// Text is handed over as the file has it. What isn't UTF-8 is read as CP437.
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
    const unsigned u = c < 0x80 ? c : kCp437[c - 0x80];
    if (u < 0x80) out += static_cast<char>(u);
    else if (u < 0x800) { out += static_cast<char>(0xC0 | (u >> 6)); out += static_cast<char>(0x80 | (u & 0x3F)); }
    else {
      out += static_cast<char>(0xE0 | (u >> 12));
      out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (u & 0x3F));
    }
  }
  return out;
}

// The helper's real stdout. File descriptor 1 is pointed at stderr in main(), so
// that whatever a replayer prints can't get into the header or the sound.
FILE* g_out = stdout;

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
      const long s = std::lrintf(std::clamp(v, -4.0f, 4.0f) * 32767.0f);
      pcm[i] = static_cast<int16_t>(std::clamp(s, -32768L, 32767L));
    }
    return std::fwrite(pcm.data(), 4, frames, g_out) == static_cast<size_t>(frames);
  }
};

// Some of the replayers trust the file. If one of them falls over a damaged file,
// the player still gets an answer instead of nothing.
int g_out_fd = STDOUT_FILENO;
volatile sig_atomic_t g_header_written = 0;

void crashed(int) {
  static const char text[] = "{\"tracks\":0,\"error\":\"the replayer crashed on this file\"}\n";
  if (!g_header_written) {
    ssize_t n = write(g_out_fd, text, sizeof(text) - 1);
    (void)n;
  }
  _exit(1);
}

const char kUsage[] = "usage: bbmp-pc <engine> <file> <track> <samplerate> [start-ms]";

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  std::fflush(stdout);
  const int real_stdout = dup(STDOUT_FILENO);
  if (real_stdout >= 0 && dup2(STDERR_FILENO, STDOUT_FILENO) >= 0) {
    if (FILE* f = fdopen(real_stdout, "wb")) { g_out = f; g_out_fd = real_stdout; }
  }
  std::signal(SIGSEGV, crashed);
  std::signal(SIGBUS, crashed);
  std::signal(SIGFPE, crashed);
  std::signal(SIGABRT, crashed);
  if (argc < 5) {
    std::fprintf(stderr, "%s\nengines:", kUsage);
    for (const Engine& e : kEngines) std::fprintf(stderr, " %s", e.name);
    std::fprintf(stderr, "\n");
    return fail(kUsage);
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

  auto info = std::make_unique<pc_info>();
  std::memset(info.get(), 0, sizeof(pc_info));
  info->rate = rate;
  const int ok = engine->open(path, data.data(), data.size(), track, info.get());

  if (!ok) return fail(info->error[0] ? info->error : "the file can't be played");
  if (info->rate <= 0 || info->tracks < 1) return fail("the replayer gave no sample rate");

  std::string names;
  for (int i = 0; i < info->n_names && i < PC_MAX_NAMES; ++i) {
    if (i) names += ',';
    names += json(utf8(info->names[i]));
  }
  std::fprintf(g_out, "{\"tracks\":%d,\"track\":%d,\"title\":%s,\"author\":%s,\"type\":%s,"
              "\"duration_ms\":%ld,\"comment\":%s,\"names\":[%s]}\n",
              info->tracks, track, json(utf8(info->title)).c_str(), json(utf8(info->author)).c_str(),
              json(utf8(info->type)).c_str(), std::max(0L, info->duration_ms),
              json(utf8(info->comment)).c_str(), names.c_str());
  std::fflush(g_out);
  g_header_written = 1;

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
    if (!src) { std::fprintf(stderr, "bbmp-pc: %s\n", src_strerror(error)); return 1; }
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
