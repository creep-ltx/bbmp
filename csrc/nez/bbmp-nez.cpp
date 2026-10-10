// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-nez: play SGC (Sega Master System, Game Gear, ColecoVision) and GBR
// (Game Boy) rips with libnezplug, as raw PCM on stdout.
//
//   bbmp-nez <sgc|gbr> <file> <track> <samplerate> [start-ms]
//
// The first line of output is JSON describing the file and track; after it comes
// signed 16-bit little-endian stereo audio until the track ends.
//
// These rips are programs: the game's sound driver, run by an emulated Z80. They
// say nothing about how long a song is, and most songs loop forever. So before a
// song is played it is run once without sound, and the writes to the sound chip
// are written down per call of the play routine. Where that list starts to repeat
// is where the song loops; a song that ends stops writing, or writes the same
// thing for ever.
//
// SGC: a track list may lie next to the song, as Modland has them: one .m3u file
// per track ("01 Title Screen.m3u") in NEZplug's format, which names the subsong,
// its title and its length. If there are any, they are the tracks.

#include <nezplug/nezplug.h>

#include <dirent.h>
#include <strings.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const int kFadeMs = 8000;          // after the loop point
const int kTailMs = 1500;          // after the last change of a song that ends
const int kDefaultMs = 180000;     // when neither an end nor a loop is found
const int kLimitSgc = 720;         // seconds a song is run at most to find its shape
const int kLimitGbr = 360;         // (Game Boy programs take three times as long to run)
const int kGain = 0x280;           // 2.5 times: libnezplug's own level is 20 dB below full
const int kBlock = 16;             // frames rendered at a time while looking for the loop
const size_t kBiosSize = 8192;

bool debug() { return std::getenv("BBMP_NEZ_DEBUG") != nullptr; }

// ---- text -------------------------------------------------------------------

bool validUtf8(const std::string& s) {
  size_t i = 0, n = s.size();
  while (i < n) {
    unsigned char c = s[i];
    size_t more = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 9;
    if (more == 9 || i + more >= n + (more ? 0 : 1)) return false;
    for (size_t k = 1; k <= more; k++)
      if ((static_cast<unsigned char>(s[i + k]) >> 6) != 2) return false;
    if (more == 1 && c < 0xC2) return false;
    i += more + 1;
  }
  return true;
}

// Text that isn't UTF-8 is taken to be Windows-1252, which the track lists are in.
std::string toUtf8(const std::string& s) {
  if (validUtf8(s)) return s;
  static const unsigned short high[32] = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
  std::string out;
  for (unsigned char c : s) {
    unsigned u = c < 0x80 ? c : c < 0xA0 ? high[c - 0x80] : c;
    if (u < 0x80) out += static_cast<char>(u);
    else if (u < 0x800) { out += static_cast<char>(0xC0 | (u >> 6)); out += static_cast<char>(0x80 | (u & 63)); }
    else { out += static_cast<char>(0xE0 | (u >> 12)); out += static_cast<char>(0x80 | ((u >> 6) & 63));
           out += static_cast<char>(0x80 | (u & 63)); }
  }
  return out;
}

bool unknown(const std::string& s) {     // how rippers write "I don't know"
  return s == "<?>" || s == "?" || s == "<unknown>" || s == "Unknown" || s == "unknown";
}

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
    s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  return s.substr(i);
}

std::string field(const std::vector<uint8_t>& d, size_t at, size_t len) {
  if (at >= d.size()) return "";
  len = std::min(len, d.size() - at);
  std::string s(reinterpret_cast<const char*>(d.data() + at), len);
  s = s.substr(0, s.find('\0'));
  for (char& c : s) if (static_cast<unsigned char>(c) < 0x20) c = ' ';
  s = trim(toUtf8(s));
  if (unknown(s)) s.clear();
  return s;
}

std::string json(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
    else out += static_cast<char>(c);
  }
  return out + "\"";
}

int fail(const std::string& why) {
  std::printf("{\"tracks\":0,\"error\":%s}\n", json(why).c_str());
  std::fflush(stdout);
  return 1;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out, size_t limit = 64u << 20) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
    out.insert(out.end(), buf, buf + n);
    if (out.size() > limit) break;
  }
  std::fclose(f);
  return out.size() <= limit;
}

// ---- track lists ------------------------------------------------------------

struct Entry {
  std::string list;      // the .m3u file's name, which gives the order
  int song = 0;          // the subsong, from 0
  std::string title, author;
  int lengthMs = 0;      // 0: not given
  int fadeMs = -1;       // -1: not given
};

