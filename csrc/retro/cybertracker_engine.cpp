// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-retro, engine "cybertracker": songs of CyberTracker (CyberBrain of No Name, 2001,
// for the C64). No stand-alone player of them exists, so the tracker itself, version
// 1.01 from its disk image, runs in a small emulated C64 (a 6502, banked memory, the
// raster interrupt, a reSIDfp SID): its own load routine reads the song, its "play
// song" routine starts it, and what it writes to the SID is heard.
//
// Written for bbmp as a clean-room rewrite from a description of how to drive the
// tracker (the places in it that are used, and the steps); the small C64, the disk
// image reader and the screen codes are kept from bbmp's earlier engine.
//
// BBMP_CYBERTRACKER_DISK  the disk image (default <exe dir>/../share/cybertracker101.d64)
// BBMP_CYBERTRACKER_SIDLOG a file to write every SID write of the sound to, with its cycle

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <unistd.h>

#include <residfp/residfp.h>

#include "cybertracker_6502.h"
#include "self_path.h"
#include "engine.h"

namespace {

using namespace retro;

const double kClock = 985248.0;          // PAL C64
const unsigned kCyclesPerLine = 63;
const unsigned kLines = 312;
const unsigned kCyclesPerFrame = kCyclesPerLine * kLines;

class C64 {
 public:
  uint8_t ram[65536];
  uint8_t io[4096];
  Cpu6502<C64> cpu;
  unsigned line = 0;
  uint64_t lineStart = 0;
  uint8_t irqFlags = 0, irqMask = 0;
  reSIDfp::residfp* sid = nullptr;       // none: a run to measure the song
  uint64_t sidClock = 0;                 // how far the SID has been run
  std::deque<int16_t>* out = nullptr;
  const std::vector<uint8_t>* disk = nullptr;   // the file that the disk drive gives
  size_t diskAt = 0;
  FILE* log = nullptr;                   // the SID write log, when one is asked for
  uint64_t logFrom = 0;                  // its cycles count from here

  C64() : cpu(*this) {
    std::memset(ram, 0, sizeof(ram));
    std::memset(io, 0, sizeof(io));
    ram[0] = 0x2f;
    ram[1] = 0x37;
  }
  C64(const C64& other) : cpu(*this) { *this = other; }
  C64& operator=(const C64& other) {
    std::memcpy(ram, other.ram, sizeof(ram));
    std::memcpy(io, other.io, sizeof(io));
    copyCpu(other.cpu);
    line = other.line;
    lineStart = other.lineStart;
    irqFlags = other.irqFlags;
    irqMask = other.irqMask;
    sid = other.sid;
    sidClock = other.sidClock;
    out = other.out;
    disk = other.disk;
    diskAt = other.diskAt;
    log = other.log;
    logFrom = other.logFrom;
    return *this;
  }

  bool ioOn() const { return (ram[1] & 3) && (ram[1] & 4); }
  bool kernalOn() const { return ram[1] & 2; }

  uint8_t read(uint16_t address) {
    if (address >= 0xd000 && address < 0xe000 && ioOn()) {
      switch (address) {
        case 0xd011: return (io[0x11] & 0x7f) | (line & 0x100 ? 0x80 : 0);
        case 0xd012: return line & 0xff;
        case 0xd019: return irqFlags | 0x70 | ((irqFlags & irqMask) ? 0x80 : 0);
        case 0xd01a: return irqMask | 0xf0;
        case 0xdc00: case 0xdc01: return 0xff;      // no key is down
        case 0xdc0d: case 0xdd0d: return 0;
      }
      if (address >= 0xd400 && address < 0xd800 && sid) {
        runSid();
        return sid->read(address & 0x1f);
      }
      return io[address - 0xd000];
    }
    return ram[address];
  }

  void write(uint16_t address, uint8_t value) {
    if (address >= 0xd000 && address < 0xe000 && ioOn()) {
      io[address - 0xd000] = value;
      if (address == 0xd019) {
        irqFlags &= ~value & 0x0f;
      } else if (address == 0xd01a) {
        irqMask = value & 0x0f;
      } else if (address >= 0xd400 && address < 0xd800 && sid) {
        runSid();
        sid->write(address & 0x1f, value);
        if (log)
          std::fprintf(log, "%llu %02x %02x\n",
                       static_cast<unsigned long long>(cpu.cycles - logFrom), address & 0x1f, value);
      }
      return;
    }
    ram[address] = value;
  }

