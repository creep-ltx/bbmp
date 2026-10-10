// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-ay-at3: Starkos (.sks) and Arkos Tracker (.aks) songs, played by Arkos
// Tracker 3's own loader, player and PSG emulation (by Julien Nevo, MIT licence).
//
//   bbmp-ay-at3 <engine> <file> <track> <samplerate> [start-ms]
//
// Engines: sks (Starkos), aks (Arkos Tracker 1, 2 and 3). Arkos Tracker reads more
// formats than these; they are refused here, as they are imports into another
// tracker's model and other helpers play them as they are.
//
// On stdout: one line of JSON about the song, then s16le stereo at the rate asked
// for until the song has played once. Everything else goes to stderr.

#include "../../audio/sources/PsgStreamGenerator.h"
#include "../../audio/sources/PsgsProcessor.h"
#include "../../business/song/tool/frameCounter/FrameCounter.h"
#include "../../controllers/model/OutputMix.h"
#include "../../import/loader/SongLoader.h"
#include "../../player/SongPlayer.h"
#include "../../song/Song.h"

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

FILE* g_out = stdout;

std::string json(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
    else if (c < 0x20 || c == 0x7F) { char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
    else out += static_cast<char>(c);
  }
  return out + "\"";
}

std::string text(const juce::String& s) {
  return s.trim().toStdString();     // UTF-8
}

