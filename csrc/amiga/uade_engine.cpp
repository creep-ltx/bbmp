// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-amiga: formats that UADE plays once the files are laid out as the Amiga
// player wants them. The engines here build that layout in a temporary folder (links,
// and files made from the song's files), let libuade play from there and remove the
// folder as soon as UADE has loaded everything.
//
//   zoundmonitor  ZoundMonitor: the samples are files of their own, in a "Samples"
//                 folder that the player looks for next to the song. Modland has one
//                 folder Zoundmonitor/Samples for all songs.
//   jesperolsen   Jesper Olsen: songs with the replay routine in front play as they
//                 are; all others go to the engine "jo" (jo_engine.cpp).
//   stonetracker  StoneTracker: name.spm and name.sps, played by StoneTracker's own
//                 replay routine (players/StoneTracker, see stonetracker_player.asm).
//                 Packed sample banks are unpacked here.
//
// UADE is used with a base folder of its own, <prefix>/share/uade, which the build
// script makes: UADE's files and players plus the ones of bbmp-amiga. The players that
// may not be passed on are downloaded on request and found through BBMP_UADE_PLAYERS.

#include "engine.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>

#include <uade/uade.h>

namespace {

std::string prefix_dir() {
  if (const char* env = std::getenv("BBMP_AMIGA_PREFIX")) return env;
  char buf[PATH_MAX];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  buf[n] = 0;
  std::string p = buf;                       // <prefix>/bin/bbmp-amiga
  for (int i = 0; i < 2; ++i) {
    const size_t slash = p.rfind('/');
    if (slash == std::string::npos) return ".";
    p.erase(slash);
  }
  return p;
}

std::string dir_of(const std::string& path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos) return ".";
  return slash == 0 ? "/" : path.substr(0, slash);
}

std::string name_of(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string absolute(const std::string& path) {
  char buf[PATH_MAX];
  if (realpath(path.c_str(), buf)) return buf;
  return path;
}

// The entry of a folder with this name, whatever its case; empty when there is none.
std::string find_in(const std::string& folder, const std::string& name) {
  struct stat st;
  const std::string exact = folder + "/" + name;
  if (stat(exact.c_str(), &st) == 0) return exact;
  std::string found;
  if (DIR* d = opendir(folder.c_str())) {
    while (const dirent* e = readdir(d))
      if (strcasecmp(e->d_name, name.c_str()) == 0) { found = folder + "/" + e->d_name; break; }
    closedir(d);
  }
  return found;
}

bool read_file(const std::string& path, std::vector<uint8_t>& data) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 0 || size > (256L << 20)) { std::fclose(f); return false; }
  data.resize(static_cast<size_t>(size));
  const size_t got = size ? std::fread(data.data(), 1, data.size(), f) : 0;
  std::fclose(f);
  return got == data.size();
}

bool write_file(const std::string& path, const uint8_t* a, size_t na, const uint8_t* b = nullptr, size_t nb = 0) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  bool ok = std::fwrite(a, 1, na, f) == na;
  if (ok && nb) ok = std::fwrite(b, 1, nb, f) == nb;
  return std::fclose(f) == 0 && ok;
}