  // The SID catches up with the processor.
  void runSid() {
    while (sidClock < cpu.cycles) {
      const uint64_t left = cpu.cycles - sidClock;
      const unsigned now = left > 2048 ? 2048 : static_cast<unsigned>(left);
      short buf[512];
      const int got = sid->clock(now, buf);
      for (int k = 0; k < got; k++) out->push_back(buf[k]);
      sidClock += now;
    }
  }

  void step() {
    if (cpu.pc >= 0xe000 && kernalOn()) {
      kernal();
    } else {
      cpu.step();
    }
    while (cpu.cycles - lineStart >= kCyclesPerLine) {
      lineStart += kCyclesPerLine;
      line = (line + 1) % kLines;
      const unsigned compare = io[0x12] | (io[0x11] & 0x80) << 1;
      if (line == compare) irqFlags |= 1;
    }
    if ((irqFlags & irqMask) && !cpu.i && !kernalOn()) {
      cpu.push(cpu.pc >> 8);
      cpu.push(cpu.pc & 0xff);
      cpu.push(cpu.flags());
      cpu.i = true;
      cpu.pc = ram[0xfffe] | ram[0xffff] << 8;
      cpu.cycles += 7;
    }
  }

 private:
  // There is no KERNAL ROM here. The tracker only calls it for the disk drive: when
  // there is a file to give, whatever is opened is that file, and otherwise there
  // is no drive.
  void kernal() {
    uint8_t status = disk ? ram[0x90] : 0x80;
    switch (cpu.pc) {
      case 0xfe00:              // SETLFS
      case 0xfdf9:              // SETNAM
        break;
      case 0xfe07:              // READST
        cpu.a = status;
        cpu.z = status == 0;
        cpu.n = status & 0x80;
        break;
      case 0xf157:              // CHRIN
        if (disk && diskAt < disk->size()) {
          cpu.a = (*disk)[diskAt++];
          status = diskAt == disk->size() ? 0x40 : 0;
        } else {
          cpu.a = 0x0d;
          status = disk ? 0x42 : 0x80;
        }
        cpu.c = false;
        break;
      default:                  // OPEN, CHKIN, CLOSE, CLRCHN and the serial bus
        cpu.c = !disk;
        if (disk) status = 0;
        else cpu.a = 5;         // device not present
        break;
    }
    ram[0x90] = status;
    const uint8_t lo = cpu.pull();
    cpu.pc = (lo | cpu.pull() << 8) + 1;
    cpu.cycles += 20;
  }

