// SPDX-License-Identifier: GPL-3.0-or-later
//
// pmdc: the PMD driver (pmdc_driver.cpp) playing on ymfm's YM2608.
//
// Time runs as in the player this one replaces: the driver's two timers decide when each
// event comes, and each event is followed by floor(µs × rate / 10^6) output frames. The
// chip renders at its own rate (7.9872 MHz / 144 = 55466.7 Hz) and is resampled.
//
// A song plays to where every part has looped once, then fades out over 4 seconds. The
// old player played a song shorter than one second for 3 minutes (its length in whole
// seconds was 0); this one plays it for its own length.

#include "pmdc_engine.h"

#include <samplerate.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>

#include "pmdc_driver.h"
#include "ymfm_opn.h"

namespace pmdc {

namespace {

constexpr uint32_t CLOCK = 7987200;
constexpr int FADE_SECONDS = 4;

class Chip : public ymfm::ymfm_interface {
 public:
  explicit Chip(const std::vector<uint8_t>& rom) : rom_(rom), opna_(*this) {
    opna_.set_fidelity(ymfm::OPN_FIDELITY_MIN);
    opna_.reset();
  }
  void write(int reg, int value) {
    // The ADPCM unit's memory is empty (no song's PCM file is on Modland): ymfm would
    // play its zeros as a rising offset, the real chip near silence. Leave it alone.
    if (reg >= 0x100 && reg <= 0x10F) return;
    // Without the rhythm ROM the drums can't sound; zeros would click
    if (reg == 0x10 && rom_.empty() && !(value & 0x80)) return;
    const bool hi = reg & 0x100;
    opna_.write(hi ? 2 : 0, static_cast<uint8_t>(reg & 0xFF));
    opna_.write(hi ? 3 : 1, static_cast<uint8_t>(value));
  }
  // One sample at the chip's FM rate (three of ymfm's at its lowest fidelity)
  void sample(int32_t& l, int32_t& r, int32_t& ssg) {
    ymfm::ym2608::output_data out[3];
    opna_.generate(out, 3);
    l = out[0].data[0];   // the FM part repeats over the three
    r = out[0].data[1];
    ssg = (out[0].data[2] + out[1].data[2] + out[2].data[2]) / 3;
  }
  uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override {
    if (type == ymfm::ACCESS_ADPCM_A && address < rom_.size()) return rom_[address];
    return 0;
  }

 private:
  std::vector<uint8_t> rom_;
  ymfm::ym2608 opna_;
};

struct ChipSink : Sink {
  std::function<void(int, int)> write;
  void reg(int r, int v) override { write(r, v); }
  void unit(const std::string&) override {}
};

}  // namespace

struct Player::Impl {
  int rate = 48000;
  Random rng;
  std::unique_ptr<Chip> chip;
  ChipSink sink;
  std::unique_ptr<Driver> drv;
  SRC_STATE* src = nullptr;
  long long frames = 0;      // output frames the timers have made
  long long chipDone = 0;    // chip samples rendered
  long long emitted = 0;     // frames given out
  long long playFrames = 0, totalFrames = 0;
  bool timersStopped = false;
  std::deque<float> fifo;    // resampled stereo, interleaved
  double dcIn = 0, dcOut = 0;   // the SSG's output is one-sided: a DC blocker

  ~Impl() {
    if (src) src_delete(src);
  }

