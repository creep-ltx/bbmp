// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-retro, engine "ned": NerdTracker II songs (*.ned), a tracker for the NES.
//
// A .ned file holds everything the tracker's NES replay routine needs: the song
// (what the tracker writes as TEMP.DAT) and the DPCM samples with their note tables
// (TEMP.IHD, which MAKEDTT.EXE turns into TEMP.DTT and TEMP.DMC). This engine does
// what the tracker's makensf.bat does, in memory: it puts the replay routine, the
// song, the DPCM note table and the samples into an NSF image, and has Game Music
// Emu play that. The replay routine itself is Bananmos' 6502 code ("replay 22"),
// assembled by the build script into share/nt2replay.bin; it is not part of this file.
//
// Where the song ends is found by walking through the song the way the replay
// routine does (orders, rows, the effects Dxx and Fxx), without making sound.

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unistd.h>

#include <gme/gme.h>

#include "self_path.h"
#include "engine.h"

using namespace retro;

namespace {

const int kInfoHeader = 52;        // "NED: ", name, 0x1a, sizes
const int kSampInst = 229;         // one DPCM instrument: note table, names, 8 samples
const int kDataHeader = 16;
const int kInstruments = 16 * 8;   // the replay routine takes 16 instruments for granted
const int kLoad = 0x8000;          // where the replay routine is assembled to
const int kSamples = 0xc080;       // where it expects the samples ($4012 = offset + 2)
const double kTickHz = 50.0;       // the tracker's tick, "BPM = 125"
const int kFadeMs = 3000;          // into the second round of a song that loops
const int kTailMs = 10000;         // the most that is played after a song has halted
const long long kMostBuffered = 1LL << 27;   // frames of a halting song played in advance

struct Replay {
  std::vector<uint8_t> code;
  int setup = 0, play = 0, table = 0, nsfPlay = 0;
};

std::string share_dir() {
  const std::string exe = self_path();
  if (exe.empty()) return "";
  return dir_of(dir_of(exe).substr(0, dir_of(exe).size() - 1)) + "share/";
}

bool load_replay(Replay& r, std::string& error) {
  const std::string dir = share_dir();
  r.code = read_file(dir + "nt2replay.bin");
  const std::vector<uint8_t> lbl = read_file(dir + "nt2replay.lbl");
  if (r.code.empty() || lbl.empty()) {
    error = "the NerdTracker replay routine is missing: " + dir + "nt2replay.bin";
    return false;
  }
  // VICE label file, as ld65 -Ln writes it: "al 008252 .NED_SetupNED"
  const std::string text(lbl.begin(), lbl.end());
  auto find = [&](const char* name) {
    const std::string key = std::string(" .") + name + "\n";
    const size_t at = text.find(key);
    if (at == std::string::npos || at < 6) return 0;
    return static_cast<int>(std::strtol(text.substr(at - 6, 6).c_str(), nullptr, 16));
  };
  r.setup = find("NED_SetupNED");
  r.play = find("NED_PlayNED");
  r.table = find("NED_ToneTableDPCM");
  r.nsfPlay = find("NSF_Play");
  if (!r.setup || !r.play || !r.nsfPlay || r.table != kLoad + static_cast<int>(r.code.size())) {
    error = "the NerdTracker replay routine isn't the one this engine was made for";
    return false;
  }
  return true;
}

// The song as the replay routine walks through it, to find out where it ends.
class Walk {
 public:
  explicit Walk(const std::vector<uint8_t>& song) : d_(song) {}