// "1:12", "0:03.5", "72" -> milliseconds; -1 when it is no time
int parseTime(std::string s) {
  s = trim(s);
  if (!s.empty() && s.back() == '-') s.pop_back();
  if (s.empty()) return -1;
  double total = 0, part = 0, scale = 0;
  bool digits = false;
  for (char c : s) {
    if (c >= '0' && c <= '9') {
      digits = true;
      if (scale > 0) { part += (c - '0') * scale; scale /= 10; }
      else part = part * 10 + (c - '0');
    } else if (c == ':' && scale == 0) { total = (total + part) * 60; part = 0; }
    else if ((c == '.' || c == ',') && scale == 0) scale = 0.1;
    else return -1;
  }
  if (!digits) return -1;
  total += part;
  if (total > 24 * 3600) return -1;
  return static_cast<int>(total * 1000 + 0.5);
}

// One line of NEZplug's list: file::TYPE,song,title,time,loop,fade,loops
bool parseLine(const std::string& line, const std::string& songName, Entry& e) {
  size_t mark = line.find("::");
  if (mark == std::string::npos) return false;
  std::string file = trim(line.substr(0, mark));
  size_t slash = file.find_last_of("/\\");
  if (slash != std::string::npos) file = file.substr(slash + 1);
  if (strcasecmp(toUtf8(file).c_str(), songName.c_str()) != 0 &&
      strcasecmp(file.c_str(), songName.c_str()) != 0) return false;
  std::vector<std::string> f(1);
  for (size_t i = mark + 2; i < line.size(); i++) {
    if (line[i] == '\\' && i + 1 < line.size()) f.back() += line[++i];
    else if (line[i] == ',') f.emplace_back();
    else f.back() += line[i];
  }
  if (f.size() < 2) return false;
  std::string n = trim(f[1]);
  if (n.empty()) return false;
  char* end = nullptr;
  long song = n[0] == '$' ? std::strtol(n.c_str() + 1, &end, 16) : std::strtol(n.c_str(), &end, 10);
  if (*end || song < 0 || song > 255) return false;
  e.song = static_cast<int>(song);
  // "Title - Composer - Game - (c) Year Company"
  std::string text = f.size() > 2 ? trim(toUtf8(f[2])) : "";
  std::vector<std::string> parts;
  size_t from = 0, at;
  while ((at = text.find(" - ", from)) != std::string::npos) {
    parts.push_back(text.substr(from, at - from));
    from = at + 3;
  }
  parts.push_back(text.substr(from));
  if (parts.size() >= 4) {
    e.author = trim(parts[parts.size() - 3]);
    e.title = parts[0];
    for (size_t i = 1; i + 3 < parts.size(); i++) e.title += " - " + parts[i];
    e.title = trim(e.title);
    if (unknown(e.author)) e.author.clear();
  } else {
    e.title = text;
  }
  if (f.size() > 3) e.lengthMs = std::max(0, parseTime(f[3]));
  if (f.size() > 5) e.fadeMs = parseTime(f[5]);
  return true;
}

// The track lists in the song's folder that are about this song, in the order of
// their file names. A list with several lines gives several tracks.
std::vector<Entry> trackLists(const std::string& path) {
  size_t slash = path.find_last_of('/');
  std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  std::vector<std::string> lists;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* de = readdir(d)) {
      std::string n = de->d_name;
      if (n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".m3u") == 0) lists.push_back(n);
    }
    closedir(d);
  }
  std::sort(lists.begin(), lists.end());
  std::vector<Entry> out;
  for (const std::string& list : lists) {
    std::vector<uint8_t> data;
    if (!readFile(dir + "/" + list, data, 1u << 20)) continue;
    std::string text(data.begin(), data.end()), line;
    text += '\n';
    for (char c : text) {
      if (c != '\n' && c != '\r') { line += c; continue; }
      line = trim(line);
      Entry e;
      if (!line.empty() && line[0] != '#' && parseLine(line, toUtf8(name), e)) {
        e.list = list;
        out.push_back(e);
      }
      line.clear();
    }
  }
  return out;
}

// ---- the subsongs of a GBR file ----------------------------------------------

struct GbrTracks { uint64_t hash; uint32_t size; const char* bits; };
const GbrTracks kGbrTracks[] = {
#include "gbr_tracks.inc"
  {0, 0, ""},
};

