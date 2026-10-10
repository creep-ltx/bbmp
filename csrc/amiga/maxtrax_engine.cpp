// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-amiga: MaxTrax (Joe Pearce's music driver: Legend Entertainment, New World
// Computing, Westwood, Maxis ... on the Amiga), files with the header "MXTX".
//
// The replay routine and the Paula it plays on are ScummVM's (audio/mods/maxtrax.cpp,
// audio/mods/paula.cpp, GPL 3 or later), which the build script fetches; the headers
// in maxtrax_shim/ stand in for the rest of ScummVM. This file reads the file over
// first (ScummVM's loader trusts it), finds the samples of songs that come without
// (a score file of a game whose samples are in another file of the same folder),
// and finds the end of the song.

#include "engine.h"

#include <dirent.h>
#include <strings.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <set>

// the engine has to look at the state of the player to see that a song is over
#define private public
#define protected public
#include "common/scummsys.h"
#include "common/stream.h"
#include "audio/mixer.h"
#include "audio/mods/maxtrax.h"
#undef private
#undef protected

static OSystem the_system;
OSystem *g_system = &the_system;

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

struct Layout {
  unsigned scores = 0, samples = 0;
  std::vector<std::set<int>> wanted;   // per score: the patches its notes are played with
  std::vector<bool> ends;              // per score: has it an end?
  std::vector<int> songs;              // the scores that have notes: these are the tracks
  std::vector<size_t> counts, tails;   // per score: where its number of events is, and where it ends
  std::vector<unsigned> ring;          // per score: how long its last note sounds (ms)
  std::set<int> patches;               // the patches the file has samples for
  bool microtonal = false;
  size_t missing = 0;                  // bytes the file is too short by (of its last sample)
  std::vector<size_t> hollow;          // where the lengths of samples without octaves are
};

// A sample of no octaves has no data, but a length: ScummVM's player would play it from
// an empty array (and take octave -1 for it). Such a sample is made silent.
void silence_hollow(std::vector<uint8_t>& d, const Layout& l) {
  for (size_t at : l.hollow) std::fill(d.begin() + static_cast<long>(at), d.begin() + static_cast<long>(at + 8), 0);
}

// Reads the file over without playing it. False when it is not a MaxTrax file that
// ScummVM's loader can be given.
bool survey(const std::vector<uint8_t>& d, Layout& l, std::string& error) {
  if (d.size() < 10 || std::memcmp(d.data(), "MXTX", 4) != 0) { error = "not a MaxTrax file"; return false; }
  const uint16_t flags = be16(&d[6]);
  size_t p = 8;
  if (flags & 0x8000) { l.microtonal = true; p += 256; }
  if (p + 2 > d.size()) { error = "the file is cut off"; return false; }
  l.scores = be16(&d[p]);
  p += 2;
  for (unsigned s = 0; s < l.scores; ++s) {
    if (p + 4 > d.size()) { error = "the file is cut off"; return false; }
    const uint32_t events = be32(&d[p]);
    p += 4;
    if (events > (d.size() - p) / 6) { error = "the file is cut off"; return false; }
    std::set<int> wanted;
    int program[16];
    for (int& x : program) x = -1;
    bool end = false;
    unsigned ring = 0;
    bool notes = false;
    for (uint32_t i = 0; i < events && !end; ++i) {
      const uint8_t* e = &d[p + 6 * i];
      const int channel = e[1] & 15;
      if (e[0] == 0xC0) program[channel] = be16(e + 4) & 63;
      else if (e[0] < 0x80) {
        if (program[channel] >= 0) wanted.insert(program[channel]);
        ring = be16(e + 4);
        notes = true;
      } else if (e[0] == 0xFF) end = true;
    }
    if (notes && s < 128) l.songs.push_back(static_cast<int>(s));
    l.wanted.push_back(wanted);
    l.ends.push_back(end);
    l.counts.push_back(p - 4);
    l.ring.push_back(ring);
    p += 6 * static_cast<size_t>(events);
    l.tails.push_back(p);
  }
  if (p + 2 > d.size()) return true;            // scores and nothing after them
  l.samples = be16(&d[p]);
  p += 2;
  for (unsigned w = 0; w < l.samples; ++w) {
    if (p + 20 > d.size()) { error = "the file is cut off"; return false; }
    const unsigned number = be16(&d[p]), octaves = be16(&d[p + 6]);
    const uint64_t attack = be32(&d[p + 8]), sustain = be32(&d[p + 12]);
    const unsigned envelopes = be16(&d[p + 16]) + be16(&d[p + 18]);
    if (number >= 64 || octaves > 10 || attack + sustain > (16u << 20)) { error = "not a MaxTrax file"; return false; }
    const uint64_t bytes = (attack + sustain) * ((1u << octaves) - 1);
    if (octaves == 0 && attack + sustain) l.hollow.push_back(p + 8);
    p += 20 + 4 * static_cast<size_t>(envelopes);
    if (p > d.size()) { error = "the file is cut off"; return false; }
    if (bytes > d.size() - p) {
      // the end of the last sample is missing: some rips are a bit short
      if (w + 1 != l.samples || bytes - (d.size() - p) > bytes / 2) { error = "the file is cut off"; return false; }
      l.missing = static_cast<size_t>(bytes - (d.size() - p));
    }
    p += static_cast<size_t>(bytes);
    l.patches.insert(static_cast<int>(number));
  }
  return true;
}