  // Ticks until the song comes back to a place where it has been (loops) or
  // stops itself with F00 (halts). False when neither happens within <limit> ticks.
  bool run(long limit, long& ticks, bool& halts) {
    speed_ = d_[3];
    restart_ = d_[4];
    length_ = d_[7];
    int at = kDataHeader + kInstruments;
    order_addr_ = at;
    at += 4 * length_;
    for (int c = 0; c < 5; c++) {
      ptrs_[c] = at;
      at += 2 * d_[8 + c];
    }
    tick_ = (speed_ + 1) & 0xff;
    row_ = order_ = 0;
    // for checking this against the replay routine itself: tick, order, row, speed
    const bool trace = std::getenv("BBMP_RETRO_NED_WALK") != nullptr;
    std::set<int> seen;
    for (long t = 0; t < limit; t++) {
      if (speed_ == 0) {
        ticks = t;
        halts = true;
        return true;
      }
      tick_ = (tick_ + 1) & 0xff;
      const int diff = (speed_ - tick_) & 0xff;
      if (diff != 0 && !(diff & 0x80)) continue;   // a tick between two rows
      tick_ = 0;
      if (row_ >= 64) {
        row_ = 0;
        if (++order_ >= length_) order_ = restart_;
      }
      if (!seen.insert(order_ << 8 | row_).second) {
        ticks = t;
        halts = false;
        return true;
      }
      read_row();
      for (int c = 3; c >= 0; c--) {
        if (!eff_[c]) continue;
        if (eff_[c] == 0x0f) {
          speed_ = parm_[c];
        } else if (eff_[c] == 0x0d) {
          row_ = (parm_[c] - 1) & 0xff;
          if (++order_ >= length_) order_ = restart_;
        }
      }
      row_ = (row_ + 1) & 0xff;
      if (trace) std::fprintf(stderr, "walk %ld %d %d %d\n", t, order_, row_, speed_);
    }
    return false;
  }

 private:
  int peek(int at) const { return at >= 0 && at < static_cast<int>(d_.size()) ? d_[at] : 0; }

  void read_row() {
    if (row_ == 0) {
      const int e = order_addr_ + ((order_ * 4) & 0xff);
      const unsigned entry = peek(e) | peek(e + 1) << 8 | peek(e + 2) << 16 |
                             static_cast<unsigned>(peek(e + 3)) << 24;
      for (int c = 4; c >= 0; c--) {
        const int pat = entry >> (5 * c) & 0x1f;
        const int off = peek(ptrs_[c] + 2 * pat) | peek(ptrs_[c] + 2 * pat + 1) << 8;
        if (!off) {
          total_[c] = 0;
          continue;
        }
        addr_[c] = off;
        skip_[c] = peek(off);
        total_[c] = (peek(off + 1) + 1) & 0xff;
        offs_[c] = (((total_[c] + 1) >> 1) + 2) & 0xff;
        odd_[c] = false;
      }
    }
    for (int c = 4; c >= 0; c--) {
      eff_[c] = parm_[c] = 0;
      total_[c] = (total_[c] - 1) & 0xff;
      if (total_[c] & 0x80) continue;
      skip_[c] = (skip_[c] - 1) & 0xff;
      if (!(skip_[c] & 0x80)) continue;
      const int base = addr_[c];
      int bits = peek(base + 2 + (row_ >> 1));
      if (row_ & 1) bits >>= 4;
      int y = offs_[c];
      bool odd = odd_[c];
      auto nibble = [&]() {
        const int v = odd ? peek(base + y) >> 4 : peek(base + y) & 0x0f;
        if (odd) y = (y + 1) & 0xff;
        odd = !odd;
        return v;
      };
      auto byte = [&]() {
        const int v = odd ? (peek(base + y) & 0xf0) | (peek(base + y + 1) & 0x0f) : peek(base + y);
        y = (y + 1) & 0xff;
        return v;
      };
      if (bits & 1) {                 // a note: a nibble for the noise, a byte otherwise
        if (c == 3) nibble();
        else byte();
      }
      if (bits & 2) nibble();         // an instrument
      if (bits & 4) eff_[c] = nibble();
      if (bits & 8) parm_[c] = byte();
      offs_[c] = y;
      odd_[c] = odd;
    }
  }

  const std::vector<uint8_t>& d_;
  int speed_ = 0, restart_ = 0, length_ = 0, tick_ = 0, row_ = 0, order_ = 0;
  int order_addr_ = 0, ptrs_[5] = {};
  int addr_[5] = {}, skip_[5] = {}, total_[5] = {}, offs_[5] = {};
  bool odd_[5] = {};
  int eff_[5] = {}, parm_[5] = {};
};

class NedEngine : public Engine {
 public:
  ~NedEngine() override {
    if (emu_) gme_delete(emu_);
  }

