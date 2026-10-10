// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-retro: what an engine has to offer to the main program.
//
// One engine per format, each in its own file. An engine registers itself with
// REGISTER_ENGINE("name", Class), so the main program needs no list of them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace retro {

struct SongInfo {
  int tracks = 1;
  std::string title, author, type, comment;
  long long duration_ms = 0;
  std::vector<std::string> names;   // instrument or sample names
};

class Engine {
 public:
  virtual ~Engine() {}
  // Reads the file and gets ready to play <track> at <rate>. False and a reason in
  // <error> when the file isn't of the engine's format or can't be played right.
  // <path> is where the file came from, for engines that need companion files.
  virtual bool open(const std::vector<uint8_t>& file, const std::string& path, int track,
                    int rate, SongInfo& info, std::string& error) = 0;
  // Writes up to <frames> stereo frames (interleaved s16). Fewer than asked for
  // means the song has ended. The engine itself decides where the song ends.
  virtual size_t render(int16_t* out, size_t frames) = 0;
};

typedef Engine* (*EngineMaker)();
void register_engine(const char* name, EngineMaker maker);
struct Registrar {
  Registrar(const char* name, EngineMaker maker) { register_engine(name, maker); }
};
#define REGISTER_ENGINE(name, Class) \
  static retro::Engine* make_##Class() { return new Class; } \
  static retro::Registrar registrar_##Class(name, make_##Class);

// Text helpers: everything leaves as UTF-8.
std::string from_latin1(const std::string& s);
std::string from_cp437(const std::string& s);
std::string from_sjis(const std::string& s);      // falls back to Latin-1
std::string from_petscii(const std::string& s);
// Kept when it is valid UTF-8 already, otherwise read as Shift-JIS, then Latin-1.
std::string to_utf8(const std::string& s);
// A fixed-size text field: up to the first NUL, without trailing blanks.
std::string field(const void* p, size_t size);

std::vector<uint8_t> read_file(const std::string& path);
std::string dir_of(const std::string& path);      // with the trailing slash
// A file next to <path>, its name compared without regard to case. Empty if none.
std::string find_beside(const std::string& path, const std::string& name);

// True when BBMP_RETRO_DEBUG is set: engines may then explain themselves on stderr.
bool debug_on();

// Little helpers for reading binary data.
inline unsigned le16(const uint8_t* p) { return p[0] | p[1] << 8; }
inline unsigned le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
inline unsigned be16(const uint8_t* p) { return p[0] << 8 | p[1]; }
inline unsigned be32(const uint8_t* p) { return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

}  // namespace retro
