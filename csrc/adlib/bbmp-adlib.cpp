// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-adlib: play one song of an Ad Lib format that AdPlug doesn't play, as raw PCM
// on stdout.
//
//   bbmp-adlib <engine> <file> <track> <samplerate> [start-ms]
//
// engine: pixelpainters  Pixel Painters' music (.fmf of Electranoid and Laser Light,
//                        .ftf of Xatax), played the way the games' own routines play it
//                        (ppf_cr.cpp)
//         d01            EdLib D01 (.d01, with Sound Blaster samples in the .s01 beside it)
//                        (d01_cr.cpp)
//         sig            Sound Images Generation 2 (.sig, the DOS Dizzy games) (sig_cr.cpp)
//         lem            Lemmings' Ad Lib driver by Sound Images (.lem, Xmas Lemmings)
//                        (lem_cr.cpp)
//         fmtracker      Sagitta Software's FM Tracker (.fmt), as FM Tracker Player 1.26 plays it
//                        (fmt_cr.cpp)
//         samurai        First Samurai's music (.mus: the game's sound module with its music;
//                        4 tunes), played from the data only (mus_cr.cpp)
//         shadowlands    Shadowlands' music (.cal, TUNE.CAL unpacked) (cal_cr.cpp)
//         vibrants       Vibrants' music (.vib), as JO's player in the Copper demo plays it
//                        (vib_cr.cpp)
//
// All were written for this player in a clean room, from a description of each format
// only; each has a test mode (BBMP_ADLIB_LOG, see common.h).
//
// Same protocol as the other helpers: one JSON line describing the track, then
// signed 16-bit little-endian stereo until the track ends.

#include <cstdlib>
#include <cstring>
#include <iconv.h>

#include <adplug/nemuopl.h>

#include "common.h"

