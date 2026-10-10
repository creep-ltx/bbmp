// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-ay-vt2: Vortex Tracker II text modules (.vt2) as Vortex Tracker 2.5 and
// later write them, played by ZXTune's Pro Tracker 3 player.
//
//   bbmp-ay-vt2 vt2 <file> <track> <samplerate> [start-ms]
//
// ZXTune reads the text format as Vortex Tracker II 1.0 wrote it and refuses these
// files for two reasons: they have two empty lines where it allows one, and their
// samples write a mask that is off as "." where it wants "t", "n" or "e". Here the
// text is rewritten the old way and given to ZXTune. The rest of what is new is
// looked after here as well:
//   ChipFreq, IntFreq   ZXTune plays every module at 50 interrupts per second. It
//                       is asked for a sample rate and a chip clock that are both
//                       50/IntFreq of the wanted ones, which comes to the same as
//                       the module's own rates when played at the wanted rate.
//   Noise=DEC           the patterns' noise column is rewritten in hex.
//   two [Module]s       a TurboSound song for two chips: both are played and mixed.
//
// On stdout: one line of JSON about the song, then s16le stereo at the rate asked
// for until the song has played once. Everything else goes to stderr.

#include "binary/container_factories.h"
#include "core/core_parameters.h"
#include "core/service.h"
#include "module/attributes.h"
#include "module/holder.h"
#include "parameters/container.h"
#include "parameters/merged_accessor.h"
#include "sound/chunk.h"

#include "error_tools.h"

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

namespace
{
  FILE* g_out = stdout;

