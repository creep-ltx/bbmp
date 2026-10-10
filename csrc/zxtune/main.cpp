// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-zxtune: play one track of a file with ZXTune, as raw PCM on stdout.
//
//   bbmp-zxtune <file> <track> <samplerate> [start-ms]
//
// The first line of output is JSON describing the file and track; after it comes
// signed 16-bit little-endian stereo audio until the track ends. Running as a
// separate process keeps an emulator crash from taking the player down with it.

#include "core/additional_files_resolve.h"
#include "core/data_location.h"
#include "core/plugin.h"
#include "core/service.h"
#include "io/api.h"
#include "module/additional_files.h"
#include "module/attributes.h"
#include "module/conversion/api.h"
#include "module/conversion/parameters.h"
#include "module/players/xsf/psf_bios.h"
#include "module/properties/path.h"
#include "parameters/container.h"
#include "parameters/merged_container.h"
#include "sound/chunk.h"
#include "tools/progress_callback.h"

#include "error_tools.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
  class FilesNextTo : public Module::AdditionalFilesSource
  {
  public:
    FilesNextTo(const Parameters::Accessor& params, String dir)
      : Params(params)
      , Dir(std::move(dir))
    {}

    Binary::Container::Ptr Get(StringView name) const override
    {
      return IO::OpenData(Dir + String(name), Params, Log::ProgressCallback::Stub());
    }

  private:
    const Parameters::Accessor& Params;
    const String Dir;
  };

  struct Track
  {
    String Subpath;
    Module::Holder::Ptr Holder;
  };

  class Collect : public Module::DetectCallback
  {
  public:
    Collect(Parameters::Accessor::Ptr params, IO::Identifier::Ptr id)
      : Params(std::move(params))
      , Id(std::move(id))
    {}

    Parameters::Container::Ptr CreateInitialProperties(StringView subpath) const override
    {
      return Parameters::CreateMergedContainer(Module::CreatePathProperties(Id->WithSubpath(subpath)),
                                               Parameters::Container::Create());
    }

    void ProcessModule(const ZXTune::DataLocation& location, const ZXTune::Plugin&,
                       Module::Holder::Ptr holder) override
    {
      if (location.GetPath()->Empty())
      {
        if (const auto* files = dynamic_cast<const Module::AdditionalFiles*>(holder.get()))
        {
          const auto& full = Id->Full();
          const FilesNextTo source(*Params, full.substr(0, full.size() - Id->Filename().size()));
          Module::ResolveAdditionalFiles(source, *files);
        }
      }
      Tracks.push_back({location.GetPath()->AsString(), std::move(holder)});
    }

    Log::ProgressCallback* GetProgress() const override
    {
      return nullptr;
    }

    std::vector<Track> Tracks;

  private:
    const Parameters::Accessor::Ptr Params;
    const IO::Identifier::Ptr Id;
  };

  String Json(StringView s)
  {
    String out = "\"";
    for (unsigned char c : s)
    {
      if (c == '"' || c == '\\')
      {
        out += '\\';
        out += static_cast<char>(c);
      }
      else if (c < 0x20)
      {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      }
      else
      {
        out += static_cast<char>(c);
      }
    }
    return out + "\"";
  }

  String Attr(const Parameters::Accessor& props, StringView name)
  {
    Parameters::StringType value;
    Parameters::FindValue(props, Parameters::Identifier(name), value);
    return value;
  }
}  // namespace

// --aydump <file> <track>: the AY chip's registers frame by frame instead of sound, for
// checking a replayer against the original player (tools/verify): 14 bytes a frame,
// registers 0 to 13, register 13 0xff in a frame that doesn't write it.
static int AyDump(const std::string& path, std::size_t track, bool text)
{
  const auto params = Parameters::Container::Create();
  const auto service = ZXTune::Service::Create(params);
  const auto id = IO::ResolveUri(path);
  Collect found(params, id);
  service->DetectModules(IO::OpenData(id->Path(), *params, Log::ProgressCallback::Stub()), found);
  if (track >= found.Tracks.size())
  {
    std::fprintf(stderr, "no such track\n");
    return 1;
  }
  const Module::Conversion::AYDumpConvertParam spec(0);     // every frame, not optimized
  const Module::Conversion::TXTConvertParam asText;
  const auto dump = text ? Module::Convert(*found.Tracks[track].Holder, asText, params)
                         : Module::Convert(*found.Tracks[track].Holder, spec, params);
  if (!dump)
  {
    std::fprintf(stderr, "not an AY song\n");
    return 1;
  }
  return std::fwrite(dump->Start(), 1, dump->Size(), stdout) == dump->Size() ? 0 : 1;
}

