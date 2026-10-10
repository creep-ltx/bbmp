// SPDX-License-Identifier: GPL-2.0-or-later
// bbmp-console: what an engine has to offer to the main program.
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
  // <error> when the file isn't of the engine's format or can't be played.
  virtual bool open(const std::vector<uint8_t>& file, int track, int rate, SongInfo& info,
                    std::string& error) = 0;
  // Writes up to <frames> stereo frames. Fewer than asked for means the song has ended.
  virtual size_t render(int16_t* out, size_t frames) = 0;
};

Engine* make_wsr_engine();
Engine* make_spu_engine();

// Text of unknown encoding to UTF-8: kept when it is UTF-8 already, otherwise read
// as Shift-JIS (both formats are Japanese), and as Latin-1 when that fails too.
std::string to_utf8(const std::string& text);

// True when BBMP_CONSOLE_DEBUG is set: engines may then explain themselves on stderr.
bool debug_on();