// A GBR file doesn't say which of its 256 subsongs are songs. For Modland's files
// that was found out beforehand (gbr-tracks.py); of any other file all are tracks.
std::vector<int> gbrTracks(const std::vector<uint8_t>& data) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (uint8_t b : data) h = (h ^ b) * 0x100000001b3ull;
  std::vector<int> out;
  for (const GbrTracks& t : kGbrTracks) {
    if (t.hash != h || t.size != data.size() || std::strlen(t.bits) != 64) continue;
    for (int s = 0; s < 256; s++) {
      char c = t.bits[s >> 2];
      int v = c <= '9' ? c - '0' : c - 'a' + 10;
      if (v & (8 >> (s & 3))) out.push_back(s);
    }
    break;
  }
  if (out.empty())
    for (int s = 0; s < 256; s++) out.push_back(s);
  return out;
}

// ---- the emulator -----------------------------------------------------------

// What the play routine did in one call. Pitches are kept apart from the rest: a
// vibrato that runs on its own clock makes them differ a little from one time
// round the loop to the next, while everything else repeats exactly.
const int kPitches = 12;
struct Pitches { uint16_t v[kPitches]; };

std::vector<uint64_t> gFrames;     // per call: what it wrote apart from pitches, hashed
std::vector<Pitches> gPitch;       // per call: the pitches after it
std::vector<uint32_t> gFrameAt;    // per call: when, in frames of sound
uint64_t gHash = 0;
uint64_t gWrites = 0;
uint32_t gNow = 0;
bool gListen = false;
bool gGameBoy = false;
Pitches gNowPitch;
int gLatch = 0, gFmReg = 0;

void strict(uint32_t what, uint32_t value) {
  gHash = (gHash ^ (what << 8 | (value & 0xff))) * 0x100000001b3ull + 0x9e3779b97f4a7c15ull;
}

void onWrite(uint32_t addr, uint32_t value) {
  if (!gListen) return;
  gWrites++;
  value &= 0xff;
  if (gGameBoy) {
    // NR13/14, NR23/24, NR33/34: 11 bits of pitch
    int ch = addr == 0xff13 || addr == 0xff14 ? 0 : addr == 0xff18 || addr == 0xff19 ? 1
           : addr == 0xff1d || addr == 0xff1e ? 2 : -1;
    if (ch < 0) { strict(addr, value); return; }
    uint16_t& f = gNowPitch.v[ch];
    if (addr & 1) { f = (f & 0x700) | value; return; }     // ff13, ff1d (ff18 is even)
    if (addr == 0xff18) { f = (f & 0x700) | value; return; }
    f = (f & 0xff) | ((value & 7) << 8);
    strict(addr, value & 0xc0);                             // start the note, use the length
    return;
  }
  addr &= 0xff;
  if ((addr & 0xfe) == 0xf0) {                              // YM2413
    if (!(addr & 1)) { gFmReg = value; return; }
    if (gFmReg >= 0x10 && gFmReg <= 0x18) gNowPitch.v[3 + gFmReg - 0x10] = value;
    else strict(0x100 | gFmReg, value);
    return;
  }
  if (addr == 0x06) { strict(0x200, value); return; }       // Game Gear stereo
  // SN76489
  if (value & 0x80) {
    gLatch = (value >> 4) & 7;
    if ((gLatch & 1) || gLatch == 6) strict(0x300 | gLatch, value & 15);
    else gNowPitch.v[gLatch >> 1] = (gNowPitch.v[gLatch >> 1] & 0x3f0) | (value & 15);
  } else {
    if ((gLatch & 1) || gLatch == 6) strict(0x300 | gLatch, value & 15);
    else gNowPitch.v[gLatch >> 1] = (gNowPitch.v[gLatch >> 1] & 15) | ((value & 0x3f) << 4);
  }
}

void onFrame() {
  if (!gListen) return;
  gFrames.push_back(gHash);
  gPitch.push_back(gNowPitch);
  gFrameAt.push_back(gNow);
  gHash = 0;
}

// Are two pitches the same note? Within 3 %, half a semitone.
bool samePitch(const Pitches& a, const Pitches& b) {
  for (int i = 0; i < kPitches; i++) {
    int x = a.v[i], y = b.v[i];
    if (gGameBoy && i < 3) { x = 2048 - x; y = 2048 - y; }
    int d = x > y ? x - y : y - x;
    if (d > 1 && d * 100 > 3 * std::max(x, y)) return false;
  }
  return true;
}

struct Player {
  NEZ_PLAY* nez = nullptr;
  ~Player() { if (nez) NEZDelete(nez); }