int main(int argc, char* argv[])
{
  // --vt: the song as Vortex Tracker II text, as ZXTune reads it (Pro Tracker songs)
  if (argc == 4 && (std::string(argv[1]) == "--aydump" || std::string(argv[1]) == "--vt"))
  {
    try
    {
      return AyDump(argv[2], std::strtoul(argv[3], nullptr, 10), std::string(argv[1]) == "--vt");
    }
    catch (const Error& e)
    {
      std::fprintf(stderr, "%s\n", e.ToString().c_str());
      return 1;
    }
  }
  if (argc < 4)
  {
    std::fprintf(stderr, "usage: %s <file> <track> <samplerate> [start-ms]\n", argv[0]);
    return 2;
  }
  const std::string path = argv[1];
  const std::size_t track = std::strtoul(argv[2], nullptr, 10);
  const uint_t rate = std::strtoul(argv[3], nullptr, 10);
  const uint_t startMs = argc > 4 ? std::strtoul(argv[4], nullptr, 10) : 0;
  try
  {
    const auto params = Parameters::Container::Create();
    const auto service = ZXTune::Service::Create(params);
    const auto id = IO::ResolveUri(path);
    Collect found(params, id);
    service->DetectModules(IO::OpenData(id->Path(), *params, Log::ProgressCallback::Stub()), found);
    if (track >= found.Tracks.size())
    {
      std::printf("{\"tracks\":%zu,\"error\":\"no such track\"}\n", found.Tracks.size());
      return 1;
    }
    const auto& holder = *found.Tracks[track].Holder;
    const auto props = holder.GetModuleProperties();
    // PlayStation songs are played with Sony's BIOS, which the user provides (BBMP_PS2_BIOS)
    const auto type = Attr(*props, Module::ATTR_TYPE);
    if (type == "PSF" || type == "PSF2")
    {
      const auto problem = Module::PSF::BiosProblem();
      if (!problem.empty())
      {
        std::printf("{\"tracks\":0,\"error\":%s}\n", Json(problem).c_str());
        return 1;
      }
    }
    const auto info = holder.GetModuleInformation();
    std::printf("{\"tracks\":%zu,\"track\":%zu,\"subpath\":%s,\"title\":%s,\"author\":%s,\"program\":%s,"
                "\"type\":%s,\"duration_ms\":%llu,\"facts\":{\"Computer\":%s,\"Date\":%s},"
                "\"comment\":%s}\n",
                found.Tracks.size(), track, Json(found.Tracks[track].Subpath).c_str(),
                Json(Attr(*props, Module::ATTR_TITLE)).c_str(), Json(Attr(*props, Module::ATTR_AUTHOR)).c_str(),
                Json(Attr(*props, Module::ATTR_PROGRAM)).c_str(), Json(Attr(*props, Module::ATTR_TYPE)).c_str(),
                static_cast<unsigned long long>(info.Duration.Get()),
                Json(Attr(*props, Module::ATTR_COMPUTER)).c_str(), Json(Attr(*props, Module::ATTR_DATE)).c_str(),
                Json(Attr(*props, Module::ATTR_COMMENT)).c_str());
    std::fflush(stdout);
    const auto renderer = holder.CreateRenderer(rate, props);
    if (startMs)
    {
      renderer->SetPosition(Time::AtMillisecond(startMs));
    }
    // Most of these songs loop, and the renderer plays them round and round. The
    // player stops at the song's length; this is for everyone else, so that a
    // rendering to a file ends: a little past the length, or after half an hour
    // when the song doesn't say how long it is.
    const unsigned long long lengthMs = info.Duration.Get();
    const unsigned long long limitMs = lengthMs ? lengthMs + 10000 : 30 * 60 * 1000;
    unsigned long long frames = static_cast<unsigned long long>(startMs) * rate / 1000;
    for (;;)
    {
      const auto chunk = renderer->Render();
      if (chunk.empty() || frames * 1000 / rate >= limitMs)
      {
        break;
      }
      frames += chunk.size();
      // Sound::Sample packs left in the low 16 bits and right in the high 16 bits: s16le stereo
      if (std::fwrite(chunk.data(), sizeof(chunk.front()), chunk.size(), stdout) != chunk.size())
      {
        break;
      }
    }
    return 0;
  }
  catch (const Error& e)
  {
    std::printf("{\"tracks\":0,\"error\":%s}\n", Json(e.ToString()).c_str());
  }
  catch (const std::exception& e)
  {
    std::printf("{\"tracks\":0,\"error\":%s}\n", Json(e.what()).c_str());
  }
  return 1;
}
