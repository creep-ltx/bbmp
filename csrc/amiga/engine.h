// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-amiga: what an engine has to offer to the main program.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SongInfo {
  int tracks = 1;
  std::string title, author, type, comment;
  long duration_ms = 0;
  std::vector<std::string> names;
};

class Engine {
 public:
  virtual ~Engine() {}
  // Reads the file and gets ready to play <track> at <rate>. False and a reason in
  // <error> when the file isn't of the engine's format or can't be played. <path> is
  // where the file is, for engines whose songs have more files than one.
  virtual bool open(const std::string& path, const std::vector<uint8_t>& file, int track,
                    int rate, SongInfo& info, std::string& error) = 0;
  // Writes up to <frames> stereo frames. Fewer than asked for means the song has ended.
  virtual size_t render(int16_t* out, size_t frames) = 0;
};

// Every engine is a source file of its own that signs up under its name:
//   static Engine* make() { return new MyEngine; }
//   REGISTER_ENGINE("maxtrax", make)
// so that the main program need not know which engines were built in.
struct EngineEntry {
  const char* name;
  Engine* (*make)();
  EngineEntry* next;
  EngineEntry(const char* n, Engine* (*m)());
};
#define REGISTER_ENGINE(name, make) static EngineEntry engine_entry_(name, make);

// A new engine of the name, or nullptr when none was built in (for engines that hand
// some files on to another).
Engine* make_engine(const std::string& name);

// Text of unknown encoding to UTF-8: kept when it is UTF-8 already, otherwise read
// as Latin-1 (the Amiga's character set).
std::string to_utf8(const std::string& text);

// True when BBMP_AMIGA_DEBUG is set: engines may then explain themselves on stderr.
bool debug_on();