namespace adlib {

namespace {

std::string json(const std::string& s) {
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

std::string fromCp437(const std::string& s) {
  iconv_t cd = iconv_open("UTF-8", "CP437");
  if (cd == (iconv_t)-1) return s;
  std::string in(s), out(in.size() * 4 + 4, '\0');
  char* ip = in.data();
  char* op = out.data();
  size_t il = in.size(), ol = out.size();
  const bool ok = iconv(cd, &ip, &il, &op, &ol) != (size_t)-1;
  iconv_close(cd);
  if (!ok) return std::string();
  out.resize(out.size() - ol);
  return out;
}

std::string field(const void* p, size_t size) {
  const char* c = static_cast<const char*>(p);
  size_t n = 0;
  while (n < size && c[n]) n++;
  while (n > 0 && (c[n - 1] == ' ' || c[n - 1] == '\t')) n--;
  return std::string(c, n);
}

std::string baseName(const char* path) {
  std::string s(path);
  const size_t slash = s.find_last_of('/');
  if (slash != std::string::npos) s = s.substr(slash + 1);
  const size_t dot = s.find_last_of('.');
  if (dot != std::string::npos && dot > 0) s = s.substr(0, dot);
  return s;
}

std::string headerLine(const Info& i) {
  std::string facts, names;
  for (const auto& f : i.facts)
    if (!f.second.empty()) facts += (facts.empty() ? "" : ",") + json(f.first) + ":" + json(f.second);
  for (const auto& n : i.names) names += (names.empty() ? "" : ",") + json(n);
  char num[64];
  std::snprintf(num, sizeof(num), "{\"tracks\":%d,\"track\":%d,", i.tracks, i.track);
  std::string line = num;
  line += "\"title\":" + json(i.title) + ",\"author\":" + json(i.author) + ",\"type\":" + json(i.type);
  std::snprintf(num, sizeof(num), ",\"duration_ms\":%lld,", i.durationMs);
  line += num;
  line += "\"facts\":{" + facts + "},\"comment\":" + json(i.comment) + ",\"names\":[" + names + "]}";
  return line;
}

void header(const Info& i) {
  std::printf("%s\n", headerLine(i).c_str());
  std::fflush(stdout);
}

OplLog::OplLog(const char* engine, const char* path, int track, const Info& info, long long dryTicks,
               double hz, int rate, long long maxMs) {
  const char* to = std::getenv("BBMP_ADLIB_LOG");
  if (!to || !*to) return;
  f_ = std::fopen(to, "w");
  if (!f_) return;
  std::string name(path);
  const size_t slash = name.find_last_of('/');
  if (slash != std::string::npos) name = name.substr(slash + 1);
  perTick_ = rate / hz;
  most_ = static_cast<long long>(maxMs / 1000.0 * hz);
  rate_ = rate;
  std::fprintf(f_, "# %s opl log 1: song \"%s\" track %d\nH %s\n# dry-run ticks %lld\nT 0\n", engine, name.c_str(),
               track, headerLine(info).c_str(), dryTicks);
}

OplLog::~OplLog() {
  if (f_) std::fclose(f_);
}

void OplLog::write(int reg, int val) {
  if (f_) std::fprintf(f_, "%02x %02x\n", reg & 0xff, val & 0xff);
}

void OplLog::line(const std::string& text) {
  if (f_) std::fprintf(f_, "%s\n", text.c_str());
}

void OplLog::tickStart() {
  if (f_) std::fprintf(f_, "T %lld\n", ++tick_);
}

void OplLog::tickEnd(bool goesOn) {
  if (!f_) return;
  pending_ += perTick_;
  const long long k = static_cast<long long>(pending_);
  pending_ -= k;
  std::fprintf(f_, "S %lld\n", k);
  if (!goesOn || tick_ >= most_) {
    std::fprintf(f_, "END\nTAIL %lld\n", static_cast<long long>(rate_) * 1000 / 1000);
    std::fclose(f_);
    f_ = nullptr;
  }
}

int fail(const std::string& why) {
  std::printf("{\"tracks\":0,\"error\":%s}\n", json(why).c_str());
  std::fflush(stdout);
  return 1;
}

bool emit(const int16_t* stereo, size_t frames) {
  return std::fwrite(stereo, 4, frames, stdout) == frames;
}

std::vector<uint8_t> readFile(const char* path) {
  std::vector<uint8_t> data;
  if (FILE* f = std::fopen(path, "rb")) {
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
  }
  return data;
}

Chip::Chip(int rate) : emu_(new CNemuopl(rate)) {
  currType = TYPE_OPL3;
  if (const char* t = std::getenv("BBMP_ADLIB_TRACE")) trace_ = std::fopen(t, "w");
}

Chip::~Chip() {
  if (trace_) std::fclose(trace_);
}

void Chip::write(int reg, int val) {
  const int r = (currChip << 8 | reg) & 0x1ff;
  regs_[r] = static_cast<uint8_t>(val);
  emu_->setchip(currChip);
  emu_->write(reg, val);
}

void Chip::init() {
  emu_->init();
  currChip = 0;
  std::memset(regs_, 0, sizeof(regs_));
}

void Chip::update(short* buf, int samples) {
  emu_->update(buf, samples);
  if (extra) extra(buf, samples);
}

void Chip::traceTick(long long tick) {
  if (!trace_) return;
  std::fprintf(trace_, "%lld ", tick);
  for (int r = 0; r < 256; r++) std::fprintf(trace_, "%02x", regs_[r]);
  std::fputc('\n', trace_);
}

void Chip::traceText(const std::string& what) {
  if (trace_) std::fprintf(trace_, "# %s\n", what.c_str());
}

int play(Song& song, Chip& opl, const Info& info, int rate, long long startMs, long long tailMs,
         long long maxMs) {
  const double perTick = rate / song.hz();
  double pending = 0;
  bool playing = true;
  long long ticks = 0, tail = tailMs * rate / 1000;
  const long long most = static_cast<long long>(maxMs / 1000.0 * song.hz());
  // returns the frames written, fewer than asked for at the end
  auto render = [&](int16_t* out, int frames) {
    int done = 0;
    while (done < frames) {
      if (pending < 1) {
        if (playing) {
          playing = song.tick();
          opl.traceTick(++ticks);
          if (ticks >= most) playing = false;
          pending += perTick;
        } else if (tail > 0) {
          pending += tail;
          tail = 0;
        } else {
          break;
        }
      }
      int n = static_cast<int>(pending);
      if (n > frames - done) n = frames - done;
      opl.update(out + 2 * done, n);
      pending -= n;
      done += n;
    }
    return done;
  };
  std::vector<int16_t> stereo(2 * 4096);
  long long toSkip = startMs * rate / 1000;
  while (toSkip > 0) {
    const int n = toSkip > 4096 ? 4096 : static_cast<int>(toSkip);
    if (render(stereo.data(), n) < n) break;
    toSkip -= n;
  }
  header(info);
  for (;;) {
    const int n = render(stereo.data(), 4096);
    if (n > 0 && !emit(stereo.data(), n)) break;
    if (n < 4096) break;
  }
  return 0;
}

}  // namespace adlib

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: %s <pixelpainters|d01|sig|lem|fmtracker|samurai|shadowlands|vibrants> <file> <track> <samplerate> [start-ms]\n", argv[0]);
    return 2;
  }
  const std::string engine = argv[1];
  const char* path = argv[2];
  const int track = std::atoi(argv[3]);
  int rate = std::atoi(argv[4]);
  if (rate < 8000 || rate > 192000) rate = 48000;
  long long startMs = argc > 5 ? std::atoll(argv[5]) : 0;
  if (startMs < 0) startMs = 0;
  if (FILE* f = std::fopen(path, "rb")) std::fclose(f);
  else return adlib::fail("can't read the file");
  if (engine == "pixelpainters") return adlib::playPpfCr(path, track, rate, startMs);
  if (engine == "d01") return adlib::playD01Cr(path, track, rate, startMs);
  if (engine == "sig") return adlib::playSigCr(path, track, rate, startMs);
  if (engine == "lem") return adlib::playLemCr(path, track, rate, startMs);
  if (engine == "fmtracker") return adlib::playFmtCr(path, track, rate, startMs);
  if (engine == "samurai") return adlib::playMusCr(path, track, rate, startMs);
  if (engine == "shadowlands") return adlib::playCalCr(path, track, rate, startMs);
  if (engine == "vibrants") return adlib::playVibCr(path, track, rate, startMs);
  return adlib::fail("unknown engine " + engine);
}
