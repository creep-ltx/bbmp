// SPDX-License-Identifier: MIT
// bbmp-rv: plays a file with a Retrovert playback plugin, or an iXalance song with
// its own built-in engine.
//
// Retrovert (https://github.com/RetrovertApp) has its replayers as plugins behind one
// C interface. This is a small host for that interface which speaks the protocol of
// the player's other helpers: one line of JSON about the song, then s16le stereo.
//
//   bbmp-rv <plugin.so> <file> <track> <samplerate> [start-ms]
//
// A plugin that crashes takes this process with it and nothing else: the player
// sees a song that ended.
//
// iXalance songs (.ixs) are played by ixs_engine.cpp, built in: a first argument of
// "ixalance" or any path whose file name is "ixalance_playback.so" (the plugin it
// replaces; no such file is needed) selects it. Its environment:
//   BBMP_IXS_DATA=<folder>  the three library samples a few songs use (909OH.WAV,
//                           909SD.WAV, CRASH.WAV); songs that need them are refused
//                           without them
//   BBMP_IXS_DUMP=<prefix>  writes <prefix>.eng.raw (the engine's 44100 Hz output),
//                           <prefix>.samples (the made samples) and <prefix>.ticks
//
// This file is under the MIT licence (csrc/retrovert/LICENSE), and so is everything
// built into the program: nothing under the GPL. README.txt says where each part
// comes from.

#include "ixs_engine.h"

#include <retrovert/io.h>
#include <retrovert/log.h>
#include <retrovert/metadata.h>
#include <retrovert/playback.h>
#include <retrovert/service.h>
#include <retrovert/settings.h>

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Info {
  std::string title, author, type, program, comment;
  double seconds = 0;
  int tracks = 0;
  std::vector<std::string> names;
} g_info;

std::string json(const std::string& s) {
  std::string out = "\"";
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
    else out += c;
  }
  return out + "\"";
}

// Plugins hand over text in whatever the file has. What isn't UTF-8 is read as Latin-1.
std::string utf8(const char* text) {
  const std::string s = text ? text : "";
  bool ok = true;
  for (size_t i = 0; i < s.size() && ok;) {
    const unsigned char c = s[i];
    const int more = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
    if (more < 0 || i + more >= s.size() + (more ? 0 : 1)) { ok = false; break; }
    for (int k = 1; k <= more; ++k) ok = ok && (static_cast<unsigned char>(s[i + k]) >> 6) == 2;
    i += more + 1;
  }
  if (ok) return s;
  std::string out;
  for (unsigned char c : s) {
    if (c < 0x80) out += c;
    else { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
  }
  return out;
}

int fail(const std::string& why) {
  std::printf("{\"tracks\":0,\"error\":%s}\n", json(why).c_str());
  std::fflush(stdout);
  return 1;
}

// ---- what the plugin may ask of its host ----

bool io_exists(RVIoPrivate*, const char* url) { return access(url, R_OK) == 0; }

RVIoReadUrlResult io_read(RVIoPrivate*, const char* url) {
  RVIoReadUrlResult none = {nullptr, 0};
  FILE* f = std::fopen(url, "rb");
  if (!f) return none;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 0) { std::fclose(f); return none; }
  uint8_t* data = static_cast<uint8_t*>(std::malloc(size ? size : 1));
  const size_t got = data ? std::fread(data, 1, size, f) : 0;
  std::fclose(f);
  if (!data || got != static_cast<size_t>(size)) { std::free(data); return none; }
  return {data, static_cast<uint64_t>(size)};
}

void io_free(RVIoPrivate*, void* memory) { std::free(memory); }

void log_write(RVLogPrivate*, uint32_t level, const char*, int, const char* fmt, ...) {
  if (level < RVLogLevel_Warn && !std::getenv("BBMP_RV_VERBOSE")) return;
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fputc('\n', stderr);
}

RVMetadataId meta_create(RVMetadataPrivate*, const char*) { return 1; }

void meta_tag(RVMetadataPrivate*, RVMetadataId, const char* tag, const char* data) {
  const std::string t = tag ? tag : "", v = utf8(data);
  if (t == RV_METADATA_TITLE_TAG) g_info.title = v;
  else if (t == RV_METADATA_ARTIST_TAG) g_info.author = v;
  else if (t == RV_METADATA_SONGTYPE_TAG) g_info.type = v;
  else if (t == RV_METADATA_AUTHORINGTOOL_TAG) g_info.program = v;
  else if (t == RV_METADATA_MESSAGE_TAG) g_info.comment = v;
}

void meta_tag_f64(RVMetadataPrivate*, RVMetadataId, const char* tag, double data) {
  if (tag && std::string(tag) == RV_METADATA_LENGTH_TAG && data > 0) g_info.seconds = data;
}

