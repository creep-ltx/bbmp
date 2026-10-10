// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-chip: play one track with a small chip-music library, as raw PCM on stdout.
//
//   bbmp-chip <engine> <file> <track> <samplerate> [start-ms]
//
// engine: ym (StSound), adplug (AdPlug + Nuked OPL3),
//         mdx (mdxmini), pmdc (PMD: own driver on ymfm), sc68 (sc68: the replay routines
//         it doesn't come with in BBMP_SC68_REPLAYS),
//         fmp (98fmplayer),
//         kss (libkss: KSS, MGS, BGM, MBM, OPX, MPK; BGM, OPX and MPK need the drivers in
//         BBMP_MSX_DRIVERS), eup (EUPPlayer: FM Towns),
//         organya (organya.h), pxtone (libpxtone: Piston Collage), qsf (Highly Quixotic)
//
// (SNDH and SNSF were played here until 7 October 2026: psgplay is GPL 2 only and
// snsf9x is Snes9x's non-commercial code, neither of which may be combined with this
// GPL 3 program. They are helpers of their own now, bbmp-psgplay and bbmp-snsf.)
//
// Same protocol as bbmp-zxtune: one JSON line describing the track, then
// signed 16-bit little-endian stereo until the track ends.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iconv.h>
#include <memory>
#include <string>
#include <vector>

#include "StSoundLibrary.h"

extern "C" {
#include "mdxmini.h"
void mdx_make_buffer(t_mdxmini* data, short* buf, int buffer_size);  // not in its header
}
extern "C" {
#include "api68/api68.h"
}
#include "fmp_engine.h"
#include "pmdc_engine.h"
#include "qsf_engine.h"
extern "C" {
#include "kss/kss.h"
#include "kssplay.h"
int KSS_autoload_mbk(const char *mbmfile, const char *extra_path, const char *dummy_file);
}
#include "organya.h"
#include "pxtnService.h"
#include "eupplayer.h"
#include "eupplayer_townsEmulator.h"
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <climits>
#include <cstdlib>
#ifdef __APPLE__
#include <mach-o/dyld.h>   // _NSGetExecutablePath
#endif
#include <unistd.h>
#include <climits>
#include <samplerate.h>

#include <adplug/adplug.h>
#include <adplug/nemuopl.h>

