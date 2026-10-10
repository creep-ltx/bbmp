// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-retro: trackers and drivers of 8-bit computers, consoles and handhelds,
// one engine per format, as raw PCM on stdout.
//
//   bbmp-retro <engine> <file> <track> <samplerate> [start-ms]
//
// One JSON line describing the track, then signed 16-bit little-endian stereo until
// the track ends. The engines are in the *_engine.cpp files beside this one.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <iconv.h>
#include <map>
#include <memory>
#include <strings.h>
#include <unistd.h>

#include "engine.h"

namespace retro {

namespace {
std::map<std::string, EngineMaker>& engines() {
  static std::map<std::string, EngineMaker> all;
  return all;
}

std::string convert(const std::string& s, const char* from, bool* ok = nullptr) {
  if (ok) *ok = false;
  iconv_t cd = iconv_open("UTF-8", from);
  if (cd == (iconv_t)-1) return s;
  std::string in(s), out(in.size() * 4 + 4, '\0');
  char* ip = in.data();
  char* op = out.data();
  size_t il = in.size(), ol = out.size();
  const bool good = iconv(cd, &ip, &il, &op, &ol) != (size_t)-1;
  iconv_close(cd);
  if (!good) return s;
  out.resize(out.size() - ol);
  if (ok) *ok = true;
  return out;
}

bool valid_utf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = s[i];
    int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
    if (n < 0 || i + n >= s.size() + (n == 0)) return false;
    for (int k = 1; k <= n; k++)
      if ((static_cast<unsigned char>(s[i + k]) >> 6) != 2) return false;
    i += n + 1;
  }
  return true;
}

std::string json(const std::string& text) {
  const std::string s = valid_utf8(text) ? text : from_latin1(text);
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

void register_engine(const char* name, EngineMaker maker) { engines()[name] = maker; }

std::string from_latin1(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else {
      out += static_cast<char>(0xc0 | c >> 6);
      out += static_cast<char>(0x80 | (c & 0x3f));
    }
  }
  return out;
}

std::string from_cp437(const std::string& s) {
  bool ok;
  const std::string out = convert(s, "CP437", &ok);
  return ok ? out : from_latin1(s);
}

std::string from_sjis(const std::string& s) {
  bool ok;
  const std::string out = convert(s, "CP932", &ok);
  return ok ? out : from_latin1(s);
}

std::string from_petscii(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (c >= 0x41 && c <= 0x5a) out += static_cast<char>(c + 0x20);        // unshifted: lower case
    else if (c >= 0xc1 && c <= 0xda) out += static_cast<char>(c - 0x80);   // shifted: upper case
    else if (c >= 0x61 && c <= 0x7a) out += static_cast<char>(c - 0x20);
    else if (c >= 0x20 && c < 0x7f) out += static_cast<char>(c);
    else out += ' ';
  }
  return out;
}

std::string to_utf8(const std::string& s) {
  if (valid_utf8(s)) return s;
  return from_sjis(s);
}

std::string field(const void* p, size_t size) {
  const char* c = static_cast<const char*>(p);
  size_t n = 0;
  while (n < size && c[n]) n++;
  while (n > 0 && (c[n - 1] == ' ' || c[n - 1] == '\t' || c[n - 1] == '\r' || c[n - 1] == '\n')) n--;
  return std::string(c, n);
}

std::vector<uint8_t> read_file(const std::string& path) {
  std::vector<uint8_t> data;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
  }
  return data;
}

std::string dir_of(const std::string& path) {
  const auto slash = path.rfind('/');
  return slash == std::string::npos ? "./" : path.substr(0, slash + 1);
}

std::string find_beside(const std::string& path, const std::string& name) {
  const std::string dir = dir_of(path);
  std::string found;
  if (DIR* d = opendir(dir.c_str())) {
    while (const dirent* e = readdir(d)) {
      if (strcasecmp(e->d_name, name.c_str()) == 0) {
        found = dir + e->d_name;
        if (name == e->d_name) break;
      }
    }
    closedir(d);
  }
  return found;
}

bool debug_on() {
  static const bool on = std::getenv("BBMP_RETRO_DEBUG") != nullptr;
  return on;
}

}  // namespace retro

int main(int argc, char** argv) {
  using namespace retro;
  if (argc < 5) {
    std::string names;
    for (const auto& e : engines()) names += (names.empty() ? "" : "|") + e.first;
    std::fprintf(stderr, "usage: %s <%s> <file> <track> <samplerate> [start-ms]\n", argv[0],
                 names.c_str());
    return 2;
  }
  // Whatever a library prints must not get into the audio: stdout becomes stderr
  // for everyone but us.
  std::fflush(stdout);
  const int realOut = dup(1);
  dup2(2, 1);
  FILE* out = fdopen(realOut, "wb");
  auto say = [&](const std::string& line) {
    std::fputs(line.c_str(), out);
    std::fputc('\n', out);
    std::fflush(out);
  };

  const std::string name = argv[1], path = argv[2];
  const int track = std::atoi(argv[3]);
  const int rate = std::atoi(argv[4]);
  const long long startMs = argc > 5 ? std::atoll(argv[5]) : 0;
  auto error = [&](const std::string& why) {
    say("{\"tracks\":0,\"error\":" + json(why) + "}");
    return 1;
  };
  const auto maker = engines().find(name);
  if (maker == engines().end()) return error("no engine called " + name);
  if (rate < 8000 || rate > 192000) return error("sample rate out of range");
  const std::vector<uint8_t> file = read_file(path);
  if (file.empty()) return error("can't read the file");

  std::unique_ptr<Engine> engine(maker->second());
  SongInfo info;
  std::string why;
  if (!engine->open(file, path, track < 0 ? 0 : track, rate, info, why))
    return error(why.empty() ? "not a file of this format" : why);
  if (info.tracks < 1) info.tracks = 1;
  if (track < 0 || track >= info.tracks) return error("no such track");

  std::string names;
  for (const auto& n : info.names) names += (names.empty() ? "" : ",") + json(n);
  char nums[64];
  std::snprintf(nums, sizeof(nums), "%lld", info.duration_ms);
  say("{\"tracks\":" + std::to_string(info.tracks) + ",\"track\":" + std::to_string(track) +
      ",\"title\":" + json(info.title) + ",\"author\":" + json(info.author) +
      ",\"type\":" + json(info.type) + ",\"duration_ms\":" + nums +
      ",\"comment\":" + json(info.comment) + ",\"names\":[" + names + "]}");

  std::vector<int16_t> buf(2 * 4096);
  long long skipFrames = startMs > 0 ? startMs * rate / 1000 : 0;
  // A song that never ends is a fault of the engine; this is the last line of defence.
  const long long limit = 3LL * 3600 * rate;
  long long total = 0;
  for (;;) {
    const size_t got = engine->render(buf.data(), 4096);
    size_t from = 0;
    if (skipFrames > 0) {
      from = skipFrames >= static_cast<long long>(got) ? got : static_cast<size_t>(skipFrames);
      skipFrames -= from;
    }
    if (got > from && std::fwrite(buf.data() + 2 * from, 4, got - from, out) != got - from) break;
    total += got;
    if (got < 4096 || total > limit) break;
  }
  std::fflush(out);
  return 0;
}