  void copyCpu(const Cpu6502<C64>& from) {
    cpu.a = from.a; cpu.x = from.x; cpu.y = from.y; cpu.sp = from.sp; cpu.pc = from.pc;
    cpu.n = from.n; cpu.v = from.v; cpu.d = from.d; cpu.i = from.i; cpu.z = from.z; cpu.c = from.c;
    cpu.cycles = from.cycles;
    cpu.jammed = from.jammed;
  }
};

std::string shareFile(const char* name) {
  const std::string exe = self_path();
  if (exe.empty()) return name;
  return dir_of(exe) + "../share/" + name;
}

// One file out of a 1541 disk image.
std::vector<uint8_t> fromDisk(const std::vector<uint8_t>& disk, const std::string& wanted) {
  auto offset = [](unsigned track, unsigned sector) -> size_t {
    size_t sectors = 0;
    for (unsigned t = 1; t < track; t++) sectors += t <= 17 ? 21 : t <= 24 ? 19 : t <= 30 ? 18 : 17;
    return (sectors + sector) * 256;
  };
  auto sectorAt = [&](unsigned track, unsigned sector) -> const uint8_t* {
    if (track < 1 || track > 40 || sector > 20) return nullptr;
    const size_t at = offset(track, sector);
    return at + 256 <= disk.size() ? disk.data() + at : nullptr;
  };
  std::vector<uint8_t> file;
  unsigned track = 18, sector = 1;
  for (int guard = 0; guard < 40 && track; guard++) {
    const uint8_t* dir = sectorAt(track, sector);
    if (!dir) break;
    for (int e = 0; e < 8; e++) {
      const uint8_t* entry = dir + e * 32;
      if ((entry[2] & 7) == 0) continue;
      std::string name(reinterpret_cast<const char*>(entry + 5), 16);
      while (!name.empty() && static_cast<unsigned char>(name.back()) == 0xa0) name.pop_back();
      if (name != wanted) continue;
      unsigned t = entry[3], s = entry[4];
      for (int blocks = 0; blocks < 800 && t; blocks++) {
        const uint8_t* data = sectorAt(t, s);
        if (!data) return {};
        const size_t bytes = data[0] ? 254 : (data[1] >= 1 ? data[1] - 1 : 0);
        file.insert(file.end(), data + 2, data + 2 + bytes);
        t = data[0];
        s = data[1];
      }
      return file;
    }
    track = dir[0];
    sector = dir[1];
  }
  return file;
}

// The tracker shows capital letters; its texts are in screen codes.
std::string fromScreenCodes(const uint8_t* p, size_t size) {
  std::string text;
  for (size_t k = 0; k < size; k++) {
    const uint8_t c = p[k] & 0x7f;
    if (c == 0) text += '@';
    else if (c <= 26) text += static_cast<char>('A' + c - 1);
    else if (c >= 32 && c < 64) text += static_cast<char>(c);
    else text += ' ';
  }
  while (!text.empty() && text.back() == ' ') text.pop_back();
  return text;
}

// ---- What the engine knows of the song files and of the tracker -------------------

// the 17 blocks of a song and the most the tracker keeps of each
const unsigned kBlockMax[17] = {32, 32, 32, 64, 512, 512, 256, 256, 256,
                                768, 768, 768, 768, 7168, 256, 256, 512};
const uint8_t kMagic[10] = {0x00, 0x04, 'N', 'N', 'T', 'R', 'K', 'M', 'Z', 'X'};

// places in CyberTracker 1.01, once its decruncher has run
namespace at {
const uint16_t kDecruncher = 0x0821, kTitle = 0x0900, kStart = 0x1500, kMainLoop = 0x1600;
const uint16_t kDiskMode = 0x1e09, kSavePages = 0x1d6c, kLoad = 0x1e5c, kQuestion = 0x78f7;
const uint16_t kOldVersionText = 0x21b8, kLoadAnyway = 0x21af, kLoadDone = 0x18da;
const uint16_t kTrackerMode = 0x1dfd, kRestorePages = 0x1dab, kPatternPointers = 0x1df4;
const uint16_t kDiskFlag = 0x2af5, kTrackToPattern = 0x4abf, kPlaySong = 0x36d4;
const uint16_t kNextRow = 0x9d1b, kSongNumber = 0x49dd, kTrackPos = 0x49de, kRow = 0x39ce;
const uint16_t kSpeed = 0x3f63, kIrqMode = 0x16b4, kSongLengths = 0x0800, kNames = 0x0200;
}  // namespace at

const uint32_t kProgramHash = 0xb0f0e6d7u;
const uint64_t kCallLimit = 1000000;
const uint64_t kHour = 985248ull * 3600;
const uint64_t kTail = 1970496;          // two seconds after the song stops itself

struct Refusal {
  std::string why;
};

// How one run of the song goes, and where it ends.
struct Run {
  bool visited[256] = {};
  bool ended = false;
  uint64_t endedAt = 0;
  uint64_t stopAt = 0;
  std::string reason;

  void end(uint64_t cycle, const std::string& r) { ended = true; endedAt = cycle; reason = r; }

