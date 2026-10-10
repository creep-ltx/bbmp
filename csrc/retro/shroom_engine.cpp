// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-retro, engine "shroom": songs of Mario Paint's music editor (SNES) as
// ShroomTool by Strobe and setrodox ripped them from ZSNES save states (*.sho).
//
// A song is a grid of up to 96 beats (more in version 3 files, which Mariopants
// writes) with three notes on each: a pitch 1..13 (B below middle C' up to G, white
// keys only) and one of the 15 instruments. There is one tempo for the whole song.
//
// The format and the way of playing are taken from Mariopants 1.1 by Brad Smith
// (files.cpp, player.cpp; http://rainwarrior.ca), changed for bbmp: written again as an
// engine of this helper, songs ending at their last note. Its source.txt permits
// modifying, reusing and redistributing it, asking that its author be credited and
// changes be marked; this is that credit and that mark. Mariopants follows the game: a note ending only
// when the next note of its own channel begins, the short fade of a note that is
// cut. The files ShroomTool wrote don't say where the song ends and whether it
// repeats; such a song is played up to its last note.
//
// The tempo is the game's own, as rnhart found it in the game's memory (nesdev forum,
// "Mario Paint (Music Maker) REing"): on every picture the game adds
// (tempo + 14) * 3291161 to a counter of 32 bits, and goes to the next beat when the
// counter overflows. So a beat always begins with a picture of the 60.1 a second,
// and beats differ in length by one picture. (Mariopants has beats of equal length
// that are 0.6% shorter, measured in an emulator.)
//
// The instruments are samples of the game and not part of this program. Two kinds of
// sample sets are read:
//   shroom-notes.bin  Mariopants' recordings of every instrument at every pitch,
//                     made by shroom-samples (see shroom_samples.cpp). Preferred,
//                     because pitch, length and loudness are those of the console.
//   *.shi             ShroomTool's instrument set of 16 samples, as ShroomPlayer
//                     uses it: one sample per instrument, played faster for higher
//                     notes. Read when the song names a set of its own, or when the
//                     recordings are missing.
// They are looked for beside the song and in ../share/ beside the program.

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <strings.h>
#include <unistd.h>

#include "self_path.h"
#include "engine.h"

namespace {

using namespace retro;

const int kInstruments = 15, kPitches = 13, kChannels = 3;
const int kNativeRate = 32000;          // the SNES's, and that of both sample sets
const int kFade = 1024;                 // samples at 32000 Hz over which a cut note fades
const double kPictures = 39375000.0 / 11 * 6 / (4 * (341 * 262 - 0.5));   // a second, NTSC SNES
const double kTempoUnit = 3291161.0;    // NTSC game; the PAL game has 3949393 and 50 pictures
const size_t kNotePos = 7 + 32 + 32 + 32;
const int kGridV2 = 96;
const int kLongest = 96 * 30;           // Mariopants' limit for version 3

const char* const kNames[kInstruments] = {
    "Mario", "Mushroom", "Yoshi", "Star", "Flower", "Game Boy", "Dog", "Cat",
    "Pig", "Swan", "Face", "Plane", "Boat", "Car", "Heart"};

// ShroomPlayer's semitones above the sample's own pitch, by note (1..13).
const int kSemitones[16] = {0, 1, 2, 4, 6, 7, 9, 11, 13, 14, 16, 18, 19, 21, 23, 25};

std::string share_dir() {
  const std::string exe = self_path();
  if (exe.empty()) return "";
  return dir_of(exe) + "../share/";
}

// Band-limited resampling with a windowed sinc. <step> is how far the reading moves
// in the input for each sample written.
class Resampler {
 public:
  explicit Resampler(double step) : step_(step), cutoff_(step > 1.0 ? 1.0 / step : 1.0) {
    half_ = static_cast<int>(std::ceil(kZeros / cutoff_));
  }

