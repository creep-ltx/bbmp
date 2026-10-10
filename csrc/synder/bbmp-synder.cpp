// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-synder: play a Synder SNG file (Modland's "Synder SNG-Player" and "Synder
// SNG-Player Stereo"), as raw PCM on stdout.
//
//   bbmp-synder [--clean] <file> <track> <samplerate> [start-ms]
//   bbmp-synder [--clean] --ref <file>     the song at 48000 Hz, no header line
//   bbmp-synder --unpack <file>            the unpacked frame data
//
// The files hold, for each voice and each frame, six synthesizer parameters (a pitch in
// hertz, a noise switch, a stereo placement, a waveform shape, a loudness and a low-pass
// coefficient), recorded from C64 tunes. DaNyL's SNG-Player plays them on a small
// wavetable synthesizer at 48000 Hz. This is a new player, written for bbmp from a
// description of how that program behaves (a clean room: its code was not looked at).
//
// By default it sounds exactly as the original program, fault included: with more than
// four voices the original keeps voices 4-7 in the same memory as its noise table, so
// those voices start from the noise table's bytes and noisy voices read the voices'
// state as noise (this shapes how the 6- and 8-voice songs sound). --clean keeps them
// apart. The song is played once, T frames, to its end.
//
// Same protocol as the other helpers: one JSON line describing the track, then signed
// 16-bit little-endian stereo until the song ends. A damaged file is refused with the
// reason; nothing in it is trusted (sizes, offsets, the packed stream).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "resample.h"

namespace {

const int SYNTH_RATE = 48000;
const size_t MAX_FILE = 16u << 20;      // the largest known song is 25 KB
const size_t MAX_UNPACKED = 64u << 20;
const size_t MAX_RATIO = 1024;          // the songs unpack to at most 223 times their size
const long long MAX_SECONDS = 30 * 60;

// ---- the file

struct Song {
  int voices = 0, rate = 0;
  bool planar = true;
  uint32_t loop = 0;
  std::string text;
  std::vector<uint8_t> data;   // the unpacked frame data
  size_t frames = 0;           // T
  uint8_t at(int v, size_t f, int k) const {
    const size_t V = size_t(voices);
    return planar ? data[size_t(k) * frames * V + size_t(v) * frames + f]
                  : data[f * 6 * V + size_t(v) * 6 + size_t(k)];
  }
};

uint32_t le16(const uint8_t* p) { return p[0] | p[1] << 8; }
uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

// The packed frame data: aPLib's LZ scheme in its plain form (no header).
class Depacker {
 public:
  Depacker(const uint8_t* in, size_t n) : in_(in), n_(n) {}

  // Empty when the stream unpacked to exactly out.size() bytes
  std::string run(std::vector<uint8_t>& out) {
    const size_t U = out.size();
    size_t o = 0;
    uint64_t prev = 0;
    bool afterMatch = false;
    if (U == 0) return "nothing to unpack";
    out[o++] = byte();
    for (;;) {
      if (bad_) return "the packed data ends too soon";
      if (!bit()) {                          // 0: a byte as is
        uint8_t b = byte();
        if (bad_) continue;
        if (o >= U) return "the packed data holds more than the size the file gives";
        out[o++] = b;
        afterMatch = false;
        continue;
      }
      uint64_t dist, len;
      if (!bit()) {                          // 1 0: a long match
        uint64_t g = gamma();
        if (!afterMatch && g == 2) {
          dist = prev;
          len = gamma();
        } else {
          uint64_t high = g - (afterMatch ? 2 : 3);
          dist = high * 256 + byte();
          len = gamma();
          if (dist >= 32000) len++;
          if (dist >= 1280) len++;
          if (dist < 128) len += 2;
          prev = dist;
        }
      } else if (!bit()) {                   // 1 1 0: a short match, or the end
        uint8_t b = byte();
        if (bad_) continue;
        if (b == 0) break;
        dist = b >> 1;
        len = 2 + (b & 1);
        prev = dist;
      } else {                               // 1 1 1: one byte from up to 15 back, or 0
        int d = 0;
        for (int i = 0; i < 4; ++i) d = d * 2 + bit();
        if (bad_) continue;
        if (o >= U) return "the packed data holds more than the size the file gives";
        if (d > 0 && size_t(d) > o) return "the packed data refers to bytes before its start";
        out[o] = d ? out[o - size_t(d)] : 0;
        ++o;
        afterMatch = false;
        continue;
      }
      if (bad_) continue;
      if (dist == 0 || dist > o) return "the packed data refers to bytes before its start";
      if (len > U - o) return "the packed data holds more than the size the file gives";
      for (uint64_t i = 0; i < len; ++i, ++o) out[o] = out[o - dist];
      afterMatch = true;
    }
    if (o != U) return "the packed data holds less than the size the file gives";
    return "";
  }