namespace {

struct Info {
  int tracks = 1;
  int track = 0;
  std::string title, author, type;
  long long durationMs = 0;
  // what else the file says about itself, for the Info tab
  std::vector<std::pair<std::string, std::string>> facts;
  std::string comment;
  std::vector<std::string> names;   // instrument names
};

std::string json(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

// Japanese PC music titles are Shift-JIS.
std::string fromSjis(const char* s) {
  iconv_t cd = iconv_open("UTF-8//IGNORE", "CP932");
  if (cd == (iconv_t)-1) return s;
  std::string in(s), out(in.size() * 3 + 4, '\0');
  char* ip = in.data();
  char* op = out.data();
  size_t il = in.size(), ol = out.size();
  const bool ok = iconv(cd, &ip, &il, &op, &ol) != (size_t)-1;
  iconv_close(cd);
  if (!ok) return s;
  out.resize(out.size() - ol);
  return out;
}

void header(const Info& i) {
  std::string facts, names;
  for (const auto& f : i.facts)
    if (!f.second.empty()) facts += (facts.empty() ? "" : ",") + json(f.first) + ":" + json(f.second);
  for (const auto& n : i.names) names += (names.empty() ? "" : ",") + json(n);
  std::printf("{\"tracks\":%d,\"track\":%d,\"title\":%s,\"author\":%s,\"type\":%s,\"duration_ms\":%lld,"
              "\"facts\":{%s},\"comment\":%s,\"names\":[%s]}\n",
              i.tracks, i.track, json(i.title).c_str(), json(i.author).c_str(), json(i.type).c_str(),
              i.durationMs, facts.c_str(), json(i.comment).c_str(), names.c_str());
  std::fflush(stdout);
}

int fail(const std::string& why) {
  std::printf("{\"tracks\":0,\"error\":%s}\n", json(why).c_str());
  return 1;
}

bool emit(const int16_t* stereo, size_t frames) {
  return std::fwrite(stereo, 4, frames, stdout) == frames;
}

std::vector<char> readFile(const char* path) {
  std::vector<char> data;
  if (FILE* f = std::fopen(path, "rb")) {
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
  }
  return data;
}

std::string dirOf(const std::string& path) {
  const auto slash = path.rfind('/');
  return slash == std::string::npos ? "./" : path.substr(0, slash + 1);
}

// Skip `ms` of audio by rendering and discarding it (these engines can't seek).
template <class Render>
bool skip(long long ms, int rate, Render render) {
  long long frames = ms * rate / 1000;
  std::vector<int16_t> buf(2 * 4096);
  while (frames > 0) {
    const int n = frames > 4096 ? 4096 : static_cast<int>(frames);
    if (!render(buf.data(), n)) return false;
    frames -= n;
  }
  return true;
}

int playYm(const char* path, int, int rate, long long startMs) {
  YMMUSIC* ym = ymMusicCreateWithRate(rate);
  if (!ymMusicLoad(ym, path)) {
    std::string err = ymMusicGetLastError(ym);
    ymMusicDestroy(ym);
    return fail(err);
  }
  ymMusicInfo_t mi;
  ymMusicGetInfo(ym, &mi);
  Info info;
  info.title = mi.pSongName ? mi.pSongName : "";
  info.author = mi.pSongAuthor ? mi.pSongAuthor : "";
  info.type = mi.pSongType ? mi.pSongType : "YM";
  info.comment = mi.pSongComment ? mi.pSongComment : "";
  info.facts.push_back({"Made with", mi.pSongPlayer ? mi.pSongPlayer : ""});
  info.durationMs = mi.musicTimeInMs;
  ymMusicSetLoopMode(ym, YMFALSE);
  ymMusicPlay(ym);
  if (startMs && ymMusicIsSeekable(ym)) ymMusicSeek(ym, static_cast<ymu32>(startMs));
  header(info);
  std::vector<ymsample> mono(4096);
  std::vector<int16_t> stereo(2 * 4096);
  while (ymMusicCompute(ym, mono.data(), 4096) && !ymMusicIsOver(ym)) {
    for (int i = 0; i < 4096; i++) stereo[2 * i] = stereo[2 * i + 1] = mono[i];
    if (!emit(stereo.data(), 4096)) break;
  }
  ymMusicDestroy(ym);
  return 0;
}

int playAdplug(const char* path, int track, int rate, long long startMs) {
  CNemuopl opl(rate);
  std::unique_ptr<CPlayer> player(CAdPlug::factory(path, &opl));
  if (!player) return fail("AdPlug doesn't know this file");
  // A tick rate that isn't a number above zero (a Loudness file with speed 0 has
  // 1193182 / 0) would make songlength() below, and the playing, go on for ever.
  if (!(player->getrefresh() > 0) || !std::isfinite(player->getrefresh()))
    return fail("AdPlug: the file has no sensible tempo");
  Info info;
  info.tracks = player->getsubsongs() > 0 ? player->getsubsongs() : 1;
  info.track = track;
  info.type = player->gettype();
  info.title = player->gettitle();
  info.author = player->getauthor();
  info.comment = player->getdesc();
  for (unsigned i = 0; i < player->getinstruments() && i < 256; i++)
    info.names.push_back(player->getinstrument(i));
  info.durationMs = player->songlength(track);
  player->rewind(track);
  std::vector<int16_t> stereo(2 * 8192);
  // AdPlug advances in ticks of 1/getrefresh() seconds
  double pending = 0;
  bool playing = true;
  auto render = [&](int16_t* out, int frames) {
    int done = 0;
    while (done < frames) {
      if (pending < 1) {
        if (!playing) return false;
        playing = player->update();
        const float refresh = player->getrefresh();
        if (!(refresh > 0) || !std::isfinite(refresh)) return false;  // see above
        pending += rate / refresh;
      }
      int n = static_cast<int>(pending);
      if (n > frames - done) n = frames - done;
      opl.update(out + 2 * done, n);
      pending -= n;
      done += n;
    }
    return true;
  };
  if (startMs) skip(startMs, rate, render);
  header(info);
  while (render(stereo.data(), 4096) && emit(stereo.data(), 4096)) {
  }
  return 0;
}

int playMdx(const char* path, int, int rate, long long startMs) {
  mdx_set_rate(rate);
  t_mdxmini mdx;
  std::memset(&mdx, 0, sizeof(mdx));
  std::string dir = dirOf(path);
  if (mdx_open(&mdx, const_cast<char*>(path), dir.data()) != 0) return fail("mdxmini can't open file");
  mdx_set_max_loop(&mdx, 1);   // once through, then mdxmini's own fade into the repeat
  if (mdx.channels != 2) {
    mdx_close(&mdx);
    return fail("mdxmini doesn't make stereo");
  }
  Info info;
  info.type = "MDX";
  char title[1024] = "";
  mdx_get_title(&mdx, title);
  info.title = fromSjis(title);
  info.durationMs = mdx_get_length(&mdx) * 1000LL;
  // mdxmini's frame loop (mdx_calc_sample) is done here, as it does it but for three
  // faults: it renders a frame (1/Timer-B, up to 65 ms) in one piece, where PCM8's
  // buffers hold 1024 samples (a slow song writes past them); it notes the song's
  // end only if that frame was the last of the call (otherwise the song goes on at
  // "volume -1, -2 ..." for ever); and 48000 * 65280 us overflows its int.
  // (The block in which the song ends is left out, as before: it is the end of the fade.)
  int left = 0;
  auto render = [&](int16_t* out, int frames) {
    for (int done = 0; done < frames;) {
      if (left == 0) {
        if (!mdx_next_frame(&mdx)) return false;
        left = static_cast<int>(static_cast<long long>(mdx.mdx->dsp_speed) * mdx_frame_length(&mdx) / 1000000);
        continue;
      }
      const int n = std::min(std::min(left, frames - done), 1024);
      mdx_make_buffer(&mdx, out + 2 * done, n);
      left -= n;
      done += n;
    }
    return true;
  };
  if (startMs) skip(startMs, rate, render);
  header(info);
  std::vector<int16_t> stereo(2 * 4096);
  while (render(stereo.data(), 4096) && emit(stereo.data(), 4096)) {
  }
  mdx_close(&mdx);
  return 0;
}

// PMD through the player's own driver (pmdc_driver.cpp, written from a description of
// PMD's behaviour) on ymfm's YM2608. The chip's drums need its rhythm ROM, which the
// user supplies (BBMP_YM2608_ROM). BBMP_PMDC_LOG=<file> writes the song's register log
// instead of playing it (for checking the driver against the old player's writes).
int playPmdc(const char* path, int, int rate, long long startMs) {
  const std::vector<char> raw = readFile(path);
  if (raw.empty()) return fail("can't read the file");
  if (raw.size() > 65536) return fail("not a PMD song (larger than 64 KB)");
  const std::vector<uint8_t> file(raw.begin(), raw.end());
  if (const char* log = std::getenv("BBMP_PMDC_LOG")) {
    const std::string err = pmdc::writeLog(file, log);
    return err.empty() ? 0 : fail(err);
  }
  std::vector<uint8_t> rom;
  if (const char* romPath = std::getenv("BBMP_YM2608_ROM")) {
    const std::vector<char> r = readFile(romPath);
    if (r.size() == 8192) rom.assign(r.begin(), r.end());
  }
  pmdc::Player player;
  pmdc::SongInfo si;
  const std::string err = player.open(file, rate, rom, si);
  if (!err.empty()) return fail(err);
  Info info;
  info.type = "PMD";
  info.title = fromSjis(si.title.c_str());
  info.author = fromSjis(si.composer.c_str());
  for (const auto& f : si.facts) info.facts.emplace_back(f.first, fromSjis(f.second.c_str()));
  info.comment = si.comment;
  info.durationMs = si.totalFrames * 1000 / rate;
  auto render = [&](int16_t* out, int frames) { return player.render(out, frames) == frames; };
  if (startMs) skip(startMs, rate, render);
  header(info);
  std::vector<int16_t> stereo(2 * 4096);
  for (;;) {
    const long n = player.render(stereo.data(), 4096);
    if (n <= 0 || !emit(stereo.data(), static_cast<size_t>(n)) || n < 4096) break;
  }
  return 0;
}

// The folder <up> levels above this program (1: its bin folder's parent), "" when it
// can't be told: Linux says where it is in /proc, macOS through _NSGetExecutablePath.
std::string aboveExe(int up) {
  char buf[PATH_MAX];
  std::string p;
#ifdef __APPLE__
  uint32_t size = sizeof(buf);
  char real[PATH_MAX];
  if (_NSGetExecutablePath(buf, &size) != 0 || !realpath(buf, real)) return "";
  p = real;
#else
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return "";
  p.assign(buf, n);
#endif
  for (int i = 0; i <= up && p.rfind('/') != std::string::npos; ++i) p.erase(p.rfind('/'));
  return p;
}

// sc68's replay routines: where the build put them, or beside this helper
// (<opt>/bbmp-chip/bin -> <opt>/sc68/share/sc68) when the whole folder was moved.
std::string sc68Share() {
  struct stat st;
  if (stat(SC68_SHARE, &st) == 0) return SC68_SHARE;
  const std::string opt = aboveExe(2);
  return opt.empty() ? SC68_SHARE : opt + "/sc68/share/sc68";
}

// Organya's sound bank (default.wdb, Studio Pixel's samples, with no terms that let it be
// passed on): the player downloads it ("organyabank") and names it in BBMP_ORGANYA_BANK.
// Empty when it isn't there.
std::string organyaSoundbank() {
  struct stat st;
  const char* bank = std::getenv("BBMP_ORGANYA_BANK");
  if (bank && *bank && stat(bank, &st) == 0 && S_ISREG(st.st_mode)) return bank;
  return "";
}

// Of sc68's replay routines only its own six stubs come with the player (build-chip.sh);
// the others, ripped from games, editors and demos, are downloaded on request into a
// folder that BBMP_SC68_REPLAYS names (Replay/<name>.bin there). sc68 looks in its user
// folder before its shared one, so that folder is given as the user folder.
bool sc68HasReplay(const std::string& share, const char* replays, const char* name) {
  std::string file = name;
  for (char& c : file) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  file = "/Replay/" + file + ".bin";
  struct stat st;
  return stat((share + file).c_str(), &st) == 0 ||
         (replays && *replays && stat((std::string(replays) + file).c_str(), &st) == 0);
}

int playSc68(const char* path, int track, int rate, long long startMs) {
  api68_init_t init;
  std::memset(&init, 0, sizeof(init));
  init.sampling_rate = rate;
  init.alloc = [](unsigned int n) { return std::malloc(n); };
  init.free = std::free;
  const std::string share = sc68Share();
  init.shared_path = share.c_str();  // external replay routines some tunes need
  const char* replays = std::getenv("BBMP_SC68_REPLAYS");
  if (replays && *replays) init.user_path = replays;
  api68_t* api = api68_init(&init);
  if (!api) return fail("sc68 failed to start");
  if (api68_load_file(api, path) != 0) {
    api68_shutdown(api);
    return fail("sc68 can't load this file");
  }
  Info info;
  info.type = "SC68";
  info.tracks = api68_tracks(api) > 0 ? api68_tracks(api) : 1;
  info.track = track;
  api68_music_info_t mi;
  if (api68_music_info(api, &mi, track + 1, nullptr) == 0) {
    info.title = mi.title ? mi.title : "";
    info.author = mi.author ? mi.author : "";
    if (mi.replay) info.type = std::string("SC68 ") + mi.replay;
    info.durationMs = mi.time_ms;
    if (mi.replay && std::strcmp(mi.replay, "built-in") != 0 &&
        !sc68HasReplay(share, replays, mi.replay)) {
      const std::string name = mi.replay;
      api68_shutdown(api);
      return fail("This song needs sc68's replay routine " + name + ", which is not here");
    }
  }
  api68_sampling_rate(api, rate);
  if (api68_play(api, track + 1) != 0) {
    api68_shutdown(api);
    return fail("sc68 can't play this track");
  }
  bool started = false;
  auto render = [&](int16_t* out, int frames) {
    const int code = api68_process(api, out, frames);
    if (code == API68_MIX_ERROR || (code & API68_END)) return false;
    // sc68 moves on to the next track by itself; stop when that happens
    if ((code & API68_CHANGE) && started) return false;
    started = true;
    return true;
  };
  if (startMs) skip(startMs, rate, render);
  header(info);
  std::vector<int16_t> stereo(2 * 4096);
  while (render(stereo.data(), 4096) && emit(stereo.data(), 4096)) {
  }
  api68_shutdown(api);
  return 0;
}

// PC-98 FMP (and PMD) through 98fmplayer's driver and OPNA emulation. The chip runs
// at its native 55467 Hz; libsamplerate converts to the output rate.
int playFmp(const char* path, int, int rate, long long startMs) {
  enum { OPNA_RATE = 55467 };
  // 98fmplayer's loader takes any file as a song and plays noise, so go by extension:
  // FMP (.opi/.ovi/.ozi) and PMD (.m/.m2/.mz)
  const std::string p = path, ext = p.substr(p.rfind('.') + 1);
  static const char* exts[] = {"opi", "ovi", "ozi", "m", "m2", "mz"};
  bool known = false;
  for (const char* e : exts) known = known || strcasecmp(ext.c_str(), e) == 0;
  if (p.find('.') == std::string::npos || !known) return fail("not an FMP or PMD file");
  int isFmp = 0;
  if (const char* err = fmpw_open(path, &isFmp)) return fail(std::string("98fmplayer: ") + err);
  Info info;
  info.type = isFmp ? "FMP" : "PMD";
  for (int line = 0; line < 3; line++) {
    const char* c = fmpw_comment(line);
    if (!c || !*c) continue;
    if (info.title.empty()) info.title = fromSjis(c);
    info.comment += fromSjis(c) + "\n";
  }
  int srcErr = 0;
  SRC_STATE* src = src_new(SRC_SINC_MEDIUM_QUALITY, 2, &srcErr);
  const int CHUNK = 4096;
  std::vector<int16_t> raw(2 * CHUNK);
  std::vector<float> in(2 * CHUNK), out(2 * CHUNK * 2);
  std::vector<int16_t> pcm(2 * CHUNK * 2);
  // Play to the loop point, then 8 seconds of the second pass fading out.
  long long fadeLeft = -1;
  const long long fadeLen = 8LL * OPNA_RATE;
  long long skipFrames = startMs * OPNA_RATE / 1000;
  long long total = 0;
  header(info);
  for (;;) {
    if (!fmpw_playing() || total > 20LL * 60 * OPNA_RATE) break;
    fmpw_render(raw.data(), CHUNK);
    total += CHUNK;
    if (fadeLeft < 0 && fmpw_loops() >= 1) fadeLeft = fadeLen;
    if (skipFrames > 0) {
      skipFrames -= CHUNK;
      continue;
    }
    for (int i = 0; i < 2 * CHUNK; i++) {
      float g = 1.0f;
      if (fadeLeft >= 0) g = static_cast<float>(fadeLeft - i / 2) / fadeLen;
      in[i] = raw[i] / 32768.0f * 2.0f * (g > 0 ? g : 0);  // the OPNA mix runs quiet
    }
    if (fadeLeft >= 0) fadeLeft -= CHUNK;
    SRC_DATA d{};
    d.data_in = in.data();
    d.input_frames = CHUNK;
    d.data_out = out.data();
    d.output_frames = CHUNK * 2;
    d.src_ratio = static_cast<double>(rate) / OPNA_RATE;
    d.end_of_input = 0;
    if (src_process(src, &d) != 0) break;
    src_float_to_short_array(out.data(), pcm.data(), d.output_frames_gen * 2);  // clips
    if (!emit(pcm.data(), d.output_frames_gen)) break;
    if (fadeLeft == 0 || (fadeLeft > 0 && fadeLeft < CHUNK)) break;
  }
  src_delete(src);
  fmpw_close();
  return 0;
}

// MoonBlaster songs need their .MBK sample kit. libkss looks it up by exact
// upper-case names, while Modland's files are lower-case, so find it here
// case-insensitively and hand it over through a symlink with the expected name.
void loadMbk(const std::string& mbm) {
  std::vector<char> head = readFile(mbm.c_str());
  if (head.size() < 0x148) return;
  std::string wanted;
  for (int i = 0; i < 8 && head[0x140 + i] != ' ' && head[0x140 + i]; i++) wanted += head[0x140 + i];
  std::string dir = dirOf(mbm), base = mbm.substr(dir.size());
  const std::string stem = base.substr(0, base.rfind('.'));
  std::string found;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      std::string n = e->d_name;
      if (n.size() < 4 || strcasecmp(n.c_str() + n.size() - 4, ".mbk") != 0) continue;
      std::string ns = n.substr(0, n.size() - 4);
      if (strcasecmp(ns.c_str(), stem.c_str()) == 0) {
        found = n;
        break;
      }
      if (!wanted.empty() && strcasecmp(ns.c_str(), wanted.c_str()) == 0) found = n;
    }
    closedir(d);
  }
  if (found.empty() || wanted.empty()) return;
  // in TMPDIR: under Landlock (the sandbox in a Flatpak) only a folder of its own is
  // writable, and TMPDIR names it
  const char* tmpdir = std::getenv("TMPDIR");
  std::string pattern = std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") + "/bbmp-mbk-XXXXXX";
  std::vector<char> made(pattern.begin(), pattern.end());
  made.push_back(0);
  char* tmp = made.data();
  if (!mkdtemp(tmp)) return;
  std::string upper = wanted;
  for (auto& c : upper) c = std::toupper(static_cast<unsigned char>(c));
  const std::string link = std::string(tmp) + "/" + upper + ".MBK";
  if (symlink((dir + found).c_str(), link.c_str()) == 0) {
    KSS_autoload_mbk(mbm.c_str(), tmp, "");
    unlink(link.c_str());
  }
  rmdir(tmp);
}

