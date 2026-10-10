// SPDX-License-Identifier: GPL-3.0-or-later
// What every replayer ("engine") of bbmp-ay offers to the main program.
#ifndef BBMP_AY_ENGINE_H
#define BBMP_AY_ENGINE_H

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace ay {

struct Info {
  int tracks = 1;                   // subsongs, at least 1
  long duration_ms = 0;             // 0 when unknown
  std::string title, author, type, comment;
  std::vector<std::string> names;   // instrument or sample names
};

// An engine refuses a file by throwing Refused; what() says why.
struct Refused : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class Engine {
 public:
  virtual ~Engine() = default;
  // Reads the song and gets ready to play `track` at `rate` Hz. Fills `info`.
  // Throws Refused when the file isn't the engine's format or can't be played right.
  virtual void open(const std::string& path, const std::vector<uint8_t>& data, int track,
                    int rate, Info& info) = 0;
  // Writes up to `frames` stereo frames (s16, interleaved) and returns how many;
  // fewer than asked for means that the song has ended.
  virtual int render(int16_t* out, int frames) = 0;
};

// One function per engine, each in its own <name>_engine.cpp.
std::unique_ptr<Engine> make_beepola();   // Beepola .bbsong (ZX Spectrum beeper)
std::unique_ptr<Engine> make_fls();       // Flash Tracker .fls
std::unique_ptr<Engine> make_fxm();       // Fuxoft AY Language .fxm and ZXAYAMAD .amad
std::unique_ptr<Engine> make_strc();      // ZXAYSTRC: Sound Tracker compiled by Raxoft

}  // namespace ay

#endif