 private:
  uint8_t byte() {
    if (pos_ >= n_) {
      bad_ = true;
      return 0;
    }
    return in_[pos_++];
  }
  int bit() {
    if (left_ == 0) {
      tag_ = byte();
      left_ = 8;
    }
    --left_;
    const int b = tag_ >> 7 & 1;
    tag_ = uint8_t(tag_ << 1);
    return b;
  }
  uint64_t gamma() {
    uint64_t v = 1;
    do {
      if (v >= (uint64_t(1) << 40)) {   // more than any file could hold
        bad_ = true;
        return 0;
      }
      v = v * 2 + uint64_t(bit());
    } while (bit() && !bad_);
    return v;
  }

  const uint8_t* in_;
  size_t n_, pos_ = 0;
  uint8_t tag_ = 0;
  int left_ = 0;
  bool bad_ = false;
};

// Empty when the song can be played
std::string load(const std::vector<uint8_t>& f, Song& s) {
  if (f.size() >= 8 && (!std::memcmp(f.data(), "SYNDAPS0", 8) || !std::memcmp(f.data(), "SYNDERS0", 8)))
    return "a Synder Tracker song, not an SNG-Player recording (not supported)";
  if (f.size() < 16 || std::memcmp(f.data(), "SYN", 3)) {
    if (f.size() >= 3 && !std::memcmp(f.data(), "CYN", 3))
      return "a CYN file (SID waveforms), which this player does not support";
    return "not a Synder SNG file (no SYN at the start)";
  }
  const uint8_t type = f[3];
  s.voices = type & 15;
  s.planar = !(type & 0x10);
  const bool packed = type & 0x40;
  if (type & 0xa0) return "unknown type byte " + std::to_string(type);
  if (s.voices < 1 || s.voices > 8) return "the file gives " + std::to_string(s.voices) + " voices (1-8 can be)";
  s.rate = int(le16(&f[4]));
  if (s.rate < 1 || s.rate > 1000) return "the frame rate " + std::to_string(s.rate) + " Hz is not one a song can have";
  s.loop = le32(&f[6]);
  const size_t textLen = le16(&f[10]);
  const size_t U = le32(&f[12]);
  if (16 + textLen > f.size()) return "the text runs past the end of the file";
  s.text.assign(reinterpret_cast<const char*>(&f[16]), textLen);
  const uint8_t* body = f.data() + 16 + textLen;
  const size_t bodyLen = f.size() - 16 - textLen;
  const size_t frameBytes = 6 * size_t(s.voices);
  if (U == 0) return "the song has no frames";
  if (U % frameBytes) return "the frame data's size is not a whole number of frames";
  if (U > MAX_UNPACKED) return "the frame data would be larger than 64 MB";
  s.frames = U / frameBytes;
  const size_t spf = size_t(SYNTH_RATE / s.rate);
  if ((long long)(s.frames) * (long long)spf > MAX_SECONDS * SYNTH_RATE)
    return "the song would last more than 30 minutes";
  if (packed) {
    if (U > MAX_RATIO * bodyLen + 4096) return "the frame data's size is far too large for the packed data";
    s.data.assign(U, 0);
    Depacker d(body, bodyLen);
    std::string why = d.run(s.data);
    if (!why.empty()) return "damaged: " + why;
  } else {
    if (bodyLen < U) return "the frame data is shorter than the size the file gives";
    s.data.assign(body, body + U);
  }
  return "";
}

// ---- the synthesizer

int8_t g_sine[512];
uint32_t g_inc[4096];
uint16_t g_shape[256][256];   // [row][shape value] -> waveform index
uint8_t g_wave[512];          // the waveform, as bytes 0..255
uint8_t g_noise[4096];

void makeTables() {
  for (int k = 0; k < 512; ++k)
    g_sine[k] = int8_t(std::nearbyintl(127.0L * std::sin(static_cast<long double>(2 * k) * 3.14159265358979323846264338327950288L / 511.0L)));
  for (int f = 0; f < 4096; ++f) g_inc[f] = uint32_t(uint64_t(f) * 131072 / 375);
  for (int x = 0; x < 256; ++x)
    for (int w = 0; w < 256; ++w)
      g_shape[x][w] = x >= w ? uint16_t((x - w) * 256 / (256 - w)) : uint16_t(256 + x * 256 / (w + 1));
  for (int x = 0; x < 256; ++x) {
    int a = g_sine[2 * x] * 8 / 16 + g_sine[(8 * x) % 512] * 8 / 16;
    int b = g_sine[2 * x] * 31 / 16 + g_sine[(6 * x) % 512] * 14 / 16;
    a = a < -128 ? -128 : a > 127 ? 127 : a;
    b = b < -128 ? -128 : b > 127 ? 127 : b;
    g_wave[x] = uint8_t(a + 128);
    g_wave[256 + x] = uint8_t(b + 128);
  }
  // MT19937, seeded 0x12345678; each entry is an output's top 8 bits
  uint32_t mt[624];
  mt[0] = 0x12345678u;
  for (int i = 1; i < 624; ++i) mt[i] = 1812433253u * (mt[i - 1] ^ (mt[i - 1] >> 30)) + uint32_t(i);
  int idx = 624;
  for (int n = 0; n < 4096; ++n) {
    if (idx >= 624) {
      for (int i = 0; i < 624; ++i) {
        uint32_t y = (mt[i] & 0x80000000u) | (mt[(i + 1) % 624] & 0x7fffffffu);
        mt[i] = mt[(i + 397) % 624] ^ (y >> 1) ^ ((y & 1) ? 0x9908b0dfu : 0);
      }
      idx = 0;
    }
    uint32_t y = mt[idx++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= y >> 18;
    g_noise[n] = uint8_t(y >> 24);
  }
}

// Each voice's state is a 24-byte record, as in the original program:
//   0 increment (4)  4 shape value  5 loudness  6 low-pass coefficient  7, 8: 255  9: 0
//   10 noise switch  11 filter  12 left shift  13 right shift  14 latched shape
//   15 previous position  16 last waveform byte  17 latched this sample  20 phase (4)
// With the original's fault the records of voices 4-7 are the noise table's first 96 bytes.
class Synth {
 public:
  Synth(const Song& s, bool clean) : song_(s), spf_(size_t(SYNTH_RATE / s.rate)) {
    std::memset(mem_, 0, sizeof(mem_));
    if (clean) {
      std::memcpy(noiseApart_, g_noise, sizeof(noiseApart_));
      noise_ = noiseApart_;
      for (int v = 0; v < 8; ++v) rec_[v] = mem_ + 24 * v;
    } else {
      std::memcpy(mem_ + 96, g_noise, sizeof(g_noise));
      noise_ = mem_ + 96;
      for (int v = 0; v < 8; ++v) rec_[v] = mem_ + 24 * v;   // voice 4's is noise_[0..23]
    }
  }

  size_t total() const { return song_.frames * spf_; }
  bool done() const { return n_ >= total(); }

  // One output sample (left, right); silence after the end
  void sample(int16_t* out) {
    if (done()) {
      out[0] = out[1] = 0;
      return;
    }
    if (n_ % spf_ == 0) frame(n_ / spf_);
    ++n_;
    int32_t left = 0, right = 0;
    for (int v = 0; v < song_.voices; ++v) {
      uint8_t* r = rec_[v];
      const uint32_t phase = le32(r + 20) + le32(r);
      put32(r + 20, phase);
      const uint8_t p = uint8_t(phase >> 16);
      r[17] = 0;
      if (r[15] >= 128 && p < 128) {
        r[14] = r[4];
        r[17] = 1;
      }
      r[15] = p;
      const uint8_t row = r[10] ? noise_[(phase >> 20) & 0xfff] : p;
      const uint8_t s = g_wave[g_shape[row][r[14]]];
      r[16] = s;
      const int d = int(s) - int(r[11]);
      r[11] = uint8_t(r[11] + ((d * (int(r[6]) + 1)) >> 8));
      const int32_t sig = (int32_t(r[11]) - 128) * int32_t(r[5]);
      left += sig >> (r[12] & 31);
      right += sig >> (r[13] & 31);
    }
    out[0] = int16_t(uint16_t(uint32_t(left)));
    out[1] = int16_t(uint16_t(uint32_t(right)));
  }

 private:
  static void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
  }

  void frame(size_t f) {
    static const int base[4] = {0, 1, 3, 16};
    static const int head[8] = {0, 1, 2, 2, 3, 3, 3, 3};
    const int V = song_.voices;
    for (int v = 0; v < V; ++v) {
      const uint8_t p0 = song_.at(v, f, 0), m = song_.at(v, f, 1), p2 = song_.at(v, f, 2),
                    p3 = song_.at(v, f, 3), p4 = song_.at(v, f, 4), p5 = song_.at(v, f, 5);
      int w = p2;
      if (p3 == 16) w = 0;
      if (p3 == 32) w = 128;
      if (p3 == 0) {
        if (w > 127) w = 255 - w;
        w = (w * 2 + 1) & 255;
      }
      const int freq = ((256 + m) * 8) >> (7 - (p0 & 7));
      int ls = base[p0 >> 6 & 3] + head[V - 1], rs = base[p0 >> 4 & 3] + head[V - 1];
      if (ls > 15) ls = 16;
      if (rs > 15) rs = 16;
      uint8_t* r = rec_[v];
      put32(r, g_inc[freq]);
      r[4] = uint8_t(w);
      r[5] = p4;
      r[6] = p5;
      r[7] = 255;
      r[8] = 255;
      r[9] = 0;
      r[10] = p0 >> 3 & 1;
      r[12] = uint8_t(ls);
      r[13] = uint8_t(rs);
    }
  }

  const Song& song_;
  size_t spf_, n_ = 0;
  uint8_t mem_[96 + 4096];     // voices 0-3, then the noise table (with the fault)
  uint8_t noiseApart_[4096];
  uint8_t* noise_;
  uint8_t* rec_[8];
};

// ---- the protocol

std::string latin1(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (c == 0) break;
    if (c < 0x20 || c == 0x7f) c = ' ';
    if (c < 0x80) out += char(c);
    else {
      out += char(0xc0 | c >> 6);
      out += char(0x80 | (c & 0x3f));
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string trim(std::string s) {
  while (!s.empty() && s.front() == ' ') s.erase(s.begin());
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

void json(const std::string& s) {
  std::putchar('"');
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') std::printf("\\%c", c);
    else if (c < 0x20 || c == 0x7f) std::printf("\\u%04x", c);
    else std::putchar(c);
  }
  std::putchar('"');
}

int fail(const std::string& why) {
  std::printf("{\"tracks\":0,\"error\":");
  json(why);
  std::printf("}\n");
  std::fflush(stdout);
  return 1;
}

bool readFile(const char* path, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  out.resize(MAX_FILE + 1);
  size_t n = std::fread(out.data(), 1, out.size(), f);
  std::fclose(f);
  out.resize(n);
  return true;
}

void source(void* user, int16_t* frame) { static_cast<Synth*>(user)->sample(frame); }

}  // namespace

int main(int argc, char** argv) {
  bool clean = false, ref = false, unpack = false;
  int a = 1;
  for (; a < argc && argv[a][0] == '-' && argv[a][1] == '-'; ++a) {
    if (!std::strcmp(argv[a], "--clean")) clean = true;
    else if (!std::strcmp(argv[a], "--ref")) ref = true;
    else if (!std::strcmp(argv[a], "--unpack")) unpack = true;
    else break;
  }
  if ((ref || unpack) ? argc - a < 1 : argc - a < 3) {
    std::fprintf(stderr, "usage: %s [--clean] <file> <track> <samplerate> [start-ms]\n"
                         "       %s [--clean] --ref <file>   (48000 Hz, no header)\n"
                         "       %s --unpack <file>\n", argv[0], argv[0], argv[0]);
    return 2;
  }
  const char* path = argv[a];
  std::vector<uint8_t> file;
  if (!readFile(path, file) || file.empty()) return fail("can't read the file");
  if (file.size() > MAX_FILE) return fail("the file is too big for a Synder SNG file");
  Song song;
  std::string why = load(file, song);
  if (!why.empty()) return fail(why);
  file.clear();
  makeTables();

  if (unpack) {
    std::fwrite(song.data.data(), 1, song.data.size(), stdout);
    return 0;
  }
  if (!ref && std::atoi(argv[a + 1]) != 0) return fail("a Synder SNG file has one track");
  int rate = ref ? SYNTH_RATE : std::atoi(argv[a + 2]);
  if (rate < 8000 || rate > 192000) rate = SYNTH_RATE;
  long long startMs = !ref && argc - a > 3 ? std::atoll(argv[a + 3]) : 0;
  if (startMs < 0) startMs = 0;

  Synth synth(song, clean);
  const size_t total = synth.total();

  if (!ref) {
    // The text is "[title - ]by author : released year publisher"
    const std::string text = latin1(song.text);
    std::string title, author, released;
    size_t by = text.compare(0, 3, "by ") == 0 ? 0 : text.find(" - by ");
    if (by != std::string::npos) {
      if (by > 0) title = trim(text.substr(0, by));
      const size_t from = text.find("by ", by) + 3;
      const size_t colon = text.find(" :", from);
      author = trim(text.substr(from, colon == std::string::npos ? std::string::npos : colon - from));
      if (colon != std::string::npos) {
        released = trim(text.substr(colon + 2));
        if (released.compare(0, 9, "released ") == 0) released = trim(released.substr(9));
      }
    }
    std::printf("{\"tracks\":1,\"track\":0,\"title\":");
    json(title);
    std::printf(",\"author\":");
    json(author);
    std::printf(",\"type\":\"Synder SNG\",\"duration_ms\":%lld,\"facts\":{",
                (long long)(total * 1000 / SYNTH_RATE));
    std::printf("\"Voices\":");
    json(std::to_string(song.voices) + (song.voices <= 3 ? " (one SID's)" : song.voices == 8 ? " (four SIDs')" : " (two SIDs')"));
    std::printf(",\"Frame rate\":");
    json(std::to_string(song.rate) + " Hz, " + std::to_string(song.frames) + " frames");
    std::printf(",\"Synthesizer\":");
    json(clean ? "DSynth's wavetable synthesizer, 48000 Hz, the voices kept apart"
               : "DSynth's wavetable synthesizer, 48000 Hz, as the SNG-Player plays it");
    if (!released.empty()) {
      std::printf(",\"Released\":");
      json(released);
    }
    std::printf("},\"comment\":");
    json(text);
    std::printf(",\"names\":[]}\n");
    std::fflush(stdout);
  }

  const size_t skip = size_t(startMs) >= total * 1000 / SYNTH_RATE ? total : size_t(startMs) * (SYNTH_RATE / 1000);
  int16_t tmp[2];
  for (size_t i = 0; i < skip; ++i) synth.sample(tmp);

  std::vector<int16_t> buf(4096 * 2);
  if (rate == SYNTH_RATE) {
    while (!synth.done()) {
      size_t n = 0;
      while (n < 4096 && !synth.done()) synth.sample(&buf[2 * n++]);
      if (std::fwrite(buf.data(), 4, n, stdout) != n) return 0;
    }
  } else {
    Resampler rs(SYNTH_RATE, rate, source, &synth);
    long long left = (long long)((total - skip) * (unsigned long long)rate / SYNTH_RATE);
    while (left > 0) {
      size_t n = left > 4096 ? 4096 : size_t(left);
      for (size_t i = 0; i < n; ++i) rs.frame(&buf[2 * i]);
      if (std::fwrite(buf.data(), 4, n, stdout) != n) return 0;
      left -= (long long)n;
    }
  }
  std::fflush(stdout);
  return 0;
}