// Three of the MSX drivers libkss can build in may not be passed on, so the build
// leaves them out (tools/build-chip.sh). The player downloads them on request and names
// their folder in BBMP_MSX_DRIVERS; a song that needs one is played with it from there,
// or refused when it isn't there. MGSDRV's and MoonBlaster's stay built in, and a KSS
// rip carries its own driver.
struct MsxDriver {
  int type;
  const char* file;
  const char* need;
  int (*set)(const uint8_t*, uint32_t);
};
const MsxDriver MSX_DRIVERS[] = {
  {BGMDATA, "kinrou5.bin", "MuSICA songs need MuSICA's driver (kinrou5)", KSS_set_kinrou},
  {MPK106DATA, "mpk106.bin", "MPK songs need MPK's driver (mpk106)", KSS_set_mpk106},
  {MPK103DATA, "mpk103.bin", "MPK songs need MPK's driver (mpk103)", KSS_set_mpk103},
  {OPXDATA, "opx4kss.bin", "OPLLDRV songs need OPLLDRV's driver (opx4kss)", KSS_set_opxdrv},
};

// "" when the song needs none of them or it was handed to libkss; else why not
std::string loadMsxDriver(const char* path) {
  std::vector<char> data = readFile(path);
  const int type = KSS_check_type(reinterpret_cast<uint8_t*>(data.data()),
                                  static_cast<uint32_t>(data.size()), path);
  for (const MsxDriver& d : MSX_DRIVERS) {
    if (d.type != type) continue;
    const char* dir = std::getenv("BBMP_MSX_DRIVERS");
    if (!dir || !*dir) return std::string(d.need) + ", which is not here";
    std::vector<char> drv = readFile((std::string(dir) + "/" + d.file).c_str());
    if (drv.empty() || d.set(reinterpret_cast<const uint8_t*>(drv.data()),
                             static_cast<uint32_t>(drv.size())) != 0)
      return std::string(d.need) + ", which is not in " + dir;
    return "";
  }
  return "";
}