  std::vector<float> in;     // chip samples not yet resampled
  long long writeDelay = 0;  // thousandths of a chip sample owed to register writes
  int keyedOff[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // slots keyed off since the last clock
  int keyState[8] = {0, 0, 0, 0, 0, 0, 0, 0};

  void clock() {
    int32_t l, r, s;
    chip->sample(l, r, s);
    // DC blocker for the SSG (about 5 Hz)
    const double x = s;
    dcOut = x - dcIn + 0.99943 * dcOut;
    dcIn = x;
    const float ssg = static_cast<float>(dcOut) * SSG_GAIN;
    in.push_back((l * FM_GAIN + ssg) / 32768.0f);
    in.push_back((r * FM_GAIN + ssg) / 32768.0f);
    chipDone++;
    for (int& k : keyedOff) k = 0;
  }

  // A register write. As in the player this one replaces, each write lets 0.72 output
  // samples of chip time pass before the next (the driver's writes take time on the real
  // machine too). And ymfm only notices a key-on when it clocks: a key-off directly
  // followed by a key-on of the same slot (PMD does this at every new voice) would not
  // start the note again, so the chip is clocked once in between.
  void write(int reg, int value) {
    writeDelay += 720LL * CLOCK / 144 / rate;   // 0.72 output samples, in chip samples x 1000
    while (writeDelay > 1000) {
      clock();
      writeDelay -= 1000;
    }
    if (reg == 0x28) {
      const int ch = value & 7, slots = (value >> 4) & 0x0F;
      if (slots & keyedOff[ch]) clock();
      keyedOff[ch] |= keyState[ch] & ~slots;
      keyState[ch] = slots;
    }
    chip->write(reg, value);
  }

  void step() {
    long long n;
    if (!timersStopped) {
      drv->event();
      const uint32_t u = drv->nextEvent();
      if (u == 0) {
        timersStopped = true;   // a song that stopped both timers: silence from here
        n = 4096;
      } else {
        drv->advance(u);
        n = static_cast<long long>(u) * rate / 1000000;
      }
    } else {
      n = 4096;
    }
    frames += n;
    // the chip runs to the event's end (unless the writes have already taken it past)
    const long long target = frames * CLOCK / (144LL * rate);
    while (chipDone < target) clock();
    if (in.empty()) return;
    const long count = static_cast<long>(in.size() / 2);
    std::vector<float> out(static_cast<size_t>(2 * (count * rate / 50000 + 64)));
    SRC_DATA d{};
    d.data_in = in.data();
    d.input_frames = count;
    d.data_out = out.data();
    d.output_frames = static_cast<long>(out.size() / 2);
    d.src_ratio = static_cast<double>(rate) * 144.0 / CLOCK;
    while (d.input_frames > 0) {
      if (src_process(src, &d) != 0) break;
      fifo.insert(fifo.end(), out.begin(), out.begin() + 2 * d.output_frames_gen);
      d.data_in += 2 * d.input_frames_used;
      d.input_frames -= d.input_frames_used;
      if (d.input_frames_used == 0 && d.output_frames_gen == 0) break;
    }
    in.clear();
  }

  // levels fitted to the old player's audio over 40 songs (least squares on the loudness
  // of the FM and SSG stems: FM +5.8 dB, SSG -3.5 dB; ymfm's SSG is one-sided, hence the
  // DC blocker)
  static constexpr float FM_GAIN = 1.95f;
  static constexpr float SSG_GAIN = 0.67f;
};

Player::Player() : impl_(new Impl) {}
Player::~Player() = default;

std::string Player::open(const std::vector<uint8_t>& file, int rate, const std::vector<uint8_t>& rom,
                         SongInfo& info) {
  Impl& m = *impl_;
  if (rate < 8000 || rate > 192000) return "unsupported sample rate";
  m.rate = rate;
  Scan sc;
  if (!scan(m.rng, file.data(), file.size(), sc)) {
    Driver probe(m.rng, nullptr);
    return probe.load(file.data(), file.size());
  }
  m.chip.reset(new Chip(rom));
  m.sink.write = [&m](int r, int v) { m.write(r, v); };
  m.drv.reset(new Driver(m.rng, &m.sink));
  m.drv->init();
  const std::string err = m.drv->load(file.data(), file.size());
  if (!err.empty()) return err;
  info.title = memoText(m.drv->memo(1));
  info.composer = memoText(m.drv->memo(2));
  info.facts.emplace_back("Arranger", memoText(m.drv->memo(3)));
  for (int n : {0, -1, -2}) {
    const std::string f = m.drv->memo(n);
    if (!f.empty()) info.facts.emplace_back(n == 0 ? "PCM file" : n == -1 ? "PPS file" : "PPZ file", f);
  }
  for (int n = 4; n < 16; n++) {
    const std::string t = memoText(m.drv->memo(n));
    if (t.empty()) break;
    info.facts.emplace_back("Memo", t);
  }
  m.drv->start();
  // the audio starts after the set-up's writes, at time 0
  m.in.clear();
  m.chipDone = 0;
  m.writeDelay = 0;
  info.lengthMs = sc.lengthMs;
  info.loopMs = sc.loopMs;
  // the length in whole seconds, as before; a song shorter than a second plays for its
  // own length (the old player played it for 3 minutes), one that never says for 3
  long long play = (sc.lengthMs / 1000) * static_cast<long long>(rate);
  if (play == 0) play = sc.lengthMs > 0 ? sc.lengthMs * static_cast<long long>(rate) / 1000 : 180LL * rate;
  m.playFrames = info.playFrames = play;
  m.totalFrames = info.totalFrames = play + FADE_SECONDS * static_cast<long long>(rate);
  if (sc.usesRhythm && rom.empty())
    info.comment = "The YM2608's rhythm sounds are silent: its rhythm ROM (ym2608_adpcm_rom.bin) "
                   "is not in the extras folder.";
  int e = 0;
  m.src = src_new(SRC_SINC_MEDIUM_QUALITY, 2, &e);
  if (!m.src) return "no resampler";
  return "";
}

long Player::render(int16_t* out, long count) {
  Impl& m = *impl_;
  long done = 0;
  while (done < count && m.emitted < m.totalFrames) {
    while (m.fifo.size() < 2) m.step();
    const float l = m.fifo.front();
    m.fifo.pop_front();
    const float r = m.fifo.front();
    m.fifo.pop_front();
    float g = 1.0f;
    if (m.emitted >= m.playFrames)
      g = 1.0f - static_cast<float>(m.emitted - m.playFrames) / (m.totalFrames - m.playFrames);
    auto cv = [g](float x) {
      const float y = std::round(x * g * 32767.0f);
      return static_cast<int16_t>(std::clamp(y, -32768.0f, 32767.0f));
    };
    out[2 * done] = cv(l);
    out[2 * done + 1] = cv(r);
    done++;
    m.emitted++;
  }
  return done;
}

// ---------------------------------------------------------------- the register log

namespace {

struct LogSink : Sink {
  FILE* f = nullptr;
  long long time = 0;
  bool on = true;
  void reg(int r, int v) override {
    if (on) std::fprintf(f, "%lld %03x %02x\n", time, r, v);
  }
  void unit(const std::string& s) override {
    if (on) std::fprintf(f, "%lld %s\n", time, s.c_str());
  }
};

void logRand(void* user, uint32_t v) {
  auto* s = static_cast<LogSink*>(user);
  if (s->on) std::fprintf(s->f, "%lld RAND %u\n", s->time, v);
}

}  // namespace

std::string writeLog(const std::vector<uint8_t>& file, const std::string& logPath) {
  Random rng;
  LogSink sink;
  sink.f = std::fopen(logPath.c_str(), "w");
  if (!sink.f) return "can't write " + logPath;
  std::fprintf(sink.f, "# init\n");
  Driver play(rng, &sink);
  play.init();
  Scan sc;
  const bool ok = scan(rng, file.data(), file.size(), sc);
  const long scanDrawn = rng.drawn;
  std::fprintf(sink.f, "# play\n");
  const std::string err = play.load(file.data(), file.size());
  if (!ok || !err.empty()) {
    std::fclose(sink.f);
    return err.empty() ? "refused" : err;
  }
  play.start();
  const long long lengthS = sc.lengthMs / 1000;
  const long long frames = std::min((lengthS ? lengthS : 180) * 48000 + 192000, 600LL * 48000);
  const long long rendered = (frames + 4095) / 4096 * 4096;
  std::fprintf(sink.f, "# render length_s=%lld loop_s=%ld frames=%lld rand_in_length_scan=%ld\n",
               lengthS, sc.loopMs / 1000, frames, scanDrawn);
  rng.onDraw = logRand;
  rng.user = &sink;
  while (sink.time < rendered) {
    play.event();
    const uint32_t u = play.nextEvent();
    if (u == 0) break;
    play.advance(u);
    sink.time += static_cast<long long>(u) * 48 / 1000;
  }
  std::fprintf(sink.f, "# end frames_rendered=%lld\n", rendered);
  std::fclose(sink.f);
  return "";
}

}  // namespace pmdc