  // `song` from 0
  bool open(const std::vector<uint8_t>& data, int song, int rate, const std::string& bios) {
    nez = NEZNew();
    if (!nez) return false;
    biosPath = bios;
    nez->sgc_config.coleco_bios_path = const_cast<char*>(biosPath.c_str());
    if (NEZLoad(nez, data.data(), static_cast<uint32_t>(data.size())) != NEZ_NESERR_NOERROR) {
      // NEZLoad has released what it had; NEZDelete would do it again
      nez = nullptr;
      return false;
    }
    NEZSetFrequency(nez, rate);
    NEZSetChannel(nez, 2);
    NEZGain(nez, kGain);
    // no filter; and this is what makes libnezplug start its DC filter from the first
    // sample instead of from zero, which is a thump at the start of every song
    NEZSetFilter(nez, 0);
    NEZSetSongNo(nez, song + 1);
    NEZReset(nez);
    return true;
  }
  void render(int16_t* out, int frames) { NEZRender(nez, out, frames); }

  std::string biosPath;
};

struct Shape {
  enum Kind { Unknown, Silent, Ends, Holds, Loops } kind = Unknown;
  double end = 0;          // seconds: the end, or where the song has been played once
  const char* how = "";    // what told us
  // what was seen, for the scan command
  double scanned = 0;      // seconds
  size_t calls = 0;        // of the play routine
  uint64_t writes = 0;     // to the sound chip
  size_t start = 0, period = 0;   // in calls: the writes repeat from `start` every `period`
  double repeatsFrom = 0, repeatsAt = 0;   // the same in seconds; repeatsAt 0: they don't
  double lastFresh = 0;    // seconds: when the program last read a byte it had not read before
  uint32_t fresh = 0;      // how many such bytes
  uint64_t begins = 0;     // what the first seconds wrote, hashed
  double level = 0;        // of the first seconds
};

const char* kindName(Shape::Kind k) {
  static const char* const names[] = {"unknown", "silent", "ends", "holds", "loops"};
  return names[k];
}

const int kScanRate = 8000;        // the song is run at this rate to find its shape
const int kWindow = 400;           // frames: 50 ms
const double kQuiet = 12.0;        // a window below this level is silence
const int kBegin = 10;             // seconds that "begins" and "level" are about