// MSX music: KSS rips and the MSX sound drivers' own formats (MGSDRV, MuSICA,
// MoonBlaster, OPLLDRV, MPK), which libkss wraps in a KSS image with the driver.
int playKss(const char* path, int track, int rate, long long startMs) {
  std::string p = path;
  const std::string noDriver = loadMsxDriver(path);
  if (!noDriver.empty()) return fail(noDriver);
  if (p.size() > 4 && strcasecmp(p.c_str() + p.size() - 4, ".mbm") == 0) loadMbk(p);
  KSS* kss = KSS_load_file(const_cast<char*>(path));
  if (!kss) return fail("libkss can't load this file");
  Info info;
  info.type = "KSS";
  info.title = fromSjis(reinterpret_cast<const char*>(kss->title));
  if (kss->info && kss->info_num > 0) {
    info.tracks = kss->info_num;
  } else if (kss->trk_max > kss->trk_min) {
    info.tracks = kss->trk_max - kss->trk_min + 1;
  }
  info.track = track;
  int song = track;
  if (kss->info && track < kss->info_num) {
    song = kss->info[track].song;
    if (kss->info[track].title[0]) info.title = fromSjis(kss->info[track].title);
    info.durationMs = kss->info[track].time_in_ms;
  } else if (kss->trk_max > kss->trk_min) {
    song = kss->trk_min + track;
  }
  KSSPLAY* kp = KSSPLAY_new(rate, 2, 16);
  KSSPLAY_set_data(kp, kss);
  KSSPLAY_reset(kp, song, 0);
  KSSPLAY_set_device_pan(kp, KSS_DEVICE_PSG, -32);
  KSSPLAY_set_device_pan(kp, KSS_DEVICE_SCC, 32);
  for (int ch = 0; ch < 6; ch++) KSSPLAY_set_channel_pan(kp, KSS_DEVICE_OPLL, ch, 1 + (ch & 1));
  // silence this long ends a song: as long as the player waits at a song's end
  // (ending.SILENT_END), so a song whose music starts late (MoonBlaster's INTRO6, 4 s
  // in) is not ended before it starts
  KSSPLAY_set_silent_limit(kp, 12000);
  // Fade out when the song loops; songs that never report a loop fade at 4 minutes.
  const long long fadeAt = (info.durationMs ? info.durationMs : 4LL * 60 * 1000) * rate / 1000;
  long long done = 0;
  auto render = [&](int16_t* out, int frames) {
    if (KSSPLAY_get_stop_flag(kp) || KSSPLAY_get_fade_flag(kp) == KSSPLAY_FADE_END) return false;
    if ((KSSPLAY_get_loop_count(kp) >= 1 || done >= fadeAt) &&
        KSSPLAY_get_fade_flag(kp) == KSSPLAY_FADE_NONE)
      KSSPLAY_fade_start(kp, 8000);
    KSSPLAY_calc(kp, out, frames);
    done += frames;
    return done < fadeAt + 10LL * rate;
  };
  if (startMs) skip(startMs, rate, render);
  header(info);
  std::vector<int16_t> stereo(2 * 4096);
  while (render(stereo.data(), 4096) && emit(stereo.data(), 4096)) {
  }
  KSSPLAY_delete(kp);
  KSS_delete(kss);
  return 0;
}

