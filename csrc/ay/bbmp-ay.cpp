// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-ay: ZX Spectrum and Amstrad CPC formats that ZXTune doesn't play.
//
//   bbmp-ay <engine> <file> <track> <samplerate> [start-ms]
//
// Engines: beepola (.bbsong), fls (Flash Tracker), fxm and amad (Fuxoft AY
// Language, plain and in an AY file), strc (Sound Tracker compiled by Raxoft, in an
// AY file), sks (Starkos), aks (Arkos Tracker) and vt2
// (Vortex Tracker II text). sks and aks are played by Arkos Tracker 3's own code
// and vt2 by ZXTune's; those are programs of their own next to this one,
// bbmp-ay-at3 and bbmp-ay-vt2, which this one starts.
//
// On stdout: one line of JSON about the song, then s16le stereo at the rate asked
// for until the song ends. Everything else goes to stderr.

#include "engine.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

struct Entry {
  const char* name;
  std::unique_ptr<ay::Engine> (*make)();
};

const Entry kEngines[] = {
#ifdef WITH_BEEPOLA
  {"beepola", ay::make_beepola},
#endif
#ifdef WITH_FLS
  {"fls", ay::make_fls},
#endif
#ifdef WITH_FXM
  {"fxm", ay::make_fxm},
  {"amad", ay::make_fxm},
#endif
#ifdef WITH_STRC
  {"strc", ay::make_strc},
#endif
};

// Engines that are programs of their own, next to this one.
struct Other {
  const char* name;
  const char* program;
};
const Other kOthers[] = {
  {"sks", "bbmp-ay-at3"},
  {"aks", "bbmp-ay-at3"},
  {"vt2", "bbmp-ay-vt2"},
};

// Songs end by themselves; this only catches an engine that never reports the end.
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

// Text is handed over as the file has it. What isn't UTF-8 is read as Latin-1.
std::string utf8(const std::string& s) {
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

int fail(const std::string& why) {
  std::fprintf(g_out, "{\"tracks\":0,\"error\":%s}\n", json(utf8(why)).c_str());
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
    if (data.size() > (64u << 20)) break;
  }
  std::fclose(f);
  return true;
}

const char kUsage[] = "usage: bbmp-ay <engine> <file> <track> <samplerate> [start-ms]";

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  if (argc >= 5) {
    for (const Other& o : kOthers) {
      if (std::strcmp(o.name, argv[1]) != 0) continue;
      std::string self = argv[0];
      char real[4096];
      const ssize_t n = readlink("/proc/self/exe", real, sizeof(real) - 1);
      if (n > 0) self.assign(real, n);
      const size_t slash = self.rfind('/');
      const std::string program = (slash == std::string::npos ? std::string() : self.substr(0, slash + 1)) + o.program;
      std::vector<char*> args(argv, argv + argc);
      args.push_back(nullptr);
      execv(program.c_str(), args.data());
      return fail(std::string(o.program) + " is not installed");
    }
  }
  std::fflush(stdout);
  const int real_stdout = dup(STDOUT_FILENO);
  if (real_stdout >= 0 && dup2(STDERR_FILENO, STDOUT_FILENO) >= 0) {
    if (FILE* f = fdopen(real_stdout, "wb")) g_out = f;
  }
  if (argc < 5) {
    std::fprintf(stderr, "%s\nengines:", kUsage);
    for (const Entry& e : kEngines) std::fprintf(stderr, " %s", e.name);
    for (const Other& o : kOthers) std::fprintf(stderr, " %s", o.name);
    std::fprintf(stderr, "\n");
    return fail(kUsage);
  }
  const Entry* entry = nullptr;
  for (const Entry& e : kEngines)
    if (std::strcmp(e.name, argv[1]) == 0) entry = &e;
  if (!entry) return fail(std::string("no engine named ") + argv[1]);
  const char* path = argv[2];
  const int track = std::atoi(argv[3]);
  const int rate = std::atoi(argv[4]);
  const long long start_ms = argc > 5 ? std::atoll(argv[5]) : 0;
  if (rate < 8000 || rate > 192000) return fail("the sample rate must be 8000 to 192000");
  if (track < 0) return fail("no such track");

  std::vector<uint8_t> data;
  if (!read_file(path, data)) return fail("the file can't be read");
  if (data.empty()) return fail("the file is empty");

  std::unique_ptr<ay::Engine> engine = entry->make();
  ay::Info info;
  try {
    engine->open(path, data, track, rate, info);
  } catch (const std::exception& e) {
    return fail(e.what()[0] ? e.what() : "the file can't be played");
  }
  if (info.tracks < 1) return fail("no tracks");
  if (track >= info.tracks) return fail("no such track");

  std::string names;
  for (size_t i = 0; i < info.names.size(); ++i) {
    if (i) names += ',';
    names += json(utf8(info.names[i]));
  }
  std::fprintf(g_out, "{\"tracks\":%d,\"track\":%d,\"title\":%s,\"author\":%s,\"type\":%s,"
               "\"duration_ms\":%ld,\"comment\":%s,\"names\":[%s]}\n",
               info.tracks, track, json(utf8(info.title)).c_str(), json(utf8(info.author)).c_str(),
               json(utf8(info.type)).c_str(), std::max(0L, info.duration_ms),
               json(utf8(info.comment)).c_str(), names.c_str());
  std::fflush(g_out);

  long long skip = start_ms > 0 ? start_ms * rate / 1000 : 0;
  const int kBlock = 4096;
  std::vector<int16_t> pcm(kBlock * 2);
  const long long longest = static_cast<long long>(kLongestSeconds * rate);
  long long rendered = 0;
  while (rendered < longest) {
    const int got = engine->render(pcm.data(), kBlock);
    if (got > 0) {
      rendered += got;
      long long from = 0;
      if (skip > 0) {
        from = std::min<long long>(skip, got);
        skip -= from;
      }
      const size_t n = static_cast<size_t>(got - from);
      if (n && std::fwrite(pcm.data() + from * 2, 4, n, g_out) != n) break;
    }
    if (got < kBlock) break;
  }
  std::fflush(g_out);
  return 0;
}