void meta_subsong(RVMetadataPrivate*, RVMetadataId, uint32_t, const char*, float) { ++g_info.tracks; }

void meta_name(RVMetadataPrivate*, RVMetadataId, const char* text) { g_info.names.push_back(utf8(text)); }

// No settings are kept: every question is answered with "not set", and the plugin
// uses what it would use without a host that has settings.
RVSettingsResult set_reg(RVSettingsPrivate*, const char*, RVSetting*, uint64_t) { return RVSettingsResult(0); }
RVSStringResult set_string(RVSettingsPrivate*, const char*, const char*, const char*) { return {RVSettingsResult(1), nullptr}; }
RVSIntResult set_int(RVSettingsPrivate*, const char*, const char*, const char*) { return {RVSettingsResult(1), 0}; }
RVSFloatResult set_float(RVSettingsPrivate*, const char*, const char*, const char*) { return {RVSettingsResult(1), 0.0f}; }
RVSBoolResult set_bool(RVSettingsPrivate*, const char*, const char*, const char*) { return {RVSettingsResult(1), false}; }

const RVIo g_io = {nullptr, io_exists, io_read, io_free};
const RVLog g_log = {nullptr, log_write};
const RVMetadata g_meta = {nullptr, meta_create, meta_tag, meta_tag_f64, meta_subsong, meta_name, meta_name};
const RVSettings g_settings = {nullptr, set_reg, set_string, set_int, set_float, set_bool};

const RVIo* get_io(RVServicePrivData*, int) { return &g_io; }
const RVLog* get_log(RVServicePrivData*, int) { return &g_log; }
const RVMetadata* get_metadata(RVServicePrivData*, int) { return &g_meta; }
const RVSettings* get_settings(RVServicePrivData*, int) { return &g_settings; }

const RVService g_service = {nullptr, get_io, get_log, get_metadata, get_settings};

// ---- audio: whatever the plugin makes, as s16 stereo at the rate that was asked for ----

float sample(const uint8_t* p, RVAudioStreamFormat format) {
  switch (format) {
    case RVAudioStreamFormat_U8: return (static_cast<int>(*p) - 128) / 128.0f;
    case RVAudioStreamFormat_S16: { int16_t v; std::memcpy(&v, p, 2); return v / 32768.0f; }
    case RVAudioStreamFormat_S24: {
      const int32_t v = (p[0] | (p[1] << 8) | (static_cast<int8_t>(p[2]) * 65536));
      return v / 8388608.0f;
    }
    case RVAudioStreamFormat_S32: { int32_t v; std::memcpy(&v, p, 4); return static_cast<float>(v / 2147483648.0); }
    case RVAudioStreamFormat_F32: { float v; std::memcpy(&v, p, 4); return v; }
  }
  return 0;
}

int width(RVAudioStreamFormat format) {
  switch (format) {
    case RVAudioStreamFormat_U8: return 1;
    case RVAudioStreamFormat_S16: return 2;
    case RVAudioStreamFormat_S24: return 3;
    default: return 4;
  }
}

class Output {
 public:
  Output(int rate, FILE* to) : rate_(rate), to_(to) {}

  // Frames to leave out at the start, for starting at a position the plugin can't seek to.
  void skip(long long frames) { skip_ = frames; }

  bool write(const uint8_t* data, uint32_t frames, const RVAudioFormat& f) {
    const int channels = std::max<uint32_t>(1, f.channel_count), bytes = width(f.audio_format);
    for (uint32_t i = 0; i < frames; ++i) {
      const uint8_t* p = data + static_cast<size_t>(i) * channels * bytes;
      const float l = sample(p, f.audio_format);
      const float r = channels > 1 ? sample(p + bytes, f.audio_format) : l;
      if (!push(l, r, f.sample_rate ? f.sample_rate : rate_)) return false;
    }
    return flush();
  }

 private:
  // Linear interpolation between the plugin's frames when its rate is another one.
  bool push(float l, float r, uint32_t from) {
    if (static_cast<int>(from) == rate_) return put(l, r);
    const double step = static_cast<double>(from) / rate_;
    while (pos_ < 1.0) {
      if (!put(prev_l_ + (l - prev_l_) * pos_, prev_r_ + (r - prev_r_) * pos_)) return false;
      pos_ += step;
    }
    pos_ -= 1.0;
    prev_l_ = l;
    prev_r_ = r;
    return true;
  }

  bool put(double l, double r) {
    if (skip_ > 0) { --skip_; return true; }
    out_.push_back(clip(l));
    out_.push_back(clip(r));
    return out_.size() < 16384 || flush();
  }

  static int16_t clip(double v) {
    const long s = std::lround(v * 32767.0);
    return static_cast<int16_t>(std::max(-32768L, std::min(32767L, s)));
  }