  std::string Json(const std::string& s)
  {
    std::string out = "\"";
    for (unsigned char c : s)
    {
      if (c == '"' || c == '\\')
      {
        out += '\\';
        out += static_cast<char>(c);
      }
      else if (c < 0x20 || c == 0x7F)
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

  bool IsUtf8(const std::string& s)
  {
    for (std::size_t i = 0; i < s.size();)
    {
      const unsigned char c = s[i];
      const int more = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
      if (more < 0 || i + more >= s.size() + (more ? 0 : 1))
      {
        return false;
      }
      for (int k = 1; k <= more; ++k)
      {
        if ((static_cast<unsigned char>(s[i + k]) >> 6) != 2)
        {
          return false;
        }
      }
      i += more + 1;
    }
    return true;
  }

  void Put(std::string& out, unsigned code)
  {
    if (code < 0x80)
    {
      out += static_cast<char>(code);
    }
    else if (code < 0x800)
    {
      out += static_cast<char>(0xC0 | (code >> 6));
      out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else
    {
      out += static_cast<char>(0xE0 | (code >> 12));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    }
  }

  // Text that isn't UTF-8 is read as Windows-1251, the code page of most authors.
  std::string Text(const std::string& s)
  {
    if (IsUtf8(s))
    {
      return s;
    }
    std::string out;
    for (unsigned char c : s)
    {
      if (c < 0x80)
      {
        out += static_cast<char>(c);
      }
      else if (c >= 0xC0)
      {
        Put(out, 0x410 + (c - 0xC0));
      }
      else if (c == 0xA8)
      {
        Put(out, 0x401);
      }
      else if (c == 0xB8)
      {
        Put(out, 0x451);
      }
      else
      {
        out += '?';
      }
    }
    return out;
  }

  int Fail(const std::string& why)
  {
    std::fprintf(g_out, "{\"tracks\":0,\"error\":%s}\n", Json(why).c_str());
    std::fflush(g_out);
    return 1;
  }

  struct ModuleText
  {
    std::string Text;        // rewritten
    std::string Title, Author;
    long ChipFreq = 1750000;
    long IntFreq = 48828;    // in 1/1000 Hz
    bool Tracker2 = false;   // the file says VortexTrackerII=
  };

  std::vector<std::string> Lines(const std::string& data)
  {
    std::vector<std::string> lines;
    std::string line;
    for (std::size_t i = 0; i < data.size(); ++i)
    {
      const char c = data[i];
      if (c == '\r' || c == '\n')
      {
        if (c == '\r' && i + 1 < data.size() && data[i + 1] == '\n')
        {
          ++i;
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
        {
          line.pop_back();
        }
        lines.push_back(line);
        line.clear();
      }
      else
      {
        line += c;
      }
    }
    if (!line.empty())
    {
      lines.push_back(line);
    }
    return lines;
  }

  bool StartsWith(const std::string& s, const char* with)
  {
    return s.compare(0, std::strlen(with), with) == 0;
  }

  // The modules of the file, each rewritten as Vortex Tracker II 1.0 wrote them.
  std::vector<ModuleText> Rewrite(const std::string& data)
  {
    std::vector<ModuleText> result;
    std::string section;
    bool blank = false;
    bool decimalNoise = false;
    for (const auto& original : Lines(data))
    {
      std::string line = original;
      if (line == "[Module]")
      {
        result.emplace_back();
        blank = false;
        decimalNoise = false;
      }
      if (result.empty())
      {
        if (line.empty())
        {
          continue;
        }
        return {};            // something else comes first: not a module
      }
      ModuleText& mod = result.back();
      if (!line.empty() && line[0] == '[')
      {
        section = line;
      }
      if (line.empty())
      {
        if (blank)
        {
          continue;           // one empty line is what ZXTune allows
        }
        blank = true;
      }
      else
      {
        blank = false;
      }
      if (section == "[Module]")
      {
        if (StartsWith(line, "Title="))
        {
          mod.Title = line.substr(6);
        }
        else if (StartsWith(line, "Author="))
        {
          mod.Author = line.substr(7);
        }
        else if (StartsWith(line, "VortexTrackerII="))
        {
          mod.Tracker2 = true;
        }
        else if (StartsWith(line, "ChipFreq="))
        {
          mod.ChipFreq = std::atol(line.c_str() + 9);
          continue;
        }
        else if (StartsWith(line, "IntFreq="))
        {
          mod.IntFreq = std::atol(line.c_str() + 8);
          continue;
        }
        else if (StartsWith(line, "Noise="))
        {
          decimalNoise = line.substr(6) == "DEC";
          continue;
        }
        else if (StartsWith(line, "Colors="))
        {
          continue;
        }
      }
      else if (StartsWith(section, "[Sample") && line.size() > 3 && line[0] != '[')
      {
        static const char OFF[] = "tne";
        for (int i = 0; i < 3; ++i)
        {
          if (line[i] == '.')
          {
            line[i] = OFF[i];
          }
        }
      }
      else if (decimalNoise && StartsWith(section, "[Pattern") && line.size() > 7 && line[0] != '[' && line[4] == '|'
               && line[7] == '|')
      {
        // ....|NN|: the noise, 0 to 31
        if (std::isdigit(static_cast<unsigned char>(line[5])) && std::isdigit(static_cast<unsigned char>(line[6])))
        {
          const int value = (line[5] - '0') * 10 + (line[6] - '0');
          static const char HEX[] = "0123456789ABCDEF";
          line[5] = HEX[(value >> 4) & 15];
          line[6] = HEX[value & 15];
        }
      }
      mod.Text += line;
      mod.Text += '\n';
    }
    return result;
  }

  struct Playing
  {
    Module::Holder::Ptr Holder;
    Module::Renderer::Ptr Renderer;
    std::vector<Sound::Sample> Left;      // rendered and not yet taken
    std::size_t At = 0;

    // fills with up to count samples; fewer when the renderer gives no more
    std::size_t Take(Sound::Sample* to, std::size_t count)
    {
      std::size_t done = 0;
      while (done < count)
      {
        if (At >= Left.size())
        {
          auto chunk = Renderer->Render();
          if (chunk.empty())
          {
            break;
          }
          Left.assign(chunk.begin(), chunk.end());
          At = 0;
        }
        const std::size_t n = std::min(count - done, Left.size() - At);
        std::copy(Left.begin() + At, Left.begin() + At + n, to + done);
        At += n;
        done += n;
      }
      return done;
    }
  };
}  // namespace

int main(int argc, char* argv[])
{
  std::signal(SIGPIPE, SIG_IGN);
  std::fflush(stdout);
  const int realStdout = dup(STDOUT_FILENO);
  if (realStdout >= 0 && dup2(STDERR_FILENO, STDOUT_FILENO) >= 0)
  {
    if (FILE* f = fdopen(realStdout, "wb"))
    {
      g_out = f;
    }
  }
  if (argc < 5 || std::strcmp(argv[1], "vt2") != 0)
  {
    return Fail("usage: bbmp-ay-vt2 vt2 <file> <track> <samplerate> [start-ms]");
  }
  const long track = std::atol(argv[3]);
  const long rate = std::atol(argv[4]);
  const long long startMs = argc > 5 ? std::atoll(argv[5]) : 0;
  if (rate < 8000 || rate > 192000)
  {
    return Fail("the sample rate must be 8000 to 192000");
  }
  if (track != 0)
  {
    return Fail("no such track");
  }
  std::string data;
  {
    FILE* f = std::fopen(argv[2], "rb");
    if (!f)
    {
      return Fail("the file can't be read");
    }
    char buf[65536];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && data.size() < (16u << 20))
    {
      data.append(buf, n);
    }
    std::fclose(f);
  }
  const auto modules = Rewrite(data);
  if (modules.empty() || !modules.front().Tracker2)
  {
    return Fail("not a Vortex Tracker II text module");
  }
  if (modules.size() > 2)
  {
    return Fail("the file has more than two modules");
  }
  const ModuleText& first = modules.front();
  for (const auto& mod : modules)
  {
    if (mod.IntFreq < 10000 || mod.IntFreq > 400000 || mod.ChipFreq < 500000 || mod.ChipFreq > 4000000)
    {
      return Fail("the module's ChipFreq or IntFreq is out of range");
    }
    if (mod.IntFreq != first.IntFreq || mod.ChipFreq != first.ChipFreq)
    {
      return Fail("the two modules of the file have different rates");
    }
  }
  try
  {
    // see the top of the file
    const double scale = 50000.0 / static_cast<double>(first.IntFreq);
    const auto askedRate = static_cast<uint_t>(std::lround(rate * scale));
    const auto askedClock = static_cast<Parameters::IntType>(std::llround(first.ChipFreq * scale));
    if (askedClock < Parameters::ZXTune::Core::AYM::CLOCKRATE_MIN
        || askedClock > Parameters::ZXTune::Core::AYM::CLOCKRATE_MAX)
    {
      return Fail("the module's ChipFreq is out of range");
    }

    const auto params = Parameters::Container::Create();
    const auto service = ZXTune::Service::Create(params);
    std::vector<Playing> playing;
    unsigned long long longest = 0;      // in samples at the asked rate
    for (const auto& mod : modules)
    {
      const auto container = Binary::CreateContainer(Binary::View(mod.Text.data(), mod.Text.size()));
      Playing p;
      p.Holder = service->OpenModule(container, "", Parameters::Container::Create());
      const auto props = p.Holder->GetModuleProperties();
      Parameters::StringType type;
      Parameters::FindValue(*props, Parameters::Identifier(Module::ATTR_TYPE), type);
      if (type != "TXT")
      {
        return Fail("not a Vortex Tracker II text module");
      }
      const auto over = Parameters::Container::Create();
      over->SetValue(Parameters::ZXTune::Core::AYM::CLOCKRATE, askedClock);
      // the file doesn't say which chip; Vortex Tracker 2.5 plays a YM unless told otherwise
      over->SetValue(Parameters::ZXTune::Core::AYM::TYPE, Parameters::ZXTune::Core::AYM::TYPE_YM);
      p.Renderer = p.Holder->CreateRenderer(askedRate, Parameters::CreateMergedAccessor(over, props));
      const auto milli = static_cast<unsigned long long>(p.Holder->GetModuleInformation().Duration.Get());
      longest = std::max(longest, milli * askedRate / 1000ULL);
      playing.push_back(std::move(p));
    }
    if (longest == 0)
    {
      return Fail("the module has no length");
    }
    std::fprintf(g_out,
                 "{\"tracks\":1,\"track\":0,\"title\":%s,\"author\":%s,\"type\":%s,\"duration_ms\":%llu,"
                 "\"comment\":%s,\"names\":[]}\n",
                 Json(Text(first.Title)).c_str(), Json(Text(first.Author)).c_str(),
                 Json(modules.size() > 1 ? "Vortex Tracker II (TurboSound)" : "Vortex Tracker II").c_str(),
                 longest * 1000ULL / static_cast<unsigned long long>(rate),
                 Json(modules.size() > 1 ? Text(modules[1].Title) : std::string()).c_str());
    std::fflush(g_out);

    unsigned long long skip = startMs > 0 ? static_cast<unsigned long long>(startMs) * rate / 1000 : 0;
    const std::size_t BLOCK = 4096;
    std::vector<Sound::Sample> one(BLOCK);
    std::vector<int> mix(BLOCK * 2);
    std::vector<int16_t> pcm(BLOCK * 2);
    for (unsigned long long done = 0; done < longest;)
    {
      const auto want = static_cast<std::size_t>(std::min<unsigned long long>(BLOCK, longest - done));
      std::fill(mix.begin(), mix.end(), 0);
      std::size_t got = 0;
      for (auto& p : playing)
      {
        const std::size_t n = p.Take(one.data(), want);
        got = std::max(got, n);
        for (std::size_t i = 0; i < n; ++i)
        {
          mix[i * 2] += one[i].Left();
          mix[i * 2 + 1] += one[i].Right();
        }
      }
      if (got == 0)
      {
        break;
      }
      for (std::size_t i = 0; i < got * 2; ++i)
      {
        // two chips: 0.7 of each
        const int v = playing.size() > 1 ? mix[i] * 7 / 10 : mix[i];
        pcm[i] = static_cast<int16_t>(std::clamp(v, -32768, 32767));
      }
      done += got;
      std::size_t from = 0;
      if (skip > 0)
      {
        from = static_cast<std::size_t>(std::min<unsigned long long>(skip, got));
        skip -= from;
      }
      if (got > from && std::fwrite(pcm.data() + from * 2, 4, got - from, g_out) != got - from)
      {
        break;
      }
    }
    std::fflush(g_out);
    return 0;
  }
  catch (const Error& e)
  {
    return Fail("not a module ZXTune can read: " + e.ToString());
  }
  catch (const std::exception& e)
  {
    return Fail(std::string("not a module ZXTune can read: ") + e.what());
  }
}