int fail(const std::string& why) {
  std::fprintf(g_out, "{\"tracks\":0,\"error\":%s}\n", json(why).c_str());
  std::fflush(g_out);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace arkostracker;
  std::signal(SIGPIPE, SIG_IGN);
  std::fflush(stdout);
  const int real_stdout = dup(STDOUT_FILENO);
  if (real_stdout >= 0 && dup2(STDERR_FILENO, STDOUT_FILENO) >= 0) {
    if (FILE* f = fdopen(real_stdout, "wb")) g_out = f;
  }
  if (argc < 5) return fail("usage: bbmp-ay-at3 <sks|aks> <file> <track> <samplerate> [start-ms]");
  const std::string engine = argv[1];
  const int track = std::atoi(argv[3]);
  const int rate = std::atoi(argv[4]);
  const long long startMs = argc > 5 ? std::atoll(argv[5]) : 0;
  if (engine != "sks" && engine != "aks") return fail("no engine named " + engine);
  if (rate < 8000 || rate > 192000) return fail("the sample rate must be 8000 to 192000");
  if (track < 0) return fail("no such track");

  const juce::ScopedJuceInitialiser_GUI juceInit;

  const juce::File file = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[2]));
  if (!file.existsAsFile()) return fail("the file can't be read");
  // Arkos Tracker goes by the file's content, but make sure of the simple cases first.
  std::string sksComment;
  if (engine == "sks") {
    juce::FileInputStream in(file);
    char head[128 + 52] = {0};
    const auto got = in.openedOk() ? in.read(head, sizeof(head)) : 0;
    // the song may have the 128 bytes of an AMSDOS header before it
    int at = -1;
    if (got >= 52 && std::memcmp(head, "STK1.0SONG", 10) == 0) at = 0;
    else if (got >= 128 + 52 && std::memcmp(head + 128, "STK1.0SONG", 10) == 0) at = 128;
    if (at < 0) return fail("not a Starkos song");
    for (int i = 0; i < 32; ++i) {
      const unsigned char c = static_cast<unsigned char>(head[at + 20 + i]);
      sksComment += c >= 0x20 && c < 0x7F ? static_cast<char>(c) : ' ';
    }
    while (!sksComment.empty() && sksComment.back() == ' ') sksComment.pop_back();
  }

  SongLoader loader;
  const auto result = loader.loadSong(file, false);
  if (result == nullptr || result->status != SongLoader::ImportStatus::ok || result->song == nullptr)
    return fail(engine == "sks" ? "Arkos Tracker can't read this Starkos song" : "not an Arkos Tracker song");
  const auto format = result->format;
  std::string type;
  if (engine == "sks") {
    if (format != ImportedFormat::starkos) return fail("not a Starkos song");
    type = "Starkos";
  } else {
    if (format == ImportedFormat::native) type = "Arkos Tracker 3";
    else if (format == ImportedFormat::at2) type = "Arkos Tracker 2";
    else if (format == ImportedFormat::at1) type = "Arkos Tracker 1";
    else return fail("not an Arkos Tracker song");
  }
  if (result->errorReport != nullptr) {
    for (int i = 0; i < result->errorReport->getLineCount(); ++i)
      std::fprintf(stderr, "bbmp-ay-at3: %s\n", result->errorReport->getLine(i).getText().toRawUTF8());
    if (result->errorReport->getErrorCount() > 0) return fail("Arkos Tracker found errors in the song");
  }
  const std::shared_ptr<Song> song = std::move(result->song);

  const int tracks = song->getSubsongCount();
  if (tracks < 1) return fail("the song has no subsong");
  if (track >= tracks) {
    std::fprintf(g_out, "{\"tracks\":%d,\"error\":\"no such track\"}\n", tracks);
    return 1;
  }
  const auto subsongIdOptional = song->getSubsongId(track);
  if (subsongIdOptional.isAbsent()) return fail("no such track");
  const auto subsongId = subsongIdOptional.getValue();

  const auto [frames, upTo, loopFrame] = FrameCounter::count(*song, subsongId);
  (void)upTo;
  (void)loopFrame;
  const auto metadata = song->getSubsongMetadata(subsongId);
  const auto psgs = song->getSubsongPsgs(subsongId);
  const auto replayHz = metadata.getReplayFrequencyHz();
  if (frames < 1 || !(replayHz > 1.0F) || psgs.empty()) return fail("the song is empty");
  const long long total = static_cast<long long>(std::llround(static_cast<double>(frames) * rate / static_cast<double>(replayHz)));

  std::string names;
  song->performOnConstInstruments([&names](const std::vector<std::unique_ptr<Instrument>>& instruments) {
    bool first = true;
    for (const auto& instrument : instruments) {
      if (first) { first = false; continue; }          // instrument 0 is the empty one
      if (!names.empty()) names += ',';
      names += json(text(instrument->getName()));
    }
  });
  std::string title = text(song->getTitle());
  std::string comment = text(song->getComments());
  if (engine == "sks") {            // the importer puts its own words there
    title.clear();
    comment = sksComment;
  }
  if (tracks > 1) {
    const auto name = text(metadata.getName());
    if (!name.empty()) title += title.empty() ? name : " - " + name;
  }
  std::string author = text(song->getAuthor());
  const std::string composer = text(song->getComposer());
  if (author.empty()) author = composer;

  std::fprintf(g_out, "{\"tracks\":%d,\"track\":%d,\"title\":%s,\"author\":%s,\"type\":%s,"
               "\"duration_ms\":%lld,\"comment\":%s,\"names\":[%s]}\n",
               tracks, track, json(title).c_str(), json(author).c_str(), json(type).c_str(),
               total * 1000 / rate, json(comment).c_str(), names.c_str());
  std::fflush(g_out);

  // As Arkos Tracker's export to WAV does it (export/wav/SongWavExporter.cpp).
  const OutputMix outputMix(100, 100, 100, 100, false, 50);
  SongPlayer songPlayer(song);
  songPlayer.setOfflineSongEndCountBeforeMuting(1);
  const auto startLocation = Location(subsongId, 0);
  const auto [loopStartLocation, pastEndLocation] = song->getLoopStartAndPastEndPositions(subsongId);
  songPlayer.play(startLocation, loopStartLocation, pastEndLocation, true, true);

  PsgsProcessor psgsProcessor;
  psgsProcessor.setOutputMix(outputMix);
  std::vector<std::unique_ptr<PsgStreamGenerator>> generators;
  int psgIndex = 0;
  for (const auto& psg : psgs) {
    auto generator = std::make_unique<PsgStreamGenerator>(songPlayer, psg.getType(), psgIndex, replayHz, psg.getPsgFrequency(),
                                                          psg.getSamplePlayerFrequency(), psg.getPsgMixingOutput(),
                                                          1.0, 1.0, 1.0, metadata.getSidPlayerCapability());
    psgsProcessor.addInputSource(generator.get(), false);
    generators.push_back(std::move(generator));
    ++psgIndex;
  }

  const int block = 1024;
  juce::AudioSampleBuffer buffer(2, block);
  buffer.clear();
  juce::AudioSourceChannelInfo info(buffer);
  psgsProcessor.prepareToPlay(block, static_cast<double>(rate));

  long long skip = startMs > 0 ? startMs * rate / 1000 : 0;
  long long done = 0;
  std::vector<int16_t> pcm(block * 2);
  // takes away what is constant in the signal (the PSG's output is one-sided)
  double mean[2] = {0, 0};
  const double meanRate = 1.0 / (rate * 0.05);
  const double gain = 1.0 / std::sqrt(static_cast<double>(psgs.size()));
  bool alive = true;
  while (alive && done < total) {
    info.clearActiveBufferRegion();
    psgsProcessor.getNextAudioBlock(info);
    const int n = static_cast<int>(std::min<long long>(block, total - done));
    for (int c = 0; c < 2; ++c) {
      const float* in = buffer.getReadPointer(c);
      for (int i = 0; i < n; ++i) {
        const double x = in[i];
        mean[c] += (x - mean[c]) * meanRate;
        const long s = std::lrint((x - mean[c]) * gain * 32767.0);
        pcm[static_cast<size_t>(i * 2 + c)] = static_cast<int16_t>(std::clamp(s, -32768L, 32767L));
      }
    }
    done += n;
    long long from = 0;
    if (skip > 0) {
      from = std::min<long long>(skip, n);
      skip -= from;
    }
    const auto count = static_cast<size_t>(n - from);
    if (count > 0 && std::fwrite(pcm.data() + from * 2, 4, count, g_out) != count) alive = false;
    if (songPlayer.hasOfflineSongEndCountReached() && done < total) {
      // the player is at the end earlier than the count of frames says: stop there
      break;
    }
  }
  std::fflush(g_out);
  return 0;
}