  bool flush() {
    const size_t n = out_.size();
    const bool ok = n == 0 || (std::fwrite(out_.data(), sizeof(int16_t), n, to_) == n
                               && std::fflush(to_) == 0);
    out_.clear();
    return ok;
  }

  int rate_;
  FILE* to_;
  long long skip_ = 0;
  double pos_ = 0;
  float prev_l_ = 0, prev_r_ = 0;
  std::vector<int16_t> out_;
};

// The built-in iXalance engine, with the same output path a plugin's frames take.
template <class Restore>
int play_ixs(const char* path, int track, int rate, long long start_ms, int real, Restore restore) {
  std::vector<uint8_t> file;
  if (FILE* f = std::fopen(path, "rb")) {
    uint8_t buf[65536];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) {
      file.insert(file.end(), buf, buf + got);
      if (file.size() > (64u << 20)) break;    // no song is near this
    }
    std::fclose(f);
  } else {
    restore();
    return fail("can't read the file");
  }
  const char* dump = std::getenv("BBMP_IXS_DUMP");
  const char* data = std::getenv("BBMP_IXS_DATA");
  ixs::Options options;
  if (data) options.library_dir = data;
  FILE* ticks = nullptr;
  if (dump && *dump) ticks = options.ticks = std::fopen((std::string(dump) + ".ticks").c_str(), "wb");
  std::string error;
  std::unique_ptr<ixs::Player> player = ixs::open(file, options, error);
  if (!player) {
    restore();
    return fail(error);
  }
  if (dump && *dump) {
    if (FILE* f = std::fopen((std::string(dump) + ".samples").c_str(), "wb")) {
      player->write_samples(f);
      std::fclose(f);
    }
  }
  FILE* raw = dump && *dump ? std::fopen((std::string(dump) + ".eng.raw").c_str(), "wb") : nullptr;

  restore();
  std::printf("{\"tracks\":1,\"track\":%d,\"title\":%s,\"author\":\"\",\"type\":\"IXS\",\"program\":\"\","
              "\"duration_ms\":0,\"comment\":\"\",\"names\":[]}\n",
              std::min(track, 0), json(utf8(ixs::title(file).c_str())).c_str());
  std::fflush(stdout);
  dup2(STDERR_FILENO, STDOUT_FILENO);

  FILE* audio = fdopen(real, "wb");
  if (!audio) _exit(1);
  Output out(rate, audio);
  if (start_ms > 0) out.skip(start_ms * rate / 1000);
  const RVAudioFormat format = {RVAudioStreamFormat_S16, 2, static_cast<uint32_t>(ixs::kRate)};
  const double limit = 20 * 60;     // as for a plugin that states no length
  double played = 0;
  std::vector<int16_t> block;
  bool ok = true;
  while (ok && played < limit && player->next(block)) {
    const uint32_t made = static_cast<uint32_t>(block.size() / 2);
    if (raw) std::fwrite(block.data(), sizeof(int16_t), block.size(), raw);
    uint32_t frames = made;
    const double left = (limit - played) * ixs::kRate;
    if (frames > left) frames = static_cast<uint32_t>(left > 0 ? left : 0);
    played += static_cast<double>(made) / ixs::kRate;
    ok = out.write(reinterpret_cast<const uint8_t*>(block.data()), frames, format);
  }
  if (raw) std::fclose(raw);
  if (ticks) std::fclose(ticks);
  std::fflush(audio);
  _exit(0);
}