// FM Towns Euphony (EUP) through EUPPlayer. Its emulator writes mono 16-bit to a
// FILE*; this cookie stream turns that into stereo on stdout.
ssize_t eupWrite(void*, const char* buf, size_t size) {
  const int16_t* mono = reinterpret_cast<const int16_t*>(buf);
  const size_t n = size / 2;
  std::vector<int16_t> stereo(2 * n);
  for (size_t i = 0; i < n; i++) stereo[2 * i] = stereo[2 * i + 1] = mono[i];
  return emit(stereo.data(), n) ? static_cast<ssize_t>(size) : -1;
}

int playEup(const char* path, int, int rate, long long startMs) {
  static EUPPlayer player;
  static EUP_TownsEmulator device;
  // EUP files have no magic number, and the interpreter happily "plays" any data as
  // noise, so go by the extension.
  const std::string p = path;
  if (p.size() < 4 || strcasecmp(p.c_str() + p.size() - 4, ".eup") != 0) return fail("not an EUP file");
  std::vector<char> file = readFile(path);
  if (file.size() < 2048 + 6 + 6) return fail("not an EUP file");
  // EUPPlayer reads events until an end mark (FE), and no further than that: a file
  // cut short, or without the mark, would be read on past its end. End marks after
  // it (an event is 6 or 12 bytes) stop it there. Where their step time (FE FE: the
  // most, 383 steps) is not yet reached it waits, then stops.
  file.insert(file.end(), 16, static_cast<char>(0xFE));
  u_char* eup = reinterpret_cast<u_char*>(file.data());
#ifdef __APPLE__   // the BSD way of a FILE that writes through a function
  FILE* out = funopen(nullptr, nullptr,
                      [](void* c, const char* b, int n) { return static_cast<int>(eupWrite(c, b, n)); },
                      nullptr, nullptr);
#else
  cookie_io_functions_t io{nullptr, eupWrite, nullptr, nullptr};
  FILE* out = fopencookie(nullptr, "w", io);
#endif
  device.outputStream(out);
  device.outputSampleUnsigned(false);
  device.outputSampleSize(2);
  device.outputSampleLSBFirst(true);
  device.rate(rate);
  player.outputDevice(&device);
  player.stopPlaying();
  player.tempo(eup[2048 + 5] + 30);
  for (int n = 0; n < 32; n++) player.mapTrack_toChannel(n, eup[0x394 + n]);
  for (int n = 0; n < 6; n++) device.assignFmDeviceToChannel(eup[0x6d4 + n]);
  for (int n = 0; n < 8; n++) device.assignPcmDeviceToChannel(eup[0x6da + n]);
  // instrument banks named in the header, looked for next to the song in any case
  const std::string dir = dirOf(path);
  auto bank = [&](int offset, const char* ext) -> std::vector<char> {
    std::string name(reinterpret_cast<const char*>(eup + offset), 8);
    name = name.substr(0, name.find_first_of(std::string(" \0", 2)));
    if (name.empty()) return {};
    name += ext;
    std::vector<char> data;
    if (DIR* d = opendir(dir.c_str())) {
      while (dirent* e = readdir(d))
        if (strcasecmp(e->d_name, name.c_str()) == 0) data = readFile((dir + e->d_name).c_str());
      closedir(d);
    }
    return data;
  };
  std::vector<char> fmb = bank(0x6e2, ".fmb"), pmb = bank(0x6ea, ".pmb");
  for (size_t n = 0; fmb.size() > 8 && n < (fmb.size() - 8) / 48; n++)
    device.setFmInstrumentParameter(n, reinterpret_cast<u_char*>(fmb.data()) + 8 + 48 * n);
  // A PCM bank: 8 bytes, 32 instruments of 128 bytes, then sounds of a 32-byte header
  // (the count of samples at 12) and the samples. EUPPlayer takes the instruments as
  // there and reads each sound whole, so it is given the bank up to the last sound
  // that is complete, and none that is too short for the instruments.
  size_t pmbSize = 8 + 128 * 32;
  if (pmb.size() >= pmbSize) {
    const u_char* b = reinterpret_cast<const u_char*>(pmb.data());
    while (pmb.size() - pmbSize >= 32) {
      const uint64_t samples = b[pmbSize + 12] | b[pmbSize + 13] << 8 | b[pmbSize + 14] << 16 |
                               static_cast<uint64_t>(b[pmbSize + 15]) << 24;
      if (samples > pmb.size() - pmbSize - 32) break;
      pmbSize += 32 + samples;
    }
    device.setPcmInstrumentParameters(reinterpret_cast<u_char*>(pmb.data()), pmbSize);
  }
  Info info;
  info.type = "Euphony";
  std::string title(file.data(), 32);
  title = title.substr(0, title.find('\0'));
  info.title = fromSjis(title.c_str());
  if (startMs) {
    // no seeking: rendering is fast, so play the start into the void
    FILE* sink = std::fopen("/dev/null", "w");
    device.outputStream(sink);
    player.startPlaying(eup + 2048 + 6);
    long long ticks = 0;
    const long long budget = startMs * 1000 / 1012;  // roughly one tick per ms of timeStep
    while (player.isPlaying() && ticks++ < budget) player.nextTick();
    std::fclose(sink);
    device.outputStream(out);
  } else {
    player.startPlaying(eup + 2048 + 6);
  }
  header(info);
  std::fflush(stdout);
  const long long limit = 15LL * 60 * 1000;
  long long ticks = 0;
  while (player.isPlaying() && ticks++ < limit && !std::ferror(out)) {
    player.nextTick();
    std::fflush(out);
  }
  std::fclose(out);
  return 0;
}