  void step(C64& m) {
    if (m.cpu.pc == at::kNextRow && m.ram[at::kRow] == 0) {
      const uint8_t pos = m.ram[at::kTrackPos];
      if (visited[pos]) { end(m.cpu.cycles, "back at a track position it has played"); return; }
      visited[pos] = true;
    }
    const uint16_t before = m.cpu.pc;
    m.step();
    if (m.cpu.jammed) { end(m.cpu.cycles, "the processor stopped"); return; }
    const uint16_t vector = m.ram[0xfffe] | m.ram[0xffff] << 8;
    if (m.cpu.pc == vector && before != vector && stopAt == 0) {
      const uint8_t mode = m.ram[at::kIrqMode], speed = m.ram[at::kSpeed];
      if ((mode == 2 && speed == 0) || mode == 0 || mode > 2) {
        stopAt = m.cpu.cycles + kTail;
        reason = mode == 2 ? "speed 0: the song stops" : "the tracker stopped playing";
      }
    }
    if (stopAt && m.cpu.cycles >= stopAt) end(m.cpu.cycles, reason);
  }
};

void checkSong(const std::vector<uint8_t>& f) {
  if (f.size() < 13 || std::memcmp(f.data(), kMagic, 10) != 0)
    throw Refusal{"not a CyberTracker song"};
  if (f[11] != 1 || f[10] > 1) {
    char text[96];
    std::snprintf(text, sizeof(text), "a song of CyberTracker %u.%02u, which is newer than 1.01",
                  f[11], f[10]);
    throw Refusal{text};
  }
  if (f[12] != 17) throw Refusal{"not the 17 blocks of a CyberTracker song"};
  size_t pos = 13;
  for (unsigned b = 0; b < 17; b++) {
    if (pos + 2 > f.size()) throw Refusal{"the file ends within its blocks"};
    const unsigned len = f[pos] | f[pos + 1] << 8;
    pos += 2;
    if (len > kBlockMax[b]) throw Refusal{"a block is longer than the tracker's memory for it"};
    if (pos + len > f.size()) throw Refusal{"the file ends within its blocks"};
    pos += len;
  }
}

// Loads the tracker from its disk image and lets it come up to its editor.
void boot(C64& m) {
  const char* env = std::getenv("BBMP_CYBERTRACKER_DISK");
  const std::string path = env && *env ? env : shareFile("cybertracker101.d64");
  const std::vector<uint8_t> image = read_file(path);
  if (image.empty()) throw Refusal{"CyberTracker's disk image is missing: " + path};
  const std::vector<uint8_t> prg = fromDisk(image, "CYBERTRACKER1.01");
  if (prg.size() < 4096 || prg.size() > 61440 || (prg[0] | prg[1] << 8) != 0x0801)
    throw Refusal{"no CyberTracker 1.01 on the disk image " + path};
  std::memcpy(m.ram + 0x0801, prg.data() + 2, prg.size() - 2);
  const unsigned end = 0x0801 + prg.size() - 2;
  m.ram[0x2d] = m.ram[0xae] = end & 0xff;
  m.ram[0x2e] = m.ram[0xaf] = end >> 8;

  m.cpu.pc = at::kDecruncher;
  for (long k = 0; k < 20000000 && m.cpu.pc != at::kTitle && !m.cpu.jammed; k++) m.step();
  uint32_t hash = 2166136261u;
  for (unsigned a = 0x1500; a <= 0xacff; a++) hash = (hash ^ m.ram[a]) * 16777619u;
  if (m.cpu.pc != at::kTitle || hash != kProgramHash)
    throw Refusal{"the program on the disk image is not the CyberTracker 1.01 that is known here"};

  m.cpu.pc = at::kStart;
  m.cpu.sp = 0xff;
  const uint64_t c0 = m.cpu.cycles;
  bool idle = false;
  while (m.cpu.cycles - c0 < 500ull * kCyclesPerFrame && !m.cpu.jammed) {
    if (m.cpu.pc == at::kMainLoop && m.cpu.cycles - c0 > 40ull * kCyclesPerFrame && !m.cpu.i) {
      idle = true;
      break;
    }
    m.step();
  }
  if (!idle) throw Refusal{"the tracker didn't start"};
}

// The tracker's own "load music", as if the file had been chosen in its file selector.
void load(C64& m, const std::vector<uint8_t>& song) {
  const Refusal failed{"the tracker didn't get through the song's data"};
  m.cpu.i = true;
  if (!m.cpu.call(at::kDiskMode, kCallLimit) || !m.cpu.call(at::kSavePages, kCallLimit))
    throw failed;
  m.ram[at::kDiskFlag] = 1;
  m.disk = &song;
  m.diskAt = 0;
  m.cpu.pc = at::kLoad;
  m.cpu.sp = 0xf0;
  for (uint64_t k = 0; m.cpu.pc != at::kLoadDone; k++) {
    if (k > 200000000 || m.cpu.jammed) throw failed;
    if (m.cpu.pc == at::kQuestion) {
      // only the question about songs of version 1.00 is answered (yes)
      if ((m.cpu.x | m.cpu.y << 8) != at::kOldVersionText) throw failed;
      m.cpu.pc = at::kLoadAnyway;
    }
    m.step();
  }
  m.disk = nullptr;
  m.cpu.i = true;
  if (!m.cpu.call(at::kTrackerMode, kCallLimit) || !m.cpu.call(at::kRestorePages, kCallLimit))
    throw failed;
  m.ram[at::kDiskFlag] = 0;
  if (!m.cpu.call(at::kPatternPointers, kCallLimit)) throw failed;
}

class CyberTracker : public Engine {
 public:
  ~CyberTracker() override {
    if (log_) std::fclose(log_);
  }