  bool open(const std::vector<uint8_t>& file, const std::string&, int track, int rate,
            SongInfo& info, std::string& error) override {
    if (file.size() < static_cast<size_t>(kInfoHeader + kDataHeader) ||
        std::memcmp(file.data(), "NED: ", 5) != 0) {
      error = "not a NerdTracker II song";
      return false;
    }
    const uint8_t* h = file.data();
    const unsigned headerSize = le32(h + 35);
    const int version = h[39];
    const unsigned dpcmInsts = h[42];
    const unsigned sampleBytes = le32(h + 44);
    if (headerSize != static_cast<unsigned>(kInfoHeader) || version != 0x21) {
      char buf[80];
      std::snprintf(buf, sizeof(buf), "a NerdTracker II song of a version this can't play (%x.%x)",
                    version >> 4, version & 15);
      error = buf;
      return false;
    }
    if (dpcmInsts > 15 || sampleBytes > file.size()) {
      error = "NerdTracker II song with a damaged header";
      return false;
    }
    const size_t chunk = kInfoHeader;
    const size_t samples = chunk + dpcmInsts * kSampInst;
    const size_t data = samples + sampleBytes;
    if (data + kDataHeader + kInstruments > file.size()) {
      error = "NerdTracker II song that is cut short";
      return false;
    }
    const std::vector<uint8_t> song(file.begin() + data, file.end());
    if (song[0] != 0x20 || song[1] != kDataHeader || song[5] != 16) {
      error = "NerdTracker II song with song data of an unknown kind";
      return false;
    }
    const int length = song[7];
    size_t need = kDataHeader + kInstruments + 4 * length;
    for (int c = 0; c < 5; c++) need += 2 * song[8 + c];
    if (length < 1 || need > song.size() || song.size() > 0x7000) {
      error = "NerdTracker II song with a damaged song header";
      return false;
    }
    if (length > 64) {
      // the NES replay routine counts orders in a byte times four; the tracker doesn't
      error = "NerdTracker II song of more than 64 orders, which the NES replay routine plays wrong";
      return false;
    }
    if (song[6] != 0) {
      error = "NerdTracker II song with DPCM instruments of a kind the tracker never wrote";
      return false;
    }
    if (track != 0) {
      info.tracks = 1;
      return true;   // the main program says "no such track"
    }

    long ticks = 0;
    bool halts = false;
    if (!Walk(song).run(static_cast<long>(kTickHz) * 3600, ticks, halts)) {
      error = "NerdTracker II song without an end";
      return false;
    }
    if (ticks < 1) {
      error = "NerdTracker II song that stops before it starts";
      return false;
    }

    Replay replay;
    if (!load_replay(replay, error)) return false;

    // What MAKEDTT.EXE does: the samples one after the other, each at a multiple of
    // 64 bytes, and for every DPCM instrument a table of four bytes a note.
    std::vector<uint8_t> dmc, table(dpcmInsts * 256, 0);
    {
      const uint8_t* inst = file.data() + chunk;
      const uint8_t* raw = file.data() + samples;
      std::vector<unsigned> offset(dpcmInsts * 8 + 1), size(dpcmInsts * 8, 0), where(dpcmInsts * 8, 0);
      for (unsigned i = 0; i < dpcmInsts; i++)
        for (int j = 0; j < 8; j++) offset[i * 8 + j] = le32(inst + i * kSampInst + 61 + j * 21 + 12);
      offset[dpcmInsts * 8] = sampleBytes;
      for (unsigned i = 0; i < dpcmInsts * 8; i++) {
        if (offset[i] == 0xffffffffu) continue;
        unsigned j = i + 1;
        while (offset[j] == 0xffffffffu) j++;   // the last one is never 0xffffffff
        if (offset[i] > sampleBytes || offset[j] > sampleBytes || offset[j] < offset[i]) {
          error = "NerdTracker II song with damaged samples";
          return false;
        }
        size[i] = offset[j] - offset[i];
      }
      for (unsigned i = 0; i < dpcmInsts * 8; i++) {
        dmc.resize((dmc.size() + 63) / 64 * 64, 0);
        if (!size[i]) continue;
        where[i] = dmc.size() >> 6;
        dmc.insert(dmc.end(), raw + offset[i], raw + offset[i] + size[i]);
      }
      if (dmc.size() > static_cast<size_t>(0x10000 - kSamples)) {
        error = "NerdTracker II song with more samples than fit into the NES's memory";
        return false;
      }
      for (unsigned i = 0; i < dpcmInsts; i++) {
        const uint8_t* p = inst + i * kSampInst;
        for (int n = 0; n < 48; n++) {
          const int smp = p[n] >> 4 & 7;
          uint8_t* t = &table[i * 256 + n * 4];
          t[2] = 0x40;                          // $4011, the level to start from
          if (!size[i * 8 + smp]) continue;     // MAKEDTT leaves these undefined
          const uint8_t* s = p + 61 + smp * 21;
          t[0] = s[18];                         // $4013, the length
          t[1] = where[i * 8 + smp];            // $4012, less 2
          t[3] = (p[n] & 0x0f) | (s[20] ? 0x40 : 0);   // $4010: the rate, looped
        }
        const std::string name = field(p + 48, 12);
        bool any = false;
        for (int j = 0; j < 8; j++) {
          const std::string sample = field(p + 61 + j * 21, 12);
          if (sample.empty() && !size[i * 8 + j]) continue;
          info.names.push_back(from_cp437(name.empty() ? sample : name + ": " + sample));
          any = true;
        }
        if (!any && !name.empty()) info.names.push_back(from_cp437(name));
      }
    }

    // The NSF: the replay routine, the DPCM table right behind it, a few bytes to
    // start it, the song where there is room for it, and the samples at $C080.
    std::vector<uint8_t> mem(0x8000, 0);
    size_t at = replay.code.size();
    std::memcpy(mem.data(), replay.code.data(), at);
    if (at + table.size() + 8 > static_cast<size_t>(kSamples - kLoad)) {
      error = "NerdTracker II song that doesn't fit into the NES's memory";
      return false;
    }
    if (!table.empty()) std::memcpy(mem.data() + at, table.data(), table.size());   // (null without DPCM)
    at += table.size();
    const int init = kLoad + static_cast<int>(at);
    size_t songAt = at + 11;
    // For comparing with NSF files made the usual way: BBMP_RETRO_NED_NTSC makes
    // this one like them, called 60 times a second and sitting out every sixth call.
    const bool ownWay = std::getenv("BBMP_RETRO_NED_NTSC") != nullptr;
    size_t end = kSamples - kLoad + dmc.size();
    if (dmc.empty()) end = 0;
    if (songAt + song.size() <= static_cast<size_t>(kSamples - kLoad)) {
      if (end < songAt + song.size()) end = songAt + song.size();
    } else {
      songAt = kSamples - kLoad + dmc.size();
      if (songAt + song.size() > mem.size()) {
        error = "NerdTracker II song that doesn't fit into the NES's memory";
        return false;
      }
      end = songAt + song.size();
    }
    const int songAddr = kLoad + static_cast<int>(songAt);
    const uint8_t stub[11] = {0xa9, static_cast<uint8_t>(ownWay ? 0x05 : 0xff),    // LDA #$FF
                              0x85, 0xff,                                          // STA NED_PAL_Counter
                              0xa9, static_cast<uint8_t>(songAddr & 0xff),         // LDA #<song
                              0xa2, static_cast<uint8_t>(songAddr >> 8),           // LDX #>song
                              0x4c, static_cast<uint8_t>(replay.setup & 0xff),     // JMP NED_SetupNED
                              static_cast<uint8_t>(replay.setup >> 8)};
    std::memcpy(mem.data() + at, stub, sizeof(stub));
    std::memcpy(mem.data() + songAt, song.data(), song.size());
    if (!dmc.empty()) std::memcpy(mem.data() + (kSamples - kLoad), dmc.data(), dmc.size());

    std::vector<uint8_t> nsf(0x80, 0);
    std::memcpy(nsf.data(), "NESM\x1a\x01", 6);
    nsf[6] = 1;                      // songs
    nsf[7] = 1;                      // the first one
    nsf[8] = kLoad & 0xff;
    nsf[9] = kLoad >> 8;
    nsf[10] = init & 0xff;
    nsf[11] = init >> 8;
    const int play = ownWay ? replay.nsfPlay : replay.play;
    nsf[12] = play & 0xff;
    nsf[13] = play >> 8;
    // An NTSC machine, for that is what the replay routine's notes are tuned to, but
    // called 50 times a second, as the tracker does. (The routine's own way on an
    // NTSC machine is to be called 60 times and to sit out every sixth.)
    const unsigned period = ownWay ? 0x411a : static_cast<unsigned>(1000000.0 / kTickHz + 0.5);
    nsf[0x6e] = period & 0xff;
    nsf[0x6f] = period >> 8;
    nsf[0x78] = 0x20;                // and the PAL speed, which isn't used
    nsf[0x79] = 0x4e;
    nsf[0x7a] = 0;                   // NTSC
    nsf.insert(nsf.end(), mem.begin(), mem.begin() + end);

    if (const char* dump = std::getenv("BBMP_RETRO_NED_DUMP")) {
      if (FILE* f = std::fopen(dump, "wb")) {
        std::fwrite(nsf.data(), 1, nsf.size(), f);
        std::fclose(f);
      }
    }
    if (gme_err_t err = gme_open_data(nsf.data(), static_cast<long>(nsf.size()), &emu_, rate)) {
      error = std::string("Game Music Emu: ") + err;
      return false;
    }
    gme_ignore_silence(emu_, 1);
    if (gme_err_t err = gme_start_track(emu_, 0)) {
      error = std::string("Game Music Emu: ") + err;
      return false;
    }

    // the replay routine is called for the first time one tick after the start
    const long long body = static_cast<long long>((ticks + 1) * rate / kTickHz + 0.5);
    // A song can halt up to an hour in; keeping all of it would take gigabytes. The
    // longest real song is some six minutes; one of more than 2^27 frames (512 MB;
    // 46 minutes at 48 kHz, 11 at 192 kHz) isn't looked at but played as one whose
    // last notes don't die away.
    const bool tooLong = body + static_cast<long long>(kTailMs) * rate / 1000 > kMostBuffered;
    if (halts && tooLong) {
      fade_ = static_cast<long long>(kFadeMs) * rate / 1000;
      total_ = body + static_cast<long long>(kTailMs) * rate / 1000;
    } else if (halts) {
      // The song stops itself and what sounds then is left to fade, or to sound
      // for ever. Play it all now to know which.
      const long long most = body + static_cast<long long>(kTailMs) * rate / 1000;
      ready_.resize(static_cast<size_t>(most) * 2);
      if (gme_play(emu_, static_cast<int>(ready_.size()), ready_.data())) {
        error = "Game Music Emu stopped playing";
        return false;
      }
      long long last = most;
      while (last > body && std::abs(ready_[2 * (last - 1)]) < 8 &&
             std::abs(ready_[2 * (last - 1) + 1]) < 8)
        last--;
      if (last >= most - rate / 10) {
        fade_ = static_cast<long long>(kFadeMs) * rate / 1000;   // it never gets quiet
        total_ = most;
      } else {
        total_ = last + rate / 4;
        if (total_ > most) total_ = most;
      }
      buffered_ = true;
    } else {
      fade_ = static_cast<long long>(kFadeMs) * rate / 1000;
      total_ = body + fade_;
    }

    const std::string name = field(h + 5, 29);
    if (name != "Noname") info.title = from_cp437(name);   // what the tracker always writes
    info.type = "NerdTracker II";
    info.tracks = 1;
    info.duration_ms = total_ * 1000 / rate;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d orders, %s", length,
                  halts ? "stops itself" : "loops: played once, then faded");
    info.comment = buf;
    if (debug_on())
      std::fprintf(stderr, "ned: %ld ticks, %s, song at $%04x, %zu bytes of samples\n", ticks,
                   halts ? "halts" : "loops", songAddr, dmc.size());
    return true;
  }

