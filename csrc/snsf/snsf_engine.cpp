// SPDX-License-Identifier: MIT
// SNES (SNSF) through snsf9x (snes9x trimmed for sound) and psflib, behind a tiny C
// interface: snes9x's headers define TRUE, uint8, Settings... which stay in here.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <unistd.h>
#include <string>
#include <vector>

#include "psflib.h"
#include "SNESSystem.h"

#include "snsf_engine.h"

namespace {

struct State {
  std::vector<uint8_t> rom, sram;
  bool haveBase = false;
  uint32_t base = 0;
  long lengthMs = -1, fadeMs = -1;
  std::string title, artist, game, year, copyright, ripper, comment;
};

State st;
SNESSystem* sys;

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

long parseTime(const char* s) {
  double total = 0, part = 0;
  bool seen = false;
  for (const char* p = s; *p; p++) {
    if (*p == ':') { total = (total + part) * 60; part = 0; }
    else if ((*p >= '0' && *p <= '9') || *p == '.' || *p == ',') {
      char* end; part = std::strtod(p, &end); p = end - 1; seen = true;
    } else if (*p != ' ') return -1;
  }
  return seen ? long((total + part) * 1000) : -1;
}

int onTag(void*, const char* name, const char* value) {
  if (!strcasecmp(name, "length")) st.lengthMs = parseTime(value);
  else if (!strcasecmp(name, "fade")) st.fadeMs = parseTime(value);
  else if (!strcasecmp(name, "title")) st.title = value;
  else if (!strcasecmp(name, "artist")) st.artist = value;
  else if (!strcasecmp(name, "game")) st.game = value;
  else if (!strcasecmp(name, "year")) st.year = value;
  else if (!strcasecmp(name, "copyright")) st.copyright = value;
  else if (!strcasecmp(name, "snsfby")) st.ripper = value;
  else if (!strcasecmp(name, "comment")) st.comment = value;
  return 0;
}

// Program section: base offset, size, ROM data. The first file loaded (the deepest
// _lib) sets the base; later files patch at their offset relative to it.
// Reserved section: SRAM patches as (offset, size, data) records.
// snes9x holds 8 MiB of ROM and 128 KiB of SRAM and copies what it is given into
// those: what lies beyond is left out here (snes9x would cut the ROM there too).
const uint64_t kMaxRom = 0x800000, kMaxSram = 0x20000;

// data[offset, offset + size) into v, as far as it is below `limit`
void patch(std::vector<uint8_t>& v, uint64_t offset, const uint8_t* data, uint64_t size, uint64_t limit) {
  if (offset >= limit) return;
  if (size > limit - offset) size = limit - offset;
  if (offset + size > v.size()) v.resize(offset + size);
  std::memcpy(v.data() + offset, data, size);
}

int onExe(void*, const uint8_t* exe, size_t exeSize, const uint8_t* res, size_t resSize) {
  if (exeSize >= 8) {
    uint64_t offset = le32(exe), size = le32(exe + 4);
    if (size > exeSize - 8) return -1;
    if (!st.haveBase) { st.base = uint32_t(offset); st.haveBase = true; }
    else offset += st.base;
    patch(st.rom, offset, exe + 8, size, kMaxRom);
  }
  while (resSize >= 8) {
    uint64_t offset = le32(res), size = le32(res + 4);
    res += 8; resSize -= 8;
    if (size > resSize) return -1;
    patch(st.sram, offset, res, size, kMaxSram);
    res += size; resSize -= size;
  }
  return 0;
}

void* fOpen(const char* uri) { return std::fopen(uri, "rb"); }
size_t fRead(void* buf, size_t size, size_t n, void* h) { return std::fread(buf, size, n, (FILE*)h); }
int fSeek(void* h, int64_t ofs, int whence) { return std::fseek((FILE*)h, long(ofs), whence); }
int fClose(void* h) { return std::fclose((FILE*)h); }
long fTell(void* h) { return std::ftell((FILE*)h); }
const psf_file_callbacks files = {"/", fOpen, fRead, fSeek, fClose, fTell};

struct Out : SNESSoundOut {
  std::vector<int16_t> buf;
  void write(const void* samples, unsigned long bytes) override {
    const int16_t* s = static_cast<const int16_t*>(samples);
    buf.insert(buf.end(), s, s + bytes / 2);
  }
} out;

}  // namespace

extern "C" const char* snsfw_open(const char* path, int rate, struct snsfw_info* info) {
  st = State();
  if (psf_load(path, &files, 0x23, onExe, nullptr, onTag, nullptr, 0) <= 0 || st.rom.empty())
    return "not an SNSF file";
  // snes9x prints status lines on stdout, which carries our audio: park them on stderr
  std::fflush(stdout);
  const int saved = dup(1);
  dup2(2, 1);
  sys = new SNESSystem();
  sys->soundSampleRate = rate;
  const bool loaded =
      sys->Load(st.rom.data(), st.rom.size(), st.sram.empty() ? nullptr : st.sram.data(), st.sram.size());
  if (loaded) {
    sys->SoundInit(&out);
    sys->SoundReset();
    sys->Init();
  }
  std::fflush(stdout);
  dup2(saved, 1);
  close(saved);
  if (!loaded) return "snes9x couldn't load the ROM image";
  info->game = st.game.c_str();
  info->year = st.year.c_str();
  info->copyright = st.copyright.c_str();
  info->ripper = st.ripper.c_str();
  info->comment = st.comment.c_str();
  info->title = !st.title.empty() ? st.title.c_str() : st.game.c_str();
  info->author = st.artist.c_str();
  info->length_ms = st.lengthMs;
  info->fade_ms = st.fadeMs;
  return nullptr;
}

extern "C" int snsfw_render(int16_t* buf, unsigned frames) {
  while (out.buf.size() < frames * 2) {
    const size_t before = out.buf.size();
    sys->CPULoop();
    if (out.buf.size() == before && out.buf.empty()) return -1;
  }
  std::memcpy(buf, out.buf.data(), frames * 4);
  out.buf.erase(out.buf.begin(), out.buf.begin() + frames * 2);
  return int(frames);
}

extern "C" void snsfw_close(void) {
  delete sys;
  sys = nullptr;
}