// Organya (Cave Story's music format) through organya.h and its default soundbank.
int playOrganya(const char* path, int, int rate, long long startMs) {
  static organya_context ctx;
  if (organya_context_init(&ctx) != ORG_RESULT_SUCCESS) return fail("organya init failed");
  const std::string bank = organyaSoundbank();
  if (bank.empty()) return fail("Organya soundbank missing (BBMP_ORGANYA_BANK)");
  if (organya_context_load_soundbank_file(&ctx, bank.c_str()) != ORG_RESULT_SUCCESS)
    return fail(("Organya soundbank missing: " + bank + " can't be read").c_str());
  organya_context_set_sample_rate(&ctx, rate);
  organya_context_set_volume(&ctx, 0.55f);  // otherwise about twice as loud as everything else
  organya_context_set_interpolation(&ctx, ORG_INTERPOLATION_CUBIC);
  organya_context_set_output_format(&ctx, ORG_OUTPUT_FORMAT_S16, ORG_FALSE);
  if (organya_context_load_song_file(&ctx, path) != ORG_RESULT_SUCCESS) return fail("not an Organya song");
  // Once through to the end of the repeat range, then 8 seconds more fading out.
  const long long passMs = static_cast<long long>(ctx.song.repeat_end) * ctx.song.tempo_ms;
  Info info;
  info.type = "Organya";
  info.durationMs = passMs + 8000;
  const long long passFrames = passMs * rate / 1000, fadeFrames = 8LL * rate;
  long long pos = 0;
  auto render = [&](int16_t* out, int frames) {
    organya_context_generate_samples(&ctx, out, frames);
    for (int i = 0; i < frames; i++) {
      const long long f = pos + i - passFrames;
      if (f > 0) {
        const float g = f >= fadeFrames ? 0.0f : 1.0f - static_cast<float>(f) / fadeFrames;
        out[2 * i] = static_cast<int16_t>(out[2 * i] * g);
        out[2 * i + 1] = static_cast<int16_t>(out[2 * i + 1] * g);
      }
    }
    pos += frames;
    return pos < passFrames + fadeFrames;
  };
  if (startMs) skip(startMs, rate, render);
  header(info);
  std::vector<int16_t> stereo(2 * 4096);
  while (render(stereo.data(), 4096) && emit(stereo.data(), 4096)) {
  }
  organya_context_deinit(&ctx);
  return 0;
}