  size_t render(int16_t* out, size_t frames) override {
    if (!emu_ || done_ >= total_) return 0;
    size_t n = frames;
    if (static_cast<long long>(n) > total_ - done_) n = static_cast<size_t>(total_ - done_);
    if (buffered_) {
      std::memcpy(out, ready_.data() + 2 * done_, n * 4);
    } else if (gme_play(emu_, static_cast<int>(n * 2), out)) {
      return 0;
    }
    if (fade_ > 0 && done_ + static_cast<long long>(n) > total_ - fade_) {
      for (size_t i = 0; i < n; i++) {
        const long long left = total_ - (done_ + static_cast<long long>(i));
        if (left >= fade_) continue;
        const double g = static_cast<double>(left) / static_cast<double>(fade_);
        out[2 * i] = static_cast<int16_t>(out[2 * i] * g * g);
        out[2 * i + 1] = static_cast<int16_t>(out[2 * i + 1] * g * g);
      }
    }
    done_ += static_cast<long long>(n);
    return n;
  }

 private:
  Music_Emu* emu_ = nullptr;
  long long total_ = 0, done_ = 0, fade_ = 0;
  bool buffered_ = false;
  std::vector<int16_t> ready_;
};

}  // namespace

REGISTER_ENGINE("ned", NedEngine)