  std::vector<float> run(const int16_t* in, size_t count) {
    std::vector<float> out;
    if (!count) return out;
    const size_t frames = static_cast<size_t>(std::ceil(count / step_));
    out.resize(frames);
    for (size_t t = 0; t < frames; t++) {
      const double centre = t * step_;
      const long whole = static_cast<long>(std::floor(centre));
      const int phase = static_cast<int>(std::lround((centre - whole) * kPhases));
      const std::vector<float>& k = kernel(phase);   // taps for whole-half_+1 .. whole+half_
      double sum = 0;
      const long first = whole - half_ + 1;
      for (int i = 0; i < 2 * half_; i++) {
        const long at = first + i;
        if (at >= 0 && at < static_cast<long>(count)) sum += k[i] * in[at];
      }
      out[t] = static_cast<float>(sum);
    }
    return out;
  }

 private:
  static constexpr int kZeros = 16;       // zero crossings on each side
  static constexpr int kPhases = 4096;

  const std::vector<float>& kernel(int phase) {
    auto found = kernels_.find(phase);
    if (found != kernels_.end()) return found->second;
    std::vector<float> k(2 * half_);
    const double frac = static_cast<double>(phase) / kPhases;
    double total = 0;
    for (int i = 0; i < 2 * half_; i++) {
      const double x = (i - half_ + 1) - frac;          // distance from the centre, in input samples
      const double a = x * cutoff_;
      const double sinc = std::fabs(a) < 1e-9 ? 1.0 : std::sin(M_PI * a) / (M_PI * a);
      const double w = std::fabs(a) >= kZeros ? 0.0
          : 0.42 + 0.5 * std::cos(M_PI * a / kZeros) + 0.08 * std::cos(2 * M_PI * a / kZeros);
      k[i] = static_cast<float>(sinc * w);
      total += k[i];
    }
    if (total != 0)
      for (float& v : k) v = static_cast<float>(v / total);
    return kernels_.emplace(phase, std::move(k)).first->second;
  }

  double step_, cutoff_;
  int half_;
  std::map<int, std::vector<float>> kernels_;
};

// Where the sound of a note comes from: samples at 32000 Hz and how fast to play them.
struct Source {
  const int16_t* data = nullptr;
  size_t count = 0;
  double speed = 1.0;
};

class SampleSet {
 public:
  std::string what;    // for the comment

  // Mariopants' recordings: one for each instrument and pitch.
  bool load_notes(const std::string& path) {
    const std::vector<uint8_t> f = read_file(path);
    const size_t wanted = kInstruments * kPitches;
    if (f.size() < 20 + 8 * wanted || std::memcmp(f.data(), "SHRONOTE", 8) != 0) return false;
    if (le32(&f[8]) != 1 || le32(&f[12]) != static_cast<unsigned>(kNativeRate) ||
        le32(&f[16]) != wanted)
      return false;
    const size_t base = 20 + 8 * wanted;
    const size_t have = (f.size() - base) / 2;
    pcm_.resize(have);
    for (size_t i = 0; i < have; i++)
      pcm_[i] = static_cast<int16_t>(f[base + 2 * i] | f[base + 2 * i + 1] << 8);
    for (int inst = 0; inst < kInstruments; inst++) {
      for (int pitch = 0; pitch < kPitches; pitch++) {
        const uint8_t* e = &f[20 + 8 * (inst * kPitches + pitch)];
        const size_t offset = le32(e), count = le32(e + 4);
        if (offset > have || count > have - offset) return false;
        Source& s = sources_[inst][pitch];
        s.data = pcm_.data() + offset;
        s.count = count;
        s.speed = 1.0;
      }
    }
    what = "Mariopants' recordings of the game";
    return true;
  }