// Run the song once, quickly, and find its end or its loop. The program does the
// same at any sample rate, only the sound is coarser; the sound chip is run too,
// because Game Boy drivers can ask it whether a channel is still sounding.
//
// Two things tell where a song loops. The writes to the sound chip repeat: exactly
// so in many drivers, but where the tempo is a fraction of the frame rate the
// notes fall a frame earlier or later each time round, and the writes only repeat
// after several rounds. And the program stops reading new bytes: a song's data is
// read from its start to its end, and after that only what was read before. The
// second is right more often (9 in 10 of the songs whose length Modland's track
// lists give, against 6 in 10), so it decides, unless the writes repeat in a way
// that says otherwise.
//
// `seconds` 0: as long as it takes, up to `limit` seconds.
Shape findShape(const std::vector<uint8_t>& data, int song, const std::string& bios,
                int limit, int seconds = 0) {
  Shape shape;
  const int rate = kScanRate;
  gFrames.clear(); gPitch.clear(); gFrameAt.clear(); gHash = 0; gWrites = 0; gNow = 0;
  gNowPitch = Pitches(); gLatch = 0; gFmReg = 0;
  gGameBoy = data.size() > 4 && std::memcmp(data.data(), "GBRF", 4) == 0;
  gListen = true;
  std::vector<uint8_t> seen(4u << 20, 0);
  nez_hook_seen = seen.data();
  nez_hook_seen_size = static_cast<uint32_t>(seen.size());
  nez_hook_fresh = 0;
  struct Done { ~Done() { gListen = false; nez_hook_seen = nullptr; } } done;
  Player p;
  if (!p.open(data, song, rate, bios)) return shape;
  uint32_t fresh = nez_hook_fresh, lastFresh = 0;
  std::vector<int16_t> scratch(kBlock * 2);
  std::vector<float> loud;           // per window
  double power = 0, beginPower = 0;
  int filled = 0;

  // the last window with sound in it before `before`, or `none`
  const size_t none = static_cast<size_t>(-1);
  auto lastLoud = [&](size_t before) {
    for (size_t i = std::min(before, loud.size()); i-- > 0;)
      if (loud[i] >= kQuiet) return i;
    return none;
  };
  auto levelOf = [&](double from, double to) {
    size_t w0 = std::min(loud.size(), static_cast<size_t>(from * rate / kWindow));
    size_t w1 = std::min(loud.size(), static_cast<size_t>(to * rate / kWindow) + 1);
    double sum = 0;
    for (size_t i = w0; i < w1; i++) sum += static_cast<double>(loud[i]) * loud[i];
    return w1 > w0 ? std::sqrt(sum / (w1 - w0)) : 0.0;
  };
  auto endsAfter = [&](size_t window, const char* how) {
    shape.kind = window == none ? Shape::Silent : Shape::Ends;
    shape.end = window == none ? 1.0 : static_cast<double>(window + 1) * kWindow / rate + 0.25;
    shape.how = how;
  };

  for (int stage = 90; ; stage *= 2) {
    const int length = seconds ? seconds : std::min(stage, limit);
    const bool last = seconds || length >= limit;
    const uint32_t until = static_cast<uint32_t>(length) * rate;
    while (gNow < until) {
      p.render(scratch.data(), kBlock);
      for (int i = 0; i < kBlock * 2; i++) power += static_cast<double>(scratch[i]) * scratch[i];
      gNow += kBlock;
      if (nez_hook_fresh != fresh) { fresh = nez_hook_fresh; lastFresh = gNow; }
      filled += kBlock;
      if (filled >= kWindow) {
        if (gNow <= static_cast<uint32_t>(rate) * kBegin) beginPower += power;
        loud.push_back(static_cast<float>(std::sqrt(power / (filled * 2))));
        power = 0;
        filled = 0;
      }
    }
    const size_t n = gFrames.size();
    const size_t windows = loud.size();
    const double ran = static_cast<double>(gNow) / rate;
    shape.scanned = ran;
    shape.calls = n;
    shape.writes = gWrites;
    shape.lastFresh = static_cast<double>(lastFresh) / rate;
    shape.fresh = fresh;
    shape.level = std::sqrt(beginPower / (static_cast<double>(std::min<uint32_t>(gNow, rate * kBegin)) * 2));
    shape.begins = 0;
    for (size_t i = 0; i < n && gFrameAt[i] < static_cast<uint32_t>(rate) * kBegin; i++) {
      shape.begins = (shape.begins ^ gFrames[i]) * 0x100000001b3ull + 1;
      for (int k = 0; k < kPitches; k++)
        shape.begins = (shape.begins ^ gPitch[i].v[k]) * 0x100000001b3ull;
    }
    shape.kind = Shape::Unknown;
    shape.period = shape.start = 0;
    shape.repeatsFrom = shape.repeatsAt = 0;

    // silence for the second half of what was run: the song has ended
    const size_t heard = lastLoud(windows);
    if (heard == none || heard < windows / 2) {
      endsAfter(heard, "silence");
      // nothing at all yet: a song may start late, look a little further
      if (heard == none && !last && stage < 180 && n >= 16) continue;
      break;
    }
    if (n < 16) break;      // the play routine isn't being called

    // Do the writes repeat? Z algorithm on the list read backwards: z[p] is how far
    // the end of the list repeats what came p calls earlier. The loop is the
    // shortest period that holds for the second half of what was run and is seen
    // twice at least: a bar played four times must not pass for one. The pitches
    // have to agree too, but for a call in fifty.
    {
      std::vector<uint64_t> r(gFrames.rbegin(), gFrames.rend());
      std::vector<size_t> z(n, 0);
      for (size_t i = 1, l = 0, rr = 0; i < n; i++) {
        if (i < rr) z[i] = std::min(rr - i, z[i - l]);
        while (i + z[i] < n && r[z[i]] == r[i + z[i]]) z[i]++;
        if (i + z[i] > rr) { l = i; rr = i + z[i]; }
      }
      for (size_t period = 1; period <= n / 2; period++) {
        size_t tail = std::min(n, z[period] + period);
        if (tail < 2 * period || tail < n / 2) continue;
        size_t bad = 0, checked = 0, from = n - tail;
        for (size_t i = n - period; i-- > n - tail;) {
          checked++;
          if (samePitch(gPitch[i], gPitch[i + period])) continue;
          if (++bad > checked / 50 + 8) { from = i + 1; break; }
        }
        tail = n - from;
        if (tail < 2 * period || tail < n / 2) continue;
        shape.period = period;
        shape.start = from;
        shape.repeatsFrom = static_cast<double>(gFrameAt[from]) / rate;
        shape.repeatsAt = static_cast<double>(gFrameAt[from + period]) / rate;
        break;
      }
    }
    const bool repeats = shape.period > 0;
    const double span = shape.repeatsAt - shape.repeatsFrom;
    // nothing new was read for as long again as the song had taken
    const bool readOut = shape.lastFresh <= ran / 2;
    if (!repeats && !readOut) {
      if (last) break;
      continue;
    }

    if (repeats && span < 0.5) {
      // the same every call: nothing happens any more
      if (levelOf(shape.repeatsFrom, ran) < kQuiet) {
        endsAfter(lastLoud(windows), "silence");
      } else {
        shape.kind = Shape::Holds;
        shape.end = shape.repeatsFrom + kTailMs / 1000.0;
        shape.how = "the writes stop";
      }
      break;
    }
    double end;
    if (repeats && readOut) {
      // The writes repeat after several rounds when the rounds differ by a frame:
      // then the period is a whole number of times what it took to read the song.
      // If it is not, the song plays again something that it has read before, and
      // is over when the writes repeat.
      const double once = shape.lastFresh - shape.repeatsFrom;
      const double rounds = once > 1 ? span / once : 0;
      const bool multiple = rounds >= 1.9 && std::fabs(rounds - std::round(rounds)) <= 0.03 * rounds;
      end = multiple ? shape.lastFresh : shape.repeatsAt;
      shape.how = multiple ? "nothing new is read" : "the writes repeat";
    } else if (repeats) {
      end = shape.repeatsAt;
      shape.how = "the writes repeat";
    } else {
      end = shape.lastFresh;
      shape.how = "nothing new is read";
    }
    if (levelOf(end, std::min(ran, 2 * end + 1)) < kQuiet) {
      endsAfter(lastLoud(static_cast<size_t>(end * rate / kWindow) + 1), "silence");
    } else {
      shape.kind = Shape::Loops;
      shape.end = std::max(end, 0.5);
    }
    break;
  }
  return shape;
}

