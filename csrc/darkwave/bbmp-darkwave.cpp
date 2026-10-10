/* SPDX-License-Identifier: GPL-3.0-or-later */
/*  bbmp-darkwave: play a DarkWave Studio project (.dwp) as raw PCM on stdout.

      bbmp-darkwave <file> <track> <samplerate> [start-ms]

    Same protocol as the other helpers: one JSON line describing the track, then signed
    16-bit little-endian stereo until the song ends.

    The sound is DarkWave Studio 4.1.6's own: its studio (studio.cpp, a port) and the
    machines of its ES-CoreMachines (built from its source with the windows taken out).
    DarkWave Studio is Copyright (C) 2004-2012 Daniel Werner, http://www.experimentalscene.com/

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <pmmintrin.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "studio.h"

namespace {

int g_out = 1;   // the real stdout; what the machines print goes to stderr

bool put(const void* data, size_t size) {
  const char* p = static_cast<const char*>(data);
  while (size) {
    const ssize_t n = write(g_out, p, size);
    if (n <= 0) return false;
    p += n;
    size -= n;
  }
  return true;
}

std::string json(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20 || c == 0x7f) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", c);
      out += buf;
    } else if (c >= 0x80) {
      // the program wrote Windows-1252; the common letters of it as UTF-8
      out += static_cast<char>(0xc0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3f));
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

int fail(const std::string& why) {
  const std::string line = "{\"tracks\":0,\"error\":" + json(why) + "}\n";
  put(line.data(), line.size());
  return 1;
}

std::vector<uint8_t> readFile(const char* path, bool* ok) {
  std::vector<uint8_t> data;
  *ok = false;
  if (FILE* f = std::fopen(path, "rb")) {
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
      data.insert(data.end(), buf, buf + n);
      if (data.size() > (16u << 20)) break;   // no project is this big (they are some 20 kB)
    }
    std::fclose(f);
    *ok = true;
  }
  return data;
}

inline int16_t clip(double x) {
  // ConvertAudioDoubleToInt, as the studio's wave output has it
  if (!(x == x)) return 0;
  return x < 0.0 ? (x <= -1.0 ? -32768 : (int16_t)(x * 32768.0)) : (x >= 1.0 ? 32767 : (int16_t)(x * 32767.0));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <file> <track> <samplerate> [start-ms]\n", argv[0]);
    return 2;
  }
  std::fflush(stdout);
  g_out = dup(STDOUT_FILENO);
  dup2(STDERR_FILENO, STDOUT_FILENO);

  const char* path = argv[1];
  const int track = std::atoi(argv[2]);
  const int rate = std::atoi(argv[3]);
  long long startMs = argc > 4 ? std::atoll(argv[4]) : 0;
  if (startMs < 0) startMs = 0;
  if (rate < 8000 || rate > 192000) return fail("unsupported sample rate");
  if (track != 0) return fail("a DarkWave Studio project has one track");
  bool ok;
  const std::vector<uint8_t> file = readFile(path, &ok);
  if (!ok) return fail("can't read the file");

  const int block = 512;
  darkwave::Studio& studio = *new darkwave::Studio(rate, block);
  // (the studio is left standing when the helper ends: some of the machines' parts are
  // taken down wrongly by their own code, which Windows forgave)
  const std::string why = studio.Load(file);
  if (!why.empty()) _exit(fail(why));
  if (!studio.MissingMachines.empty()) {
    std::string list;
    for (auto& m : studio.MissingMachines) list += (list.empty() ? "" : ", ") + m;
    _exit(fail("the project needs machines this player doesn't have: " + list));
  }

  // A project's sequence is often a bar or two that the studio's Loop button repeats (the
  // program's own sample projects are 2 to 6 s long). The loop is not saved in the project;
  // what is shorter than half a minute is played as many times as it takes to fill one.
  const long long once = studio.SongSamples();
  const long long enough = 30LL * rate;
  studio.Passes = once > 0 && once < enough ? (int)((enough + once - 1) / once) : 1;
  if (const char* passes = std::getenv("BBMP_DARKWAVE_PASSES"))   // for tests: this many
    studio.Passes = std::max(1, std::min(1000, std::atoi(passes)));
  const long long length = once * studio.Passes;
  const long long durationMs = length * 1000 / rate;
  char version[32], tempo[64], sequence[96];
  std::snprintf(version, sizeof version, "%d.%d", studio.MajorVersion, studio.MinorVersion);
  std::snprintf(tempo, sizeof tempo, "%g BPM, %g ticks per beat", studio.TheHost.BPM, studio.TheHost.TPB);
  if (studio.Passes > 1)
    std::snprintf(sequence, sizeof sequence, "%.1f s, played %d times (the studio's Loop)", (double)once / rate,
                  studio.Passes);
  else
    std::snprintf(sequence, sizeof sequence, "%.1f s", (double)once / rate);
  std::string names;
  for (auto& n : studio.MachineNames)
    if (n != "Master") names += (names.empty() ? "" : ",") + json(n);
  std::string comment = studio.Comment;
  const std::string header = "{\"tracks\":1,\"track\":0,\"title\":\"\",\"author\":\"\",\"type\":\"DarkWave Studio\","
                             "\"duration_ms\":" + std::to_string(durationMs) +
                             ",\"facts\":{\"Project format\":" + json(version) + ",\"Tempo\":" + json(tempo) +
                             ",\"Sequence\":" + json(sequence) +
                             "},\"comment\":" + json(comment) + ",\"names\":[" + names + "]}\n";
  if (!put(header.data(), header.size())) _exit(1);

  // as the studio's audio thread would: numbers too small to matter are zero
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
  _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);

  std::vector<double> left(block), right(block);
  // The program's machines run from the moment the project is loaded, and Play is pressed
  // later: by then they have taken in their settings (a drum machine's first beat is too
  // loud without this) and their oscillators and LFOs have run on. A second of that here.
  // BBMP_DARKWAVE_IDLE=<blocks of 512> sets it: where the program's oscillators are when
  // the song starts depends on how long it ran before, which is what comparing with its own
  // output needs to match.
  int idle = rate / block;
  if (const char* e = std::getenv("BBMP_DARKWAVE_IDLE")) idle = std::max(0, std::min(1 << 16, std::atoi(e)));
  studio.Idle(idle, left.data(), right.data());
  std::vector<int16_t> pcm(block * 2);
  long long skip = startMs * rate / 1000;
  const double quiet = 8.0 / 32768.0;
  const long long tailMax = 10LL * rate, fadeFrom = 7LL * rate, quietFor = rate / 2;
  long long tail = 0, silent = 0;
  bool ending = false;
  for (;;) {
    if (ending && (tail >= tailMax || silent >= quietFor)) break;
    const bool playing = studio.Work(left.data(), right.data());
    if (!playing) ending = true;   // what sounds after the sequence is the end of the song
    for (int i = 0; i < block; i++) {
      double l = left[i], r = right[i];
      if (ending) {
        const long long at = tail + i;
        if (at >= fadeFrom) {
          const double gain = std::max(0.0, 1.0 - (double)(at - fadeFrom) / (tailMax - fadeFrom));
          l *= gain;
          r *= gain;
        }
        if (std::fabs(l) < quiet && std::fabs(r) < quiet) ++silent; else silent = 0;
      }
      pcm[i * 2] = clip(l);
      pcm[i * 2 + 1] = clip(r);
    }
    if (ending) tail += block;
    if (skip >= block) {
      skip -= block;
      continue;
    }
    const size_t from = (size_t)skip * 2;
    skip = 0;
    if (!put(pcm.data() + from, (pcm.size() - from) * sizeof(int16_t))) break;
  }
  _exit(0);   // without taking the machines down
}