  // ShroomTool's instrument set: "shri", 16 samples, the 13th instrument has two
  // (one for the notes below the 7th, one for those from it on).
  bool load_shi(const std::string& path, std::string& error) {
    const std::vector<uint8_t> f = read_file(path);
    if (f.size() < 512 || std::memcmp(f.data(), "shri", 4) != 0) {
      error = "not an instrument set of ShroomTool (.shi)";
      return false;
    }
    if (le16(&f[4]) != 1 || f[6] != 0) {
      error = "an instrument set (.shi) of a version that is not known";
      return false;
    }
    struct Entry { size_t offset, count; };
    Entry entries[16];
    size_t total = 0;
    for (int i = 0; i < 16; i++) {
      const uint8_t* e = &f[80 + 16 * i];
      const size_t bytes = le32(e), bits = le32(e + 4), rate = le32(e + 8), offset = le32(e + 12);
      // ShroomTool writes 32004 for every sample, whatever its rate; ShroomPlayer wants 32 kHz
      if (bits != 16 || rate < 31000 || rate > 33000 || offset < 512 || offset > f.size()) {
        error = "an instrument set (.shi) with samples that are not 16 bit and 32 kHz";
        return false;
      }
      size_t count = bytes / 2;
      if (count > (f.size() - offset) / 2) count = (f.size() - offset) / 2;   // the last one is cut short
      entries[i] = {offset, count};
      total += count;
    }
    pcm_.resize(total);
    size_t at = 0;
    for (int inst = 0; inst < kInstruments; inst++) {
      for (int pitch = 0; pitch < kPitches; pitch++) {
        int sample = inst;
        if (inst > 12 || (inst == 12 && pitch + 1 >= 7)) sample++;
        sources_[inst][pitch].speed = std::pow(2.0, (kSemitones[pitch + 1] - 1) / 12.0);
        sources_[inst][pitch].count = entries[sample].count;
        sources_[inst][pitch].data = nullptr;   // set below
      }
    }
    for (int i = 0; i < 16; i++) {
      for (size_t k = 0; k < entries[i].count; k++)
        pcm_[at + k] = static_cast<int16_t>(f[entries[i].offset + 2 * k] |
                                            f[entries[i].offset + 2 * k + 1] << 8);
      for (int inst = 0; inst < kInstruments; inst++)
        for (int pitch = 0; pitch < kPitches; pitch++) {
          int sample = inst;
          if (inst > 12 || (inst == 12 && pitch + 1 >= 7)) sample++;
          if (sample == i) sources_[inst][pitch].data = pcm_.data() + at;
        }
      at += entries[i].count;
    }
    const std::string name = field(&f[7], 32), author = field(&f[39], 32);
    what = "ShroomTool's instrument set";
    if (!name.empty()) what += " \"" + from_cp437(name) + "\"";
    if (!author.empty()) what += " by " + from_cp437(author);
    return true;
  }

  const Source& source(int inst, int pitch) const { return sources_[inst][pitch]; }

 private:
  std::vector<int16_t> pcm_;
  Source sources_[kInstruments][kPitches];
};

struct Song {
  std::vector<uint8_t> notes;   // 6 bytes a beat: note and instrument of three channels
  int length = 0;               // in beats
  int tempo = 0;
  bool loop = false;
  int version = 0;
  std::string title, author, set;