std::string biosFile() {
  if (const char* e = std::getenv("BBMP_COLECO_BIOS")) if (*e) return e;
  std::string base;
  if (const char* x = std::getenv("XDG_DATA_HOME")) base = x;
  if (base.empty()) {
    const char* home = std::getenv("HOME");
    base = std::string(home ? home : ".") + "/.local/share";
  }
  return base + "/modland-player/coleco.rom";
}

// For looking into a file: what every subsong does, one line each.
int scan(int argc, char** argv) {
  std::vector<uint8_t> data;
  if (!readFile(argv[2], data) || data.size() < 0x30) return 1;
  const int first = argc > 4 ? std::atoi(argv[3]) : 0;
  const int last = argc > 4 ? std::atoi(argv[4]) : 255;
  const int seconds = argc > 5 ? std::atoi(argv[5]) : 0;
  nez_hook_write = onWrite;
  nez_hook_frame = onFrame;
  std::string bios = biosFile();
  std::printf("song\tkind\tend\thow\tran\tcalls\twrites\trepeats-from\trepeats-at\tperiod\tlast-fresh\tfresh\tlevel\tbegins\n");
  const int limit = data[0] == 'G' ? kLimitGbr : kLimitSgc;
  for (int song = first; song <= last; song++) {
    Shape s = findShape(data, song, bios, limit, seconds);
    std::printf("%d\t%s\t%.3f\t%s\t%.0f\t%zu\t%llu\t%.3f\t%.3f\t%zu\t%.3f\t%u\t%.1f\t%016llx\n", song,
                kindName(s.kind), s.end, s.how, s.scanned, s.calls,
                static_cast<unsigned long long>(s.writes), s.repeatsFrom, s.repeatsAt, s.period,
                s.lastFresh, s.fresh, s.level, static_cast<unsigned long long>(s.begins));
    std::fflush(stdout);
  }
  return 0;
}