  bool open(const std::vector<uint8_t>& file, const std::string&, int track, int rate,
            SongInfo& info, std::string& error) override {
    try {
      checkSong(file);
      song_ = file;
      machine_.reset(new C64);
      C64& m = *machine_;
      boot(m);
      load(m, song_);

      int songs = 0, positions = 0;
      while (songs < 256 && m.ram[at::kSongLengths + songs]) positions += m.ram[at::kSongLengths + songs++];
      if (songs == 0) throw Refusal{"the song is empty"};
      if (positions > 512) throw Refusal{"the songs are longer than the tracker's track memory"};
      if (track >= songs) {
        info.tracks = songs;
        throw Refusal{"no such track"};
      }

      m.ram[at::kSongNumber] = track;
      m.ram[at::kTrackPos] = 0;
      if (!m.cpu.call(at::kTrackToPattern, kCallLimit) || !m.cpu.call(at::kPlaySong, kCallLimit) ||
          m.ram[at::kIrqMode] != 1)
        throw Refusal{"the tracker didn't start playing"};
      m.cpu.pc = at::kMainLoop;
      m.cpu.sp = 0xff;
      m.cpu.i = false;

      info.tracks = songs;
      info.type = file[10] ? "CyberTracker 1.01" : "CyberTracker 1.00";
      for (int n = 1; n <= 31; n++) info.names.push_back(fromScreenCodes(m.ram + at::kNames + 16 * n, 16));
      while (!info.names.empty() && info.names.back().empty()) info.names.pop_back();

      // a dry run on a copy of the machine, without a SID, finds the length
      const uint64_t from = m.cpu.cycles;
      hourEnd_ = from + kHour;
      std::string dryReason = "no end found";
      uint64_t dryLength = kHour;
      {
        std::unique_ptr<C64> dry(new C64(m));
        dry->sid = nullptr;
        dry->log = nullptr;
        Run run;
        while (!run.ended && dry->cpu.cycles < hourEnd_) run.step(*dry);
        if (run.ended) {
          info.duration_ms = static_cast<long long>((run.endedAt - from) * 1000.0 / kClock);
          dryReason = run.reason;
          dryLength = run.endedAt - from;
        }
      }

      sid_.reset(new reSIDfp::residfp);
      bool ok = false;
      try {
        sid_->setChipModel(reSIDfp::MOS6581);
        sid_->reset();
        ok = sid_->setSamplingParameters(kClock, reSIDfp::RESAMPLE, rate);
      } catch (...) {
        ok = false;
      }
      if (!ok) throw Refusal{"the SID can't do this sample rate"};
      m.sid = sid_.get();
      m.out = &waiting_;
      m.sidClock = m.cpu.cycles;

      const char* logPath = std::getenv("BBMP_CYBERTRACKER_SIDLOG");
      if (logPath && *logPath && (log_ = std::fopen(logPath, "w"))) {
        std::fprintf(log_, "# cybertracker sid log 1: track %d of %d, dry run %s at %llu\n", track,
                     songs, dryReason.c_str(), static_cast<unsigned long long>(dryLength));
        m.log = log_;
        m.logFrom = from;
      }
      return true;
    } catch (const Refusal& r) {
      error = r.why;
      return false;
    }
  }

  size_t render(int16_t* out, size_t frames) override {
    if (!machine_) return 0;
    C64& m = *machine_;
    while (waiting_.size() < frames && !done_) {
      const uint64_t target = m.cpu.cycles + 4096;
      while (m.cpu.cycles < target && !done_) {
        run_.step(m);
        if (run_.ended || m.cpu.cycles >= hourEnd_) {
          done_ = true;
          if (log_) {
            std::fprintf(log_, "END %llu %s\n",
                         static_cast<unsigned long long>(m.cpu.cycles - m.logFrom),
                         run_.ended ? run_.reason.c_str() : "an hour");
            std::fflush(log_);
          }
        }
      }
      m.runSid();
    }
    const size_t n = waiting_.size() < frames ? waiting_.size() : frames;
    for (size_t k = 0; k < n; k++) {
      out[2 * k] = out[2 * k + 1] = waiting_.front();
      waiting_.pop_front();
    }
    return n;
  }

 private:
  std::vector<uint8_t> song_;
  std::unique_ptr<C64> machine_;
  std::unique_ptr<reSIDfp::residfp> sid_;
  std::deque<int16_t> waiting_;
  Run run_;
  bool done_ = false;
  uint64_t hourEnd_ = 0;
  FILE* log_ = nullptr;
};

}  // namespace

REGISTER_ENGINE("cybertracker", CyberTracker)