  bool note(int beat, int channel, int& pitch, int& inst) const {
    const uint8_t n = notes[beat * 6 + channel * 2], i = notes[beat * 6 + channel * 2 + 1];
    if (n < 1 || n > kPitches || i >= kInstruments) return false;
    pitch = n - 1;
    inst = i;
    return true;
  }
};

bool parse(const std::vector<uint8_t>& f, Song& song, std::string& error) {
  if (f.size() < 7 || std::memcmp(f.data(), "shro", 4) != 0) {
    error = "not a Shroom song (no \"shro\" at the start)";
    return false;
  }
  song.version = le16(&f[4]);
  if (song.version != 2 && song.version != 3) {
    error = "a Shroom song of version " + std::to_string(song.version) + ", only 2 and 3 are known";
    return false;
  }
  if (f[6] != 0) {
    error = "a compressed Shroom song, which no program is known to write";
    return false;
  }
  if (f.size() < kNotePos + 5) {
    error = "the file is too short for a Shroom song";
    return false;
  }
  song.title = from_cp437(field(&f[7], 32));
  song.author = from_cp437(field(&f[39], 32));
  song.set = field(&f[71], 32);
  bool lengthKnown = true;
  if (song.version == 2) {
    if (f.size() < kNotePos + 577) {
      error = "the file is too short for a Shroom song";
      return false;
    }
    song.notes.assign(f.begin() + kNotePos, f.begin() + kNotePos + 576);
    song.tempo = f[kNotePos + 576];
    if (f.size() >= 683) {          // what Mariopants adds
      song.length = f[680];
      song.loop = f[681] != 0;
      if (song.length < 1) song.length = 1;
      if (song.length > kGridV2) song.length = kGridV2;
    } else {
      song.length = kGridV2;
      lengthKnown = false;
    }
  } else {
    song.length = le16(&f[kNotePos]);
    song.loop = f[kNotePos + 2] != 0;
    song.tempo = f[kNotePos + 4];
    if (song.length < 1 || song.length > kLongest) {
      error = "a Shroom song with a length that can't be";
      return false;
    }
    if (f.size() < kNotePos + 5 + 6 * static_cast<size_t>(song.length)) {
      error = "the file is shorter than the song it holds";
      return false;
    }
    song.notes.assign(f.begin() + kNotePos + 5, f.begin() + kNotePos + 5 + 6 * song.length);
  }
  if (song.tempo > 0x9f) song.tempo = 0x9f;   // the fastest the game has

  // Anything that is neither a note nor the mark of an empty place means that this
  // isn't such a song after all.
  int last = -1, strange = 0;
  for (int beat = 0; beat < song.length; beat++) {
    for (int ch = 0; ch < kChannels; ch++) {
      int pitch, inst;
      if (song.note(beat, ch, pitch, inst)) last = beat;
      else if (song.notes[beat * 6 + ch * 2] != 0xff || song.notes[beat * 6 + ch * 2 + 1] != 0xdf)
        strange++;
    }
  }
  if (strange > song.length * kChannels / 4) {
    error = "the notes of this Shroom song make no sense";
    return false;
  }
  if (last < 0) {
    error = "a Shroom song without a single note";
    return false;
  }
  if (!lengthKnown) song.length = last + 1;
  return true;
}

class ShroomEngine : public Engine {
 public:
  bool open(const std::vector<uint8_t>& file, const std::string& path, int, int rate,
            SongInfo& info, std::string& error) override {
    if (!parse(file, song_, error)) return false;
    if (!find_samples(path, error)) return false;
    rate_ = rate;

    // every note the song plays, at the rate of the output
    std::map<double, std::unique_ptr<Resampler>> resamplers;
    for (int beat = 0; beat < song_.length; beat++) {
      for (int ch = 0; ch < kChannels; ch++) {
        int pitch, inst;
        if (!song_.note(beat, ch, pitch, inst) || sounds_.count(inst * kPitches + pitch)) continue;
        const Source& src = set_.source(inst, pitch);
        const double step = src.speed * kNativeRate / rate_;
        auto& r = resamplers[step];
        if (!r) r.reset(new Resampler(step));
        sounds_[inst * kPitches + pitch] = r->run(src.data, src.count);
      }
    }

    // for comparing with Mariopants: its beat, in samples at 32000 Hz
    const char* timing = std::getenv("BBMP_SHROOM_TIMING");
    if (timing && std::strcmp(timing, "mariopants") == 0) {
      const long beat32 = static_cast<long>(690892.8 / (14 + song_.tempo));
      beat_ = static_cast<double>(beat32) * rate_ / kNativeRate;
    }
    fade_ = kFade * rate_ / kNativeRate;
    if (fade_ < 1) fade_ = 1;

    // the song is over when its last beat is, and the notes have died away
    long long end = beat_start(song_.length);
    for (int beat = 0; beat < song_.length; beat++) {
      for (int ch = 0; ch < kChannels; ch++) {
        int pitch, inst;
        if (!song_.note(beat, ch, pitch, inst)) continue;
        const long long stop = beat_start(beat) +
            static_cast<long long>(sounds_[inst * kPitches + pitch].size());
        if (stop > end) end = stop;
      }
    }
    total_ = end;

    info.tracks = 1;
    info.title = song_.title;
    info.author = song_.author;
    info.type = "Shroom v" + std::to_string(song_.version);
    info.duration_ms = total_ * 1000 / rate_;
    info.comment = "Mario Paint, tempo " + std::to_string(song_.tempo) + ", " +
                   std::to_string(song_.length) + " beats" +
                   (song_.loop ? ", repeating" : "") + ". Instruments: " + set_.what + ".";
    bool used[kInstruments] = {};
    for (const auto& s : sounds_) used[s.first / kPitches] = true;
    for (int i = 0; i < kInstruments; i++)
      if (used[i]) info.names.push_back(kNames[i]);
    return true;
  }

