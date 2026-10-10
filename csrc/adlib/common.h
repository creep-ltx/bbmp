// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-adlib: what the engines share: the JSON header line, the PCM output, and a
// driver that runs a song's timer routine against an emulated OPL chip.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <adplug/opl.h>

namespace adlib {

struct Info {
  int tracks = 1;
  int track = 0;
  std::string title, author, type;
  long long durationMs = 0;
  std::vector<std::pair<std::string, std::string>> facts;   // what else the file says
  std::string comment;
  std::vector<std::string> names;                           // instrument names
};

// Text from old PC programs is CP437; everything leaves as UTF-8.
std::string fromCp437(const std::string& s);
// A fixed-size text field: up to the first NUL, without trailing blanks.
std::string field(const void* p, size_t size);
// The file's name without folder and extension, for songs that carry no title.
std::string baseName(const char* path);

void header(const Info& info);
// The header line as header() prints it, without the line end.
std::string headerLine(const Info& info);
int fail(const std::string& why);
bool emit(const int16_t* stereo, size_t frames);
std::vector<uint8_t> readFile(const char* path);

// A song as its player ran it: a routine called by a timer. tick() is one call of it;
// it writes to the chip and says whether the song goes on.
class Song {
 public:
  virtual ~Song() = default;
  virtual bool tick() = 0;
  virtual double hz() const = 0;        // how often the timer calls it
};

// The emulated chip (Nuked OPL3, as AdPlug has it), with what else a song adds to its
// sound (`extra`, samples). With BBMP_ADLIB_TRACE=<file>, the registers are written to
// the file after every tick, one line each, for comparing with the original players.
class Chip : public Copl {
 public:
  explicit Chip(int rate);
  ~Chip() override;
  void write(int reg, int val) override;
  void init() override;
  void update(short* buf, int samples) override;
  void traceTick(long long tick);
  void traceText(const std::string& what);   // an event of the song, on a line of its own
  std::function<void(short*, int)> extra;

 private:
  std::unique_ptr<Copl> emu_;
  uint8_t regs_[512] = {};
  FILE* trace_ = nullptr;
};

// The test mode of the engines that have one: with BBMP_ADLIB_LOG=<file>, every chip
// write is listed there as it is made, tick by tick, with the samples each tick lasts:
//   # <engine> opl log 1: song "<file name>" track <t>
//   H <the header line>
//   # dry-run ticks <N>
//   T 0             then the writes before the first tick ("ff 00": register, value)
//   T 1             then tick 1's writes, then "S <samples>", and so on
//   END             after the last tick, then "TAIL <frames>" (the second after it)
// The sample counting is the same as play()'s, so the log says exactly what is heard.
class OplLog {
 public:
  // Opens the log if BBMP_ADLIB_LOG is set; otherwise does nothing at all.
  OplLog(const char* engine, const char* path, int track, const Info& info, long long dryTicks, double hz,
         int rate, long long maxMs = 20 * 60 * 1000);
  ~OplLog();
  bool on() const { return f_ != nullptr; }
  void write(int reg, int val);
  void line(const std::string& text);   // another event of the song, among the writes
  void tickStart();
  void tickEnd(bool goesOn);   // after the tick's writes; ends the log after the last tick

 private:
  FILE* f_ = nullptr;
  double perTick_ = 0, pending_ = 0;
  long long tick_ = 0, most_ = 0;
  int rate_ = 0;
};

// Plays `song` on `opl` to stdout: skips startMs, writes the header, then the sound
// until the song ends (and `tailMs` after that, for the last notes to fade), or for at
// most maxMs.
int play(Song& song, Chip& opl, const Info& info, int rate, long long startMs, long long tailMs = 1000,
         long long maxMs = 20 * 60 * 1000);

int playPpfCr(const char* path, int track, int rate, long long startMs);
int playD01Cr(const char* path, int track, int rate, long long startMs);
int playSigCr(const char* path, int track, int rate, long long startMs);
int playLemCr(const char* path, int track, int rate, long long startMs);
int playFmtCr(const char* path, int track, int rate, long long startMs);
int playMusCr(const char* path, int track, int rate, long long startMs);
int playCalCr(const char* path, int track, int rate, long long startMs);
int playVibCr(const char* path, int track, int rate, long long startMs);

}  // namespace adlib