// Piston Collage (ptcop / pttune) through Pixel's pxtone library.
bool pxRead(void* user, void* dst, int32_t size, int32_t num) {
  return std::fread(dst, size, num, static_cast<FILE*>(user)) == static_cast<size_t>(num);
}
bool pxWrite(void*, const void*, int32_t, int32_t) { return false; }
bool pxSeek(void* user, int32_t mode, int32_t size) {
  return std::fseek(static_cast<FILE*>(user), size, mode) == 0;
}
bool pxPos(void* user, int32_t* pos) {
  *pos = static_cast<int32_t>(std::ftell(static_cast<FILE*>(user)));
  return *pos >= 0;
}

int playPxtone(const char* path, int, int rate, long long startMs) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return fail("can't read file");
  pxtnService px(pxRead, pxWrite, pxSeek, pxPos);
  if (px.init() != pxtnOK || !px.set_destination_quality(2, rate) || px.read(f) != pxtnOK ||
      px.tones_ready() != pxtnOK) {
    std::fclose(f);
    return fail("pxtone can't load this song");
  }
  std::fclose(f);
  Info info;
  info.type = "pxtone";
  int32_t len = 0;
  if (const char* name = px.text->get_name_buf(&len); name && len > 0)
    info.title = fromSjis(std::string(name, len).c_str());
  if (info.title == "no name") info.title.clear();  // pxtone's placeholder
  if (const char* text = px.text->get_comment_buf(&len); text && len > 0)
    info.comment = fromSjis(std::string(text, len).c_str());
  const long long total = px.moo_get_total_sample();
  info.durationMs = total * 1000 / rate;
  px.moo_set_loop(false);
  pxtnVOMITPREPARATION prep{};
  prep.start_pos_float = total > 0 ? static_cast<float>(startMs * rate / 1000) / total : 0;
  prep.master_volume = 0.8f;
  if (!px.moo_preparation(&prep)) return fail("pxtone can't start the song");
  header(info);
  std::vector<int16_t> stereo(2 * 4096);
  while (!px.moo_is_end_vomit() && px.Moo(stereo.data(), 4096 * 4) && emit(stereo.data(), 4096)) {
  }
  return 0;
}