  size_t render(int16_t* out, size_t frames) override {
    size_t done = 0;
    while (done < frames && frame_ < total_) {
      while (next_ < song_.length && frame_ >= beat_start(next_)) start_beat(next_++);
      long long until = total_;
      if (next_ < song_.length && beat_start(next_) < until) until = beat_start(next_);
      size_t n = frames - done;
      if (static_cast<long long>(n) > until - frame_) n = static_cast<size_t>(until - frame_);
      mix(out + 2 * done, n);
      done += n;
      frame_ += n;
    }
    return done;
  }

 private:
  struct Voice {
    const std::vector<float>* now = nullptr;
    size_t at = 0;
    const std::vector<float>* old = nullptr;   // the note that was cut, fading
    size_t oldAt = 0;
    long fade = 0;
  };

  // The frame at which a beat begins.
  long long beat_start(int beat) const {
    if (beat_ > 0) return std::llround(beat * beat_);
    // the picture at which the game's counter has overflowed <beat> times
    const double step = (song_.tempo + 14) * kTempoUnit;
    const double pictures = std::ceil(beat * 4294967296.0 / step - 1e-9);
    return std::llround(pictures * rate_ / kPictures);
  }

  bool find_samples(const std::string& path, std::string& error) {
    const std::string share = share_dir();
    const bool usual = song_.set.empty() || strcasecmp(song_.set.c_str(), "default.shi") == 0;
    std::string why;
    if (usual) {
      // the game's own instruments: the recordings first, where the player says they are
      if (const char* notes = std::getenv("BBMP_SHROOM_NOTES"))
        if (*notes && set_.load_notes(notes)) return true;
      const std::string beside = find_beside(path, "shroom-notes.bin");
      if (!beside.empty() && set_.load_notes(beside)) return true;
      if (!share.empty() && set_.load_notes(share + "shroom-notes.bin")) return true;
      const std::string shi = find_beside(path, "default.shi");
      if (!shi.empty() && set_.load_shi(shi, why)) return true;
      if (!share.empty() && !read_file(share + "default.shi").empty() &&
          set_.load_shi(share + "default.shi", why))
        return true;
      error = "Mario Paint's instrument sounds are missing: put notes.wav of Mariopants 1.1 "
              "into the player's extras folder (modland extras says where)";
      return false;
    }
    // A set of the song's own. The name must be a plain file name.
    if (song_.set.find('/') != std::string::npos || song_.set.find('\\') != std::string::npos) {
      error = "the song asks for an instrument set outside its folder";
      return false;
    }
    const std::string shi = find_beside(path, song_.set);
    if (shi.empty()) {
      error = "the song's instrument set is missing: " + from_cp437(song_.set);
      return false;
    }
    if (!set_.load_shi(shi, error)) return false;
    return true;
  }

  void start_beat(int beat) {
    for (int ch = 0; ch < kChannels; ch++) {
      int pitch, inst;
      if (!song_.note(beat, ch, pitch, inst)) continue;
      Voice& v = voices_[ch];
      v.old = v.now;
      v.oldAt = v.at;
      v.fade = fade_;
      v.now = &sounds_[inst * kPitches + pitch];
      v.at = 0;
    }
  }

  void mix(int16_t* out, size_t frames) {
    for (size_t i = 0; i < frames; i++) {
      float sum = 0;
      for (Voice& v : voices_) {
        if (v.now) {
          if (v.at < v.now->size()) sum += (*v.now)[v.at++];
          else v.now = nullptr;
        }
        if (v.old) {
          if (v.oldAt < v.old->size() && v.fade > 0) {
            sum += (*v.old)[v.oldAt++] * static_cast<float>(v.fade) / static_cast<float>(fade_);
            v.fade--;
          } else {
            v.old = nullptr;
          }
        }
      }
      long s = std::lround(sum / kChannels);
      if (s > 32767) s = 32767;
      if (s < -32768) s = -32768;
      out[2 * i] = out[2 * i + 1] = static_cast<int16_t>(s);
    }
  }

  Song song_;
  SampleSet set_;
  std::map<int, std::vector<float>> sounds_;   // by instrument * 13 + pitch
  Voice voices_[kChannels];
  int rate_ = 48000;
  double beat_ = 0;         // frames; 0 for the game's own timing
  long fade_ = 1;
  long long total_ = 0, frame_ = 0;
  int next_ = 0;            // the beat that begins next
};

}  // namespace

REGISTER_ENGINE("shroom", ShroomEngine)