bool read_file(const std::string& path, std::vector<uint8_t>& data) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size <= 0 || size > (64L << 20)) { std::fclose(f); return false; }
  data.resize(static_cast<size_t>(size));
  const size_t got = std::fread(data.data(), 1, data.size(), f);
  std::fclose(f);
  return got == data.size();
}

std::string first_word(const std::string& name) {
  std::string w;
  for (char c : name) {
    if (c == ' ' || c == '.' || c == '(') break;
    w += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return w;
}

class MaxTraxEngine : public Engine {
 public:
  bool open(const std::string& path, const std::vector<uint8_t>& file, int track, int rate,
            SongInfo& info, std::string& error) override {
    Layout l;
    if (!survey(file, l, error)) return false;
    if (l.songs.empty()) { error = "the file holds samples and no song"; return false; }
    // Scores without a note (Dune 2 has them, as place holders) are no tracks.
    if (track >= static_cast<int>(l.songs.size())) { error = "no such track"; return false; }
    track = l.songs[static_cast<size_t>(track)];

    song_ = file;
    song_.resize(song_.size() + l.missing, 0);
    silence_hollow(song_, l);
    // A score without an end mark (the replay routine would play on into what comes
    // after it) gets one, where its last note has rung out.
    for (size_t i = l.scores; i-- > 0;) {
      if (l.ends[i]) continue;
      const uint8_t end[6] = {0xFF, 0, static_cast<uint8_t>(l.ring[i] >> 8), static_cast<uint8_t>(l.ring[i]), 0, 0};
      song_.insert(song_.begin() + static_cast<long>(l.tails[i]), end, end + 6);
      const uint32_t n = be32(&song_[l.counts[i]]) + 1;
      for (int b = 0; b < 4; ++b) song_[l.counts[i] + static_cast<size_t>(b)] = static_cast<uint8_t>(n >> (24 - 8 * b));
      if (static_cast<int>(i) == track) info.comment = "The song has no end mark: it ends after its last note.";
    }
    // A part that is repeated sixteen times or more is the game's way of playing
    // on for ever (Dune 2: a hundred times, which takes an hour and a half): such a
    // part is played twice.
    {
      size_t p = 8 + (l.microtonal ? 256 : 0) + 2;
      for (unsigned i = 0; i < l.scores; ++i) {
        const uint32_t events = be32(&song_[p]);
        p += 4;
        for (uint32_t e = 0; e < events; ++e, p += 6) {
          if (song_[p] != 0xA0 || song_[p + 4] != 2 || song_[p + 5] < 16) continue;
          if (static_cast<int>(i) == track)
            info.comment += std::string(info.comment.empty() ? "" : "\n") + "A part that is repeated " +
                            std::to_string(song_[p + 5]) + " times is played twice.";
          song_[p + 5] = 1;
        }
      }
    }
    samples_.clear();
    std::string from;
    if (l.samples == 0) {
      // A song of a game that keeps its samples in a file of their own: the file of
      // the same folder whose name starts with the same word and that has all the
      // samples this song is played with.
      std::set<int> wanted;
      for (const auto& w : l.wanted) wanted.insert(w.begin(), w.end());
      const size_t slash = path.rfind('/');
      const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
      const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
      std::vector<std::string> others;
      if (DIR* d = opendir(dir.c_str())) {
        while (const dirent* e = readdir(d)) {
          const std::string other = e->d_name;
          if (other != name && other.size() > 5 && !strcasecmp(other.c_str() + other.size() - 5, ".mxtx") &&
              first_word(other) == first_word(name))
            others.push_back(other);
        }
        closedir(d);
      }
      std::sort(others.begin(), others.end());
      size_t best = 0;
      for (const std::string& other : others) {
        std::vector<uint8_t> data;
        Layout ol;
        std::string why;
        if (!read_file(dir + "/" + other, data) || !survey(data, ol, why) || ol.samples == 0) continue;
        if (!std::includes(ol.patches.begin(), ol.patches.end(), wanted.begin(), wanted.end())) continue;
        if (ol.patches.size() > best) {
          best = ol.patches.size();
          data.resize(data.size() + ol.missing, 0);
          silence_hollow(data, ol);
          samples_ = data;
          from = other;
        }
      }
      if (samples_.empty()) {
        error = "the file has no samples, and no other MaxTrax file of its folder has the ones it needs";
        return false;
      }
    }

    rate_ = rate;
    player_.reset(make(rate, error));
    if (!player_) return false;
    info.tracks = static_cast<int>(l.songs.size());
    if (track >= player_->_numScores) { error = "no such track"; return false; }
    info.type = "MaxTrax";
    if (!from.empty()) info.comment += std::string(info.comment.empty() ? "" : "\n") + "Samples from " + from;
    if (l.microtonal) info.comment += std::string(info.comment.empty() ? "" : "\n") + "The song's own tuning table is not used.";

    // The length: the song played once at a low sample rate, which is quick. The
    // sequencer runs the same at every rate.
    {
      const int low = 2000;
      std::unique_ptr<Audio::MaxTrax> dry(make(low, error));
      if (!dry || !dry->playSong(track, false)) { error = "can't start the song"; return false; }
      std::vector<int16_t> buf(static_cast<size_t>(low / 50) * 2);
      long long frames = 0;
      int after = 0, ringing = 0;
      while (frames < 3600LL * low) {
        dry->readBuffer(buf.data(), static_cast<int>(buf.size()));
        frames += low / 50;
        if (over(*dry, ringing) && ++after >= 2) break;
      }
      if (frames >= 3600LL * low) { error = "the song doesn't end"; return false; }
      info.duration_ms = static_cast<long>(frames * 1000 / low);
    }
    if (!player_->playSong(track, false)) { error = "can't start the song"; return false; }
    return true;
  }

  size_t render(int16_t* out, size_t frames) override {
    size_t done = 0;
    const size_t tick = static_cast<size_t>(rate_) / 50;
    while (done < frames && !ended_) {
      const size_t want = std::min(frames - done, tick - in_tick_);
      player_->readBuffer(out + done * 2, static_cast<int>(want * 2));
      done += want;
      in_tick_ += want;
      if (in_tick_ == tick) {
        in_tick_ = 0;
        if (over(*player_, ringing_)) { if (++after_ >= 2) ended_ = true; } else after_ = 0;
        if (++ticks_ > 3600L * 50) ended_ = true;
      }
    }
    return done;
  }

 private:
  // The score has come to its end and the last note has rung out. Asked once every
  // tick (a fiftieth of a second). A note that is never let go of (a looped sample
  // that the score doesn't stop) is given ten seconds.
  static bool over(const Audio::MaxTrax& p, int& ringing) {
    if (p._playerCtx.scoreIndex >= 0) return false;
    if (++ringing > 10 * 50) return true;
    for (const auto& v : p._voiceCtx)
      if (v.status != Audio::MaxTrax::VoiceContext::kStatusFree) return false;
    return true;
  }

  Audio::MaxTrax* make(int rate, std::string& error) {
    std::unique_ptr<Audio::MaxTrax> p(new Audio::MaxTrax(rate, true));
    bool ok;
    if (samples_.empty()) {
      Common::SeekableReadStream s(song_.data(), song_.size());
      ok = p->load(s, true, true);
    } else {
      Common::SeekableReadStream s(song_.data(), song_.size());
      Common::SeekableReadStream w(samples_.data(), samples_.size());
      // scores only: the loader reads the header and the scores and is done
      ok = load_scores(*p, s) && p->load(w, false, true);
    }
    if (!ok) { error = "the file can't be loaded"; return nullptr; }
    return p.release();
  }

  // ScummVM's loader takes it as an error when a file ends right behind its scores;
  // such a file gets an empty list of samples to read.
  bool load_scores(Audio::MaxTrax& p, Common::SeekableReadStream&) {
    std::vector<uint8_t> padded = song_;
    padded.insert(padded.end(), 4, 0);
    Common::SeekableReadStream s(padded.data(), padded.size());
    return p.load(s, true, false);
  }

  std::vector<uint8_t> song_, samples_;
  std::unique_ptr<Audio::MaxTrax> player_;
  int rate_ = 48000;
  size_t in_tick_ = 0;
  long ticks_ = 0;
  int after_ = 0, ringing_ = 0;
  bool ended_ = false;
};

Engine* make_maxtrax() { return new MaxTraxEngine; }

}  // namespace

REGISTER_ENGINE("maxtrax", make_maxtrax)