// For comparing with other players: a subsong by its number, for so many seconds,
// as raw sound without a header.
int render(int argc, char** argv) {
  std::vector<uint8_t> data;
  if (argc < 6 || !readFile(argv[2], data) || data.size() < 0x30) return 1;
  const int rate = std::atoi(argv[4]);
  Player p;
  if (!p.open(data, std::atoi(argv[3]), rate, biosFile())) return 1;
  std::vector<int16_t> buf(static_cast<size_t>(rate) * 2);
  for (int s = std::atoi(argv[5]); s > 0; s--) {
    p.render(buf.data(), rate);
    std::fwrite(buf.data(), 4, rate, stdout);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "scan") return scan(argc, argv);
  if (argc >= 3 && std::string(argv[1]) == "render") return render(argc, argv);
  if (argc < 5) {
    std::fprintf(stderr, "usage: %s <sgc|gbr> <file> <track> <samplerate> [start-ms]\n"
                 "       %s scan <file> [first-song last-song [seconds]]\n"
                 "       %s render <file> <song> <samplerate> <seconds>\n",
                 argv[0], argv[0], argv[0]);
    return 2;
  }
  const std::string engine = argv[1], path = argv[2];
  const int track = std::atoi(argv[3]);
  const int rate = std::atoi(argv[4]);
  const long startMs = argc > 5 ? std::atol(argv[5]) : 0;
  if (engine != "sgc" && engine != "gbr") return fail("bbmp-nez plays sgc and gbr, not " + engine);
  if (rate < 8000 || rate > 192000) return fail("unsupported sample rate");

  std::vector<uint8_t> data;
  if (!readFile(path, data)) return fail("can't read the file");

  std::string type, title, author, comment, system, bios;
  std::vector<std::pair<std::string, std::string>> facts;
  std::vector<Entry> lists;
  int tracks = 0, song = 0;
  bool needsBiosCheck = false;

  if (engine == "sgc") {
    if (data.size() < 0xA0 + 1 || std::memcmp(data.data(), "SGC\x1a", 4) != 0)
      return fail("not an SGC file");
    static const char* const systems[] = {"Sega Master System", "Sega Game Gear", "ColecoVision"};
    const unsigned sys = data[0x28];
    if (sys > 2) return fail("SGC for an unknown system (" + std::to_string(sys) + ")");
    const int songs = data[0x25];
    if (songs < 1) return fail("the SGC file has no songs");
    if (sys == 2) {
      // Some ColecoVision songs call the machine's BIOS ROM, which can't be given
      // out with this program. Without it, a song is run first to see whether it
      // does (see below).
      const unsigned load = data[8] | data[9] << 8;
      if (load < 0x8000) return fail("ColecoVision song that loads below 8000h");
      bios = biosFile();
      std::vector<uint8_t> rom;
      if (!readFile(bios, rom, 1u << 20)) {
        needsBiosCheck = true;
      } else if (rom.size() != kBiosSize) {
        return fail("the ColecoVision BIOS ROM " + bios + " is " + std::to_string(rom.size()) +
                    " bytes, not 8192");
      }
    } else if ((data[8] | data[9] << 8) < 0x400) {
      return fail("SGC file that loads below 0400h");
    }
    type = "SGC";
    system = systems[sys];
    title = field(data, 0x40, 32);
    author = field(data, 0x60, 32);
    comment = field(data, 0x80, 32);
    facts.push_back({"System", system + (data[0x05] & 1 ? " (PAL, 50 Hz)" : " (NTSC, 60 Hz)")});
    if (!title.empty()) facts.push_back({"Game", title});
    lists = trackLists(path);
    // a list that names a subsong the file doesn't have is not about this file
    lists.erase(std::remove_if(lists.begin(), lists.end(),
                               [&](const Entry& e) { return e.song >= 256; }), lists.end());
    if (!lists.empty()) {
      tracks = static_cast<int>(lists.size());
      if (track < 0 || track >= tracks) return fail("no such track");
      song = lists[track].song;
      if (!lists[track].title.empty()) title = lists[track].title;
      if (!lists[track].author.empty()) author = lists[track].author;
    } else {
      tracks = songs;
      if (track < 0 || track >= tracks) return fail("no such track");
      song = track;
    }
  } else {
    if (data.size() < 0x20 + 0x150 || std::memcmp(data.data(), "GBRF", 4) != 0)
      return fail("not a GBR file");
    if (data[4] == 0) return fail("the GBR file has no ROM banks");
    type = "GBR";
    system = "Game Boy";
    // the cartridge's name, if the rip kept it
    title = field(data, 0x20 + 0x134, 15);
    size_t letters = 0;
    for (unsigned char c : title) {
      if (c < 0x20 || c > 0x7e) { title.clear(); break; }
      if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) letters++;
    }
    if (letters < 3 || letters * 4 < title.size() * 3) title.clear();
    facts.push_back({"System", system + std::string(data[0x0f] & 0x80 && (data[0x0f] & 0xf0) != 0xf0
                                                     ? " Color" : "")});
    const std::vector<int> songs = gbrTracks(data);
    tracks = static_cast<int>(songs.size());
    if (track < 0 || track >= tracks) return fail("no such track");
    song = songs[track];
    facts.push_back({"Subsong", std::to_string(song) + " of the file's 256"});
  }

  nez_hook_write = onWrite;
  nez_hook_frame = onFrame;

  int64_t playFrames = 0, fadeFrames = 0;
  const bool listed = !lists.empty() && lists[track].lengthMs > 0;
  Shape shape;
  auto frames = [&](double sec) { return static_cast<int64_t>(sec * rate + 0.5); };
  if (needsBiosCheck) {
    nez_hook_bios = 0;
    shape = findShape(data, song, bios, kLimitSgc);
    if (nez_hook_bios)
      return fail("this ColecoVision song calls the ColecoVision BIOS ROM: put the ROM (8192 bytes) at " +
                  bios + " to play it");
  }
  if (listed) {
    // The list's length of a song that loops is twice through the loop in most
    // lists, and a fade. Where the song's own loop is found and the list's length
    // is the intro and twice that loop, the song is played once through.
    const double length = lists[track].lengthMs / 1000.0;
    const double fade = (lists[track].fadeMs >= 0 ? lists[track].fadeMs : kFadeMs) / 1000.0;
    playFrames = frames(length);
    fadeFrames = frames(fade);
    std::string how = "as the track list says";
    if (fade >= 5.0) {
      if (!needsBiosCheck) shape = findShape(data, song, bios, kLimitSgc);
      const double loop = length - shape.end, intro = shape.end - loop;
      if (shape.kind == Shape::Loops && loop > 0 && intro >= -0.06 * loop - 1.5 &&
          intro <= 0.6 * shape.end) {
        playFrames = frames(shape.end);
        fadeFrames = frames(kFadeMs / 1000.0);
        how = "once through, to where the song loops (the track list has it twice)";
      }
    }
    facts.push_back({"Length", how});
  } else {
    if (!needsBiosCheck) shape = findShape(data, song, bios, engine == "gbr" ? kLimitGbr : kLimitSgc);
    switch (shape.kind) {
      case Shape::Loops:
        playFrames = frames(shape.end);
        fadeFrames = frames(kFadeMs / 1000.0);
        break;
      case Shape::Holds:
        fadeFrames = frames(1.0);
        playFrames = frames(shape.end) - fadeFrames;
        break;
      case Shape::Ends:
      case Shape::Silent:
        playFrames = frames(shape.end);
        break;
      case Shape::Unknown:
        playFrames = frames(kDefaultMs / 1000.0);
        fadeFrames = frames(kFadeMs / 1000.0);
        break;
    }
    std::string length = shape.kind == Shape::Loops ? "once through, to where the song loops"
                       : shape.kind == Shape::Unknown ? "no end or loop found"
                       : shape.kind == Shape::Holds ? "ends on a sound that stays"
                       : shape.kind == Shape::Silent ? "silent" : "to the song's end";
    facts.push_back({"Length", length});
  }
  if (debug())
    std::fprintf(stderr, "song %d: listed %d %s (%s) play %.2fs fade %.2fs; ran %.0fs, calls %zu, "
                 "writes repeat from %.2fs at %.2fs, last new byte read at %.2fs\n", song, listed,
                 kindName(shape.kind), shape.how, static_cast<double>(playFrames) / rate,
                 static_cast<double>(fadeFrames) / rate, shape.scanned, shape.calls,
                 shape.repeatsFrom, shape.repeatsAt, shape.lastFresh);

  Player p;
  if (!p.open(data, song, rate, bios)) return fail("libnezplug can't load this file");

  const int64_t total = playFrames + fadeFrames;
  std::string out = "{\"tracks\":" + std::to_string(tracks) + ",\"track\":" + std::to_string(track);
  out += ",\"title\":" + json(title) + ",\"author\":" + json(author) + ",\"type\":" + json(type);
  out += ",\"duration_ms\":" + std::to_string(total * 1000 / rate);
  out += ",\"comment\":" + json(comment) + ",\"names\":[],\"facts\":{";
  for (size_t i = 0; i < facts.size(); i++)
    out += (i ? "," : "") + json(facts[i].first) + ":" + json(facts[i].second);
  out += "}}\n";
  std::fputs(out.c_str(), stdout);
  std::fflush(stdout);

  const int chunk = 2048;
  std::vector<int16_t> buf(chunk * 2);
  int64_t pos = 0;
  const int64_t skip = std::min<int64_t>(total, std::max<long>(0, startMs) * static_cast<int64_t>(rate) / 1000);
  while (pos < total) {
    int n = static_cast<int>(std::min<int64_t>(chunk, total - pos));
    if (pos < skip) n = static_cast<int>(std::min<int64_t>(n, skip - pos));
    p.render(buf.data(), n);
    if (pos + n > playFrames && fadeFrames > 0) {
      for (int i = 0; i < n; i++) {
        int64_t into = pos + i - playFrames;
        if (into <= 0) continue;
        double g = 1.0 - static_cast<double>(into) / fadeFrames;
        if (g < 0) g = 0;
        g *= g;
        buf[i * 2] = static_cast<int16_t>(buf[i * 2] * g);
        buf[i * 2 + 1] = static_cast<int16_t>(buf[i * 2 + 1] * g);
      }
    }
    if (pos >= skip && std::fwrite(buf.data(), 4, n, stdout) != static_cast<size_t>(n)) break;
    pos += n;
  }
  std::fflush(stdout);
  return 0;
}
