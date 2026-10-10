// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-amiga: plays Amiga (and a few other) formats UADE has no player for.
//
//   bbmp-amiga <engine> <file> <track> <samplerate> [start-ms]
//
// Answers with one line of JSON about the song and then s16le stereo at the sample
// rate asked for, until the song ends. Nothing else goes to stdout. The engines are
// in the other source files of this folder; "bbmp-amiga" alone lists them.

#include "engine.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

EngineEntry*& engines() {
  static EngineEntry* first = nullptr;
  return first;
}

std::string json(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
    else if (c < 0x20 || c == 0x7f) { char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
    else out += static_cast<char>(c);
  }
  return out + "\"";
}

bool valid_utf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = s[i];
    const int more = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
    if (more < 0 || i + more >= s.size() + (more ? 0 : 1)) return false;
    for (int k = 1; k <= more; ++k)
      if ((static_cast<unsigned char>(s[i + k]) >> 6) != 2) return false;
    i += more + 1;
  }
  return true;
}

// Replayers print now and then. What they print must not get into the sound, so
// stdout is handed over to stderr, and the song goes to what stdout was.
int g_out = STDOUT_FILENO;

void keep_stdout_clean() {
  std::fflush(stdout);
  const int kept = dup(STDOUT_FILENO);
  if (kept < 0) return;
  if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
    const int null = open("/dev/null", O_WRONLY);
    if (null >= 0) { dup2(null, STDOUT_FILENO); close(null); }
  }
  g_out = kept;
}

// Everything or nothing: false when the reader has gone away.
bool write_all(const void* data, size_t size) {
  const char* p = static_cast<const char*>(data);
  while (size) {
    const ssize_t n = ::write(g_out, p, size);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

int fail(const std::string& why) {
  const std::string line = "{\"tracks\":0,\"error\":" + json(why) + "}\n";
  write_all(line.data(), line.size());
  return 1;
}

std::string usage() {
  std::string names;
  for (EngineEntry* e = engines(); e; e = e->next) names = e->name + std::string(names.empty() ? "" : "|") + names;
  return "usage: bbmp-amiga " + names + " <file> <track> <samplerate> [start-ms]";
}

}  // namespace

EngineEntry::EngineEntry(const char* n, Engine* (*m)()) : name(n), make(m), next(engines()) {
  engines() = this;
}

Engine* make_engine(const std::string& name) {
  for (EngineEntry* e = engines(); e; e = e->next)
    if (name == e->name) return e->make();
  return nullptr;
}

bool debug_on() {
  static const bool on = std::getenv("BBMP_AMIGA_DEBUG") != nullptr;
  return on;
}

std::string to_utf8(const std::string& text) {
  if (valid_utf8(text)) return text;
  std::string out;
  for (unsigned char c : text) {
    if (c < 0x80) out += static_cast<char>(c);
    else { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
  }
  return out;
}

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  keep_stdout_clean();
  if (argc < 5) {
    std::fprintf(stderr, "%s\n", usage().c_str());
    return fail(usage());
  }
  const std::string name = argv[1];
  const char* path = argv[2];
  const int track = std::atoi(argv[3]);
  const int rate = std::atoi(argv[4]);
  const long start_ms = argc > 5 ? std::atol(argv[5]) : 0;
  if (rate < 8000 || rate > 192000) return fail("sample rate out of range");
  if (track < 0) return fail("no such track");

  std::unique_ptr<Engine> engine(make_engine(name));
  if (!engine) return fail("unknown engine: " + name);

  std::vector<uint8_t> file;
  {
    FILE* f = std::fopen(path, "rb");
    if (!f) return fail(std::string("can't open the file: ") + std::strerror(errno));
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > (256L << 20)) { std::fclose(f); return fail("empty or too large"); }
    file.resize(static_cast<size_t>(size));
    const size_t got = std::fread(file.data(), 1, file.size(), f);
    std::fclose(f);
    if (got != file.size()) return fail("can't read the file");
  }

  SongInfo info;
  std::string error;
  if (!engine->open(path, file, track, rate, info, error))
    return fail(error.empty() ? "can't play this file" : error);

  std::string head = "{\"tracks\":" + std::to_string(info.tracks) + ",\"track\":" + std::to_string(track) +
                     ",\"title\":" + json(to_utf8(info.title)) + ",\"author\":" + json(to_utf8(info.author)) +
                     ",\"type\":" + json(to_utf8(info.type)) + ",\"duration_ms\":" + std::to_string(info.duration_ms) +
                     ",\"comment\":" + json(to_utf8(info.comment)) + ",\"names\":[";
  for (size_t i = 0; i < info.names.size(); ++i) head += (i ? "," : "") + json(to_utf8(info.names[i]));
  head += "]}\n";
  if (!write_all(head.data(), head.size())) return 0;

  std::vector<int16_t> buf(4096 * 2);
  long long skip = start_ms > 0 ? static_cast<long long>(start_ms) * rate / 1000 : 0;
  while (skip > 0) {
    const size_t want = static_cast<size_t>(skip < 4096 ? skip : 4096);
    const size_t got = engine->render(buf.data(), want);
    skip -= static_cast<long long>(got);
    if (got < want) return 0;
  }
  for (;;) {
    const size_t got = engine->render(buf.data(), 4096);
    if (got && !write_all(buf.data(), got * 4)) break;
    if (got < 4096) break;
  }
  return 0;
}