void remove_tree(const std::string& folder) {
  if (DIR* d = opendir(folder.c_str())) {
    while (const dirent* e = readdir(d)) {
      if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
      const std::string p = folder + "/" + e->d_name;
      struct stat st;
      if (lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(p);
      else unlink(p.c_str());
    }
    closedir(d);
  }
  rmdir(folder.c_str());
}

std::string text(const uint8_t* p, size_t max) {
  std::string s;
  for (size_t i = 0; i < max && p[i]; ++i) s += static_cast<char>(p[i] < 0x20 ? ' ' : p[i]);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

// ---------------------------------------------------------------------------------

class UadeEngine : public Engine {
 public:
  ~UadeEngine() override {
    if (state_) uade_cleanup_state(state_);
    drop_folder();
  }

  bool open(const std::string& path, const std::vector<uint8_t>& file, int track, int rate,
            SongInfo& info, std::string& error) override {
    const std::string base = prefix_dir() + "/share/uade";
    struct stat st;
    if (stat((base + "/eagleplayer.conf").c_str(), &st) != 0) {
      error = "bbmp-amiga is not installed: no " + base;
      return false;
    }
    const std::string whole = absolute(path);
    // The length of the song's path moves what UADE lays out in the Amiga's memory, and
    // with it the sound (a little). So the folder is made in the temporary folder by a
    // path relative to it, as long as "/tmp/bbmp-amiga-tmpXXXXXX" was (and
    // "/tmp/modland-amiga-XXXXXX" before it): the sound is the same wherever TMPDIR is
    // (a sandbox's folder of its own under Landlock is longer than /tmp).
    const char* tmp = std::getenv("TMPDIR");
    if (chdir(tmp && *tmp ? tmp : "/tmp") != 0) { error = "can't use the temporary folder"; return false; }
    std::string pattern = "bbmp-amiga-tmp-----XXXXXX";
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back(0);
    if (!mkdtemp(buf.data())) { error = "can't make a temporary folder"; return false; }
    folder_ = buf.data();

    std::string song;
    if (!prepare(whole, file, prefix_dir() + "/share", song, info, error)) return false;

    uade_config* cfg = uade_new_config();
    if (!cfg) { error = "UADE failed to start"; return false; }
    const std::string hz = std::to_string(rate);
    uade_config_set_option(cfg, UC_BASE_DIR, base.c_str());
    // UADE's core, beside this helper (<opt>/bbmp-amiga -> <opt>/uade), when the
    // folder it was built in is gone (a package)
    const std::string opt = prefix_dir().substr(0, prefix_dir().rfind('/'));
    const std::string core = opt + "/uade/lib/uade/uadecore";
    if (stat(core.c_str(), &st) == 0) uade_config_set_option(cfg, UC_UADECORE_FILE, core.c_str());
    uade_config_set_option(cfg, UC_FREQUENCY, hz.c_str());
    uade_config_set_option(cfg, UC_ONE_SUBSONG, nullptr);
    uade_config_set_option(cfg, UC_PANNING_VALUE, "0.4");   // as Modland Player's UADE backend
    uade_config_set_option(cfg, UC_SUBSONG_TIMEOUT_VALUE, "600");
    uade_config_set_option(cfg, UC_SILENCE_TIMEOUT_VALUE, "10");
    if (debug_on()) uade_config_set_option(cfg, UC_VERBOSE, nullptr);
    state_ = uade_new_state(cfg);
    std::free(cfg);
    if (!state_) { error = "UADE failed to start"; return false; }

    if (uade_play(song.c_str(), -1, state_) <= 0) { error = refusal(); return false; }
    const struct uade_song_info* si = uade_get_song_info(state_);
    const int low = si->subsongs.min, high = si->subsongs.max;
    info.tracks = high >= low ? high - low + 1 : 1;
    if (track >= info.tracks) { error = "no such track"; return false; }
    if (si->subsongs.cur != low + track) {
      uade_stop(state_);
      if (uade_play(song.c_str(), low + track, state_) <= 0) { error = refusal(); return false; }
    }
    drop_folder();                     // UADE has read all it needs
    rate_ = rate;
    return true;
  }

  // UADE ends a song that has become silent ten seconds after the last sound. Those
  // seconds are not given out: silence is held back until sound follows it.
  size_t render(int16_t* out, size_t frames) override {
    size_t done = 0;
    while (done < frames) {
      if (ready_.empty()) {
        if (ended_) break;
        int16_t buf[2048];
        const ssize_t n = uade_read(buf, sizeof(buf), state_);
        if (n <= 0) {
          ended_ = true;
          // what is left of the song's last sound may ring out for a second
          const size_t keep = std::min(held_.size(), static_cast<size_t>(rate_) * 2);
          ready_.insert(ready_.end(), held_.begin(), held_.begin() + static_cast<long>(keep));
          held_.clear();
          continue;
        }
        for (ssize_t i = 0; i + 1 < n / 2; i += 2) {
          const bool silent = buf[i] > -4 && buf[i] < 4 && buf[i + 1] > -4 && buf[i + 1] < 4;
          held_.push_back(buf[i]);
          held_.push_back(buf[i + 1]);
          if (!silent) {
            ready_.insert(ready_.end(), held_.begin(), held_.end());
            held_.clear();
          }
        }
        continue;
      }
      const size_t take = std::min(frames - done, ready_.size() / 2);
      for (size_t i = 0; i < take * 2; ++i) out[done * 2 + i] = ready_[i];
      ready_.erase(ready_.begin(), ready_.begin() + static_cast<long>(take * 2));
      done += take;
    }
    return done;
  }

 protected:
  // Makes the files in folder() and says which one UADE is to play.
  virtual bool prepare(const std::string& path, const std::vector<uint8_t>& file,
                       const std::string& share, std::string& song, SongInfo& info,
                       std::string& error) = 0;

  const std::string& folder() const { return folder_; }

  bool link(const std::string& target, const std::string& name, std::string& error) {
    if (symlink(target.c_str(), (folder_ + "/" + name).c_str()) != 0) {
      error = "can't make a link in the temporary folder";
      return false;
    }
    return true;
  }

 private:
  std::string refusal() {
    std::string why = "UADE can't play this file";
    // A player that isn't there: those whose terms don't allow passing them on are
    // downloaded on request and found through BBMP_UADE_PLAYERS. The patched libuade
    // (tools/build-uade.sh) names the player it couldn't load.
    const struct uade_song_info* si = uade_get_song_info(state_);
    struct stat st;
    const bool missing = si && si->playerfname[0] && stat(si->playerfname, &st) != 0;
    uade_notification n;
    while (uade_read_notification(&n, state_)) {
      if (n.type == UADE_NOTIFICATION_SONG_END && n.song_end.reason)
        why = std::string("UADE can't play this file: ") + n.song_end.reason;
      uade_cleanup_notification(&n);
    }
    if (missing) why = std::string("UADE's Amiga player ") + name_of(si->playerfname) + " is not there";
    return why;
  }

  void drop_folder() {
    if (!folder_.empty()) remove_tree(folder_);
    folder_.clear();
  }

  uade_state* state_ = nullptr;
  std::string folder_;
  std::deque<int16_t> ready_, held_;
  bool ended_ = false;
  int rate_ = 48000;
};

// ---------------------------------------------------------------------------------

class ZoundMonitor : public UadeEngine {
  bool prepare(const std::string& path, const std::vector<uint8_t>& file, const std::string&,
               std::string& song, SongInfo& info, std::string& error) override {
    // the check of the Amiga player: tables of the sizes the first two bytes give,
    // and after them the path the samples were loaded from ("df?:..." or "?amp...")
    const size_t at = (file.size() > 1 ? (file[0] + 1) * 16 + (file[1] + 1) * 128 : 0) + 869;
    if (file.size() < 873 || at + 4 >= file.size() ||
        !((file[at] == 'd' && file[at + 1] == 'f' && file[at + 3] == ':') ||
          (file[at + 1] == 'a' && file[at + 2] == 'm' && file[at + 3] == 'p'))) {
      error = "not a ZoundMonitor song";
      return false;
    }
    const std::string dir = dir_of(path);
    std::vector<std::string> folders;
    for (const std::string& d : {dir, dir_of(dir), dir_of(dir_of(dir))}) {
      const std::string f = find_in(d, "Samples");
      if (!f.empty()) folders.push_back(f);
    }
    if (mkdir((folder() + "/Samples").c_str(), 0700) != 0) { error = "can't make a temporary folder"; return false; }
    std::string missing;
    for (int i = 0; i < 16; ++i) {
      const std::string name = text(&file[9 + 54 * i], 40);
      if (name.empty()) continue;
      info.names.push_back(name);
      // the player asks for "Samples/" and the name as it is written, so only plain
      // names can be served
      if (name.find('/') != std::string::npos || name.find(':') != std::string::npos) {
        error = "the song names a sample with its path: " + name;
        return false;
      }
      std::string found;
      for (const std::string& f : folders)
        if (found.empty()) found = find_in(f, name);
      if (found.empty()) { missing += (missing.empty() ? "" : ", ") + name; continue; }
      if (!link(found, "Samples/" + name, error)) return false;
    }
    if (!missing.empty()) {
      error = "samples are missing (looked in the Samples folders next to and above the song): " + missing;
      return false;
    }
    info.type = "ZoundMonitor";
    song = folder() + "/song.sng";
    return link(path, "song.sng", error);
  }
};

// ---------------------------------------------------------------------------------

// Songs with their replay routine in front (Guldkornsexpressen) play through UADE as
// they are. All others (LollyPop, Georg Glaxo, Harald Hardtand) are played by the
// engine "jo" (jo_engine.cpp), written for this player: the games' own routines may not
// be shipped.
class JesperOlsenRoutine : public UadeEngine {
  bool prepare(const std::string& path, const std::vector<uint8_t>&, const std::string&,
               std::string& song, SongInfo& info, std::string& error) override {
    info.type = "Jesper Olsen";
    song = folder() + "/jo.song";
    return link(path, "jo.song", error);
  }
};

class JesperOlsen : public Engine {
 public:
  bool open(const std::string& path, const std::vector<uint8_t>& file, int track, int rate,
            SongInfo& info, std::string& error) override {
    if (file.size() >= 64 && be16(&file[0]) == 0x6000) engine_.reset(new JesperOlsenRoutine);
    else engine_.reset(make_engine("jo"));
    if (!engine_) { error = "the Jesper Olsen engine is not built in"; return false; }
    return engine_->open(path, file, track, rate, info, error);
  }
  size_t render(int16_t* out, size_t frames) override { return engine_ ? engine_->render(out, frames) : 0; }

 private:
  std::unique_ptr<Engine> engine_;
};

// ---------------------------------------------------------------------------------

// StoneTracker can keep its sample bank packed (flags bit 0 of the bank). The only
// packing known is "delta Huffman" (tag "psn"): after the bank's header come the tag, a
// code count, the unpacked length and a table of codes; then, from an even offset, the
// coded bits in big-endian 16-bit words, highest bit first. Each decoded value is the
// difference to the previous sample byte. Written from the format's description.
struct BankTreeNode {
  int next[2] = {-1, -1};  // index of the node behind a 0 bit and behind a 1 bit, -1 if none
  uint8_t value = 0;
};

bool unpack_bank(const std::vector<uint8_t>& packed, std::vector<uint8_t>& plain, std::string& error) {
  const char* const damaged = "the sample bank is damaged";
  const size_t size = packed.size();
  const size_t header = 6 + 32 * static_cast<size_t>(packed[5]);
  if (size < header + 8 || std::memcmp(&packed[header], "psn", 3) != 0) {
    error = "the sample bank is packed in a way that is not known (only delta Huffman is)";
    return false;
  }
  const unsigned codes = packed[header + 3] + 1u;
  const uint32_t length = be32(&packed[header + 4]);
  if (length > 64u * 1024 * 1024) {
    error = damaged;
    return false;
  }

  // The tree: a node with no 1-branch stops the walk (it is a leaf); a code that ends
  // somewhere takes that node's 1-branch away, whatever hung below it.
  std::vector<BankTreeNode> tree(1);
  size_t pos = header + 8;
  for (unsigned c = 0; c < codes; ++c) {
    if (pos + 2 > size) {
      error = damaged;
      return false;
    }
    const uint8_t value = packed[pos];
    const unsigned bits = packed[pos + 1];
    const size_t width = std::min(bits / 8u, 3u) + 1;
    pos += 2;
    if (pos + width > size || bits == 0 || bits > 32) {
      error = damaged;
      return false;
    }
    uint32_t pattern = 0;
    for (size_t i = 0; i < width; ++i) pattern = (pattern << 8) | packed[pos + i];
    pos += width;
    size_t at = 0;
    for (unsigned b = 0; b < bits; ++b) {
      const unsigned way = (pattern >> b) & 1u;
      if (tree[at].next[way] < 0) {
        tree[at].next[way] = static_cast<int>(tree.size());
        tree.emplace_back();
      }
      at = static_cast<size_t>(tree[at].next[way]);
    }
    tree[at].value = value;
    tree[at].next[1] = -1;
  }
  if (pos & 1) ++pos;

  plain.assign(packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(header));
  plain[4] &= 0xfe;
  // at most one byte per coded bit, so a short file can't make a huge reservation
  plain.reserve(header + std::min<size_t>(length, pos < size ? (size - pos) * 8 : 0));
  const size_t end = header + length;
  uint8_t sample = 0;
  size_t at = 0;
  while (plain.size() < end) {
    if (pos + 2 > size) {
      error = damaged;
      return false;
    }
    const unsigned word = be16(&packed[pos]);
    pos += 2;
    for (int b = 15; b >= 0 && plain.size() < end; --b) {
      const int step = tree[at].next[(word >> b) & 1u];
      if (step < 0) {
        error = damaged;
        return false;
      }
      at = static_cast<size_t>(step);
      if (tree[at].next[1] < 0) {
        sample = static_cast<uint8_t>(sample + tree[at].value);
        plain.push_back(sample);
        at = 0;
      }
    }
  }
  return true;
}

class StoneTracker : public UadeEngine {
  bool prepare(const std::string& path, const std::vector<uint8_t>& file, const std::string&,
               std::string& song, SongInfo& info, std::string& error) override {
    if (file.size() < 64 || std::memcmp(file.data(), "SPM", 3) != 0 || file[3] < 1 || file[3] > 2 ||
        be16(&file[36]) == 0) {
      error = "not a StoneTracker module";
      return false;
    }
    // name.spm -> name.sps, SPM.name -> SPS.name
    std::string name = name_of(path), bank;
    if (name.size() > 4 && strncasecmp(name.c_str(), "spm.", 4) == 0) {
      bank = find_in(dir_of(path), "SPS." + name.substr(4));
    } else if (name.size() > 4 && strcasecmp(name.c_str() + name.size() - 4, ".spm") == 0) {
      bank = find_in(dir_of(path), name.substr(0, name.size() - 3) + "sps");
    }
    std::vector<uint8_t> samples;
    if (bank.empty() || !read_file(bank, samples)) {
      error = "the sample bank (.sps) of the module is missing";
      return false;
    }
    if (samples.size() < 6 || std::memcmp(samples.data(), "SPS", 3) != 0 || samples[3] < 1 || samples[3] > 2 ||
        samples.size() < 6 + 32 * static_cast<size_t>(samples[5])) {
      error = "the sample bank (.sps) is not a StoneTracker sample bank";
      return false;
    }
    for (unsigned i = 0; i < samples[5]; ++i) info.names.push_back(text(&samples[6 + 32 * i], 8));
    info.type = "StoneTracker";
    info.title = text(&file[4], 31);
    // the songs of the module have names of their own: they are the comment
    const unsigned songs = be16(&file[36]);
    for (unsigned i = 0; i < songs && 48 + 4 * i + 4 <= file.size(); ++i) {
      const size_t at = be32(&file[48 + 4 * i]);
      if (at + 36 > file.size()) break;
      info.comment += (i ? "\n" : "") + text(&file[at], 31);
    }
    if (samples[4] & 1) {
      std::vector<uint8_t> plain;
      if (!unpack_bank(samples, plain, error)) return false;
      if (!write_file(folder() + "/SPS.song", plain.data(), plain.size())) {
        error = "can't write to the temporary folder";
        return false;
      }
    } else if (!link(absolute(bank), "SPS.song", error)) {
      return false;
    }
    song = folder() + "/SPM.song";
    return link(path, "SPM.song", error);
  }
};

Engine* make_zoundmonitor() { return new ZoundMonitor; }
Engine* make_jesperolsen() { return new JesperOlsen; }
Engine* make_stonetracker() { return new StoneTracker; }

}  // namespace

static EngineEntry entry_zoundmonitor("zoundmonitor", make_zoundmonitor);
static EngineEntry entry_jesperolsen("jesperolsen", make_jesperolsen);
static EngineEntry entry_stonetracker("stonetracker", make_stonetracker);