bool is_ixs_engine(const char* arg) {
  const std::string a = arg;
  const size_t slash = a.rfind('/');
  const std::string base = slash == std::string::npos ? a : a.substr(slash + 1);
  return base == "ixalance" || base == "ixalance_playback.so";
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: %s <plugin.so> <file> <track> <samplerate> [start-ms]\n", argv[0]);
    return 2;
  }
  const char* path = argv[2];
  const int track = std::atoi(argv[3]);
  const int rate = std::atoi(argv[4]);
  const long long start_ms = argc > 5 ? std::atoll(argv[5]) : 0;
  if (rate < 8000 || rate > 192000) return fail("unsupported sample rate");

  // What a plugin prints must not end up in the audio: its stdout becomes stderr, and
  // the real stdout is kept for the header and the samples.
  std::fflush(stdout);
  const int real = dup(STDOUT_FILENO);
  dup2(STDERR_FILENO, STDOUT_FILENO);
  auto restore = [real] { std::fflush(stdout); dup2(real, STDOUT_FILENO); };
  if (is_ixs_engine(argv[1])) return play_ixs(path, track, rate, start_ms, real, restore);

  void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!lib) { restore(); return fail(std::string("can't load the plugin: ") + dlerror()); }
  using Entry = RVPlaybackPlugin* (*)();
  Entry entry = reinterpret_cast<Entry>(dlsym(lib, "rv_playback_plugin"));
  RVPlaybackPlugin* plugin = entry ? entry() : nullptr;
  if (!plugin) { restore(); return fail("not a Retrovert playback plugin"); }
  if (plugin->api_version != RV_PLAYBACK_PLUGIN_API_VERSION) {
    restore();
    return fail("the plugin was built for another version of Retrovert's interface");
  }
  if (plugin->static_init) plugin->static_init(&g_service);

  // Ask the plugin itself before opening: some open anything and play noise.
  std::vector<uint8_t> head(4096);
  uint64_t total = 0;
  if (FILE* f = std::fopen(path, "rb")) {
    head.resize(std::fread(head.data(), 1, head.size(), f));
    std::fseek(f, 0, SEEK_END);
    total = static_cast<uint64_t>(std::max(0L, std::ftell(f)));
    std::fclose(f);
  } else {
    restore();
    return fail("can't read the file");
  }
  if (plugin->probe_can_play &&
      plugin->probe_can_play(head.data(), head.size(), path, total) == RVProbeResult_Unsupported) {
    restore();
    return fail(std::string(plugin->name ? plugin->name : "the plugin") + " doesn't know this file");
  }

  if (plugin->metadata) plugin->metadata(path, &g_service);
  void* instance = plugin->create ? plugin->create(&g_service) : nullptr;
  if (!instance) { restore(); return fail("the plugin couldn't start"); }
  if (plugin->open(instance, path, static_cast<uint32_t>(std::max(0, track)), &g_service) < 0) {
    restore();
    return fail(std::string(plugin->name ? plugin->name : "the plugin") + " can't play this file");
  }

  // A plugin's seek says where it got to (iXalance's stays at 0): the rest of the way
  // is played and left out.
  long long reached = 0;
  if (start_ms > 0 && plugin->seek) {
    const int64_t at = plugin->seek(instance, start_ms);
    if (at > 0) reached = std::min<long long>(at, start_ms);
  }

  restore();
  const int tracks = std::max(1, g_info.tracks);
  std::string names;
  for (const auto& n : g_info.names) names += (names.empty() ? "" : ",") + json(n);
  std::printf("{\"tracks\":%d,\"track\":%d,\"title\":%s,\"author\":%s,\"type\":%s,\"program\":%s,"
              "\"duration_ms\":%lld,\"comment\":%s,\"names\":[%s]}\n",
              tracks, std::min(track, tracks - 1), json(g_info.title).c_str(),
              json(g_info.author).c_str(), json(g_info.type).c_str(), json(g_info.program).c_str(),
              static_cast<long long>(g_info.seconds * 1000), json(g_info.comment).c_str(),
              names.c_str());
  std::fflush(stdout);
  dup2(STDERR_FILENO, STDOUT_FILENO);      // and away again while the plugin works

  FILE* audio = fdopen(real, "wb");
  if (!audio) _exit(1);
  Output out(rate, audio);
  if (start_ms > reached) out.skip((start_ms - reached) * rate / 1000);
  std::vector<uint8_t> buffer(4096 * 8 * 4);
  int waiting = 0;
  bool ok = true;
  // Some plugins say how long a song is and then play on for ever. A song is over
  // when its stated length has been played; one without a stated length after
  // twenty minutes, which is there to end what would never end by itself.
  const double limit = g_info.seconds > 0 ? g_info.seconds : 20 * 60;
  double played = reached / 1000.0;    // the length counts from the start of the song
  while (ok && played < limit) {
    RVReadData dest;
    dest.channels_output = buffer.data();
    dest.channels_output_max_bytes_size = static_cast<uint32_t>(buffer.size());
    dest.info.format = {RVAudioStreamFormat_S16, 2, static_cast<uint32_t>(rate)};
    dest.info.frame_count = 4096;
    dest.info.status = RVReadStatus_Ok;
    const RVReadInfo got = plugin->read_data(instance, dest);
    if (got.status == RVReadStatus_Error) break;
    if (got.frame_count == 0 && got.status != RVReadStatus_Finished) {
      if (++waiting > 2000) break;          // a plugin that never gets ready
      usleep(1000);
      continue;
    }
    waiting = 0;
    uint32_t frames = got.frame_count;
    if (got.format.sample_rate) {
      const double left = (limit - played) * got.format.sample_rate;
      if (frames > left) frames = static_cast<uint32_t>(left > 0 ? left : 0);
      played += static_cast<double>(got.frame_count) / got.format.sample_rate;
    }
    ok = out.write(buffer.data(), frames, got.format);
    if (got.status == RVReadStatus_Finished) break;
  }
  // The process ends here. A plugin's own teardown is skipped on purpose: nothing of
  // it is needed any more, and some take their time over it.
  std::fflush(audio);
  _exit(0);
}