// Capcom QSound (CPS1/CPS2 arcade) through Highly Quixotic, resampled from 44.1 kHz.
int playQsf(const char* path, int, int rate, long long startMs) {
  qsfw_info qi{};
  if (const char* err = qsfw_open(path, &qi)) return fail(err);
  Info info;
  info.type = "QSF";
  info.title = qi.title ? qi.title : "";
  info.author = qi.author ? qi.author : "";
  info.facts = {{"Game", qi.game}, {"Year", qi.year}, {"Copyright", qi.copyright}, {"Ripped by", qi.ripper}};
  info.comment = qi.comment;
  const long long lengthMs = qi.length_ms > 0 ? qi.length_ms : (2 * 60 + 50) * 1000;
  const long long fadeMs = qi.fade_ms >= 0 ? qi.fade_ms : 10000;
  info.durationMs = lengthMs + fadeMs;
  const long long playFrames = lengthMs * QSFW_RATE / 1000, fadeFrames = fadeMs * QSFW_RATE / 1000;
  int srcErr = 0;
  SRC_STATE* src = src_new(SRC_SINC_MEDIUM_QUALITY, 2, &srcErr);
  const int CHUNK = 4096;
  std::vector<int16_t> raw(2 * CHUNK), pcm(4 * CHUNK);
  std::vector<float> in(2 * CHUNK), out(4 * CHUNK);
  long long pos = 0, skipFrames = startMs * QSFW_RATE / 1000;
  header(info);
  while (pos < playFrames + fadeFrames) {
    const int n = qsfw_render(raw.data(), CHUNK);
    if (n <= 0) break;
    for (int i = 0; i < n; i++) {
      const long long f = pos + i;
      float g = 1.0f;
      if (f >= playFrames) g = fadeFrames > 0 ? 1.0f - static_cast<float>(f - playFrames) / fadeFrames : 0;
      if (g < 0) g = 0;
      in[2 * i] = raw[2 * i] / 32768.0f * g;
      in[2 * i + 1] = raw[2 * i + 1] / 32768.0f * g;
    }
    pos += n;
    if (skipFrames > 0) {
      skipFrames -= n;
      continue;
    }
    SRC_DATA d{};
    d.data_in = in.data();
    d.input_frames = n;
    d.data_out = out.data();
    d.output_frames = 2 * CHUNK;
    d.src_ratio = static_cast<double>(rate) / QSFW_RATE;
    if (src_process(src, &d) != 0) break;
    src_float_to_short_array(out.data(), pcm.data(), d.output_frames_gen * 2);
    if (!emit(pcm.data(), d.output_frames_gen)) break;
  }
  src_delete(src);
  qsfw_close();
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 4 && std::string(argv[1]) == "rhythm") {
    const int r = fmpw_write_rhythm(argv[2], argv[3]);
    if (r != 0) std::fprintf(stderr, "rhythm: error %d\n", r);
    return r == 0 ? 0 : 1;
  }
  if (argc < 5) {
    std::fprintf(stderr, "usage: %s <ym|adplug|mdx|pmdc|sc68|fmp|kss|eup|organya|pxtone|qsf> <file> <track> <samplerate> [start-ms]\n", argv[0]);
    return 2;
  }
  const std::string engine = argv[1];
  const char* path = argv[2];
  const int track = std::atoi(argv[3]);
  const int rate = std::atoi(argv[4]);
  const long long startMs = argc > 5 ? std::atoll(argv[5]) : 0;
  if (engine == "ym") return playYm(path, track, rate, startMs);
  if (engine == "adplug") return playAdplug(path, track, rate, startMs);
  if (engine == "mdx") return playMdx(path, track, rate, startMs);
  if (engine == "pmdc") return playPmdc(path, track, rate, startMs);
  if (engine == "sc68") return playSc68(path, track, rate, startMs);
  if (engine == "fmp") return playFmp(path, track, rate, startMs);
  if (engine == "kss") return playKss(path, track, rate, startMs);
  if (engine == "eup") return playEup(path, track, rate, startMs);
  if (engine == "organya") return playOrganya(path, track, rate, startMs);
  if (engine == "pxtone") return playPxtone(path, track, rate, startMs);
  if (engine == "qsf") return playQsf(path, track, rate, startMs);
  return fail("unknown engine " + engine);
}
