// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-psycle: plays a Psycle song (.psy, PSY2SONG and PSY3SONG) with Psycle's own
// player code and Psycle's native plugins.
//
//   bbmp-psycle <file> <track> <samplerate> [start-ms]
//
// One line of JSON about the song goes to stdout, then s16le stereo until the song
// ends. The engine is the host of Psycle for Windows (psycle.sourceforge.net, GPL 2
// or later), built without its windows; the native plugins are libraries next to the
// helper, in ../lib/psycle-plugins. VST, Lua and LADSPA machines are not there, nor
// are the few native plugins of which there is no source. A song that needs one of
// them is refused, unless what is missing is a small part of it: see judge().
//
// In the environment:
//   BBMP_PSYCLE_PLUGINS  the folder of the plugins, when it is somewhere else
//   BBMP_PSYCLE_MISSING  "play": play every song, whatever is missing
//   BBMP_PSYCLE_NEWBLITZ "1": songs that name "blitz.dll" or "gamefx.dll" get the
//                           newer of the two versions of those plugins
//   BBMP_PSYCLE_INFO     "1": the machines and wires of the song on stderr; "2": also
//                           the values of the plugins and how loud each machine was;
//                           "x": the machines, and nothing is played

#include <psycle/host/detail/project.private.hpp>
#include "host.hpp"

#include <psycle/host/FileIO.hpp>
#include <psycle/host/Machine.hpp>
#include <psycle/host/Player.hpp>
#include <psycle/host/Plugin.hpp>
#include <psycle/host/Song.hpp>
#include <psycle/host/internal_machines.hpp>
#include <psycle/host/player_plugins/winamp/fake_progressDialog.hpp>

#include <unistd.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using psycle::host::CProgressDialog;
using psycle::host::Dummy;
using psycle::host::Global;
using psycle::host::Host;
using psycle::host::Machine;
using psycle::host::Mixer;
using psycle::host::MACH_MIXER;
using psycle::host::OldPsyFile;
using psycle::host::PatternEntry;
using psycle::host::Player;
using psycle::host::Song;
using psycle::host::MACH_DUMMY;
using psycle::host::MACH_DUPLICATOR;
using psycle::host::MACH_DUPLICATOR2;
using psycle::host::MACH_PLUGIN;
using psycle::host::MAX_TRACKS;
namespace notecommands = psycle::host::notecommands;

namespace {

std::string json(const std::string& s) {
	std::string out = "\"";
	for (size_t i = 0; i < s.size(); ++i) {
		const unsigned char c = s[i];
		if (c == '"' || c == '\\') { out += '\\'; out += c; }
		else if (c == '\n') out += "\\n";
		else if (c == '\r') out += "\\r";
		else if (c == '\t') out += "\\t";
		else if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
		else out += c;
	}
	return out + "\"";
}

// Songs are written on Windows: what isn't UTF-8 is read as Windows-1252.
std::string utf8(const std::string& s) {
	bool ok = true;
	for (size_t i = 0; i < s.size() && ok;) {
		const unsigned char c = s[i];
		const int more = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
		if (more < 0 || i + more >= s.size() + (more ? 0 : 1)) { ok = false; break; }
		for (int k = 1; k <= more; ++k) ok = ok && (static_cast<unsigned char>(s[i + k]) >> 6) == 2;
		i += more + 1;
	}
	if (ok) return s;
	static const unsigned short high[32] = {
		0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
		0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};
	std::string out;
	for (size_t i = 0; i < s.size(); ++i) {
		const unsigned char c = s[i];
		unsigned u = c;
		if (c >= 0x80 && c < 0xA0) u = high[c - 0x80];
		if (u < 0x80) out += static_cast<char>(u);
		else if (u < 0x800) { out += static_cast<char>(0xC0 | (u >> 6)); out += static_cast<char>(0x80 | (u & 0x3F)); }
		else {
			out += static_cast<char>(0xE0 | (u >> 12));
			out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (u & 0x3F));
		}
	}
	return out;
}

std::string trimmed(const std::string& s) {
	size_t a = 0, b = s.size();
	while (a < b && static_cast<unsigned char>(s[a]) <= ' ') ++a;
	while (b > a && static_cast<unsigned char>(s[b - 1]) <= ' ') --b;
	return s.substr(a, b - a);
}

int g_out = -1;         // the real stdout; fd 1 is stderr while Psycle's code runs

bool put(const void* data, size_t bytes) {
	const char* p = static_cast<const char*>(data);
	while (bytes) {
		const ssize_t n = ::write(g_out, p, bytes);
		if (n <= 0) return false;
		p += n;
		bytes -= static_cast<size_t>(n);
	}
	return true;
}

int fail(const std::string& why) {
	const std::string line = "{\"tracks\":0,\"error\":" + json(utf8(why)) + "}\n";
	put(line.data(), line.size());
	return 1;
}

std::string plugin_dir(const char* argv0) {
	if (const char* env = std::getenv("BBMP_PSYCLE_PLUGINS")) return env;
	char self[4096];
	const ssize_t n = ::readlink("/proc/self/exe", self, sizeof self - 1);
	std::string path = n > 0 ? std::string(self, static_cast<size_t>(n)) : std::string(argv0);
	const std::string::size_type slash = path.rfind('/');
	path = slash == std::string::npos ? "." : path.substr(0, slash);
	return path + "/../lib/psycle-plugins";
}

// ---- what a song is made of, and what of it is missing ----

struct Part {
	int index;
	std::string name;       // as the song calls the machine
	std::string dll;        // the plugin it is, for the machines that are one
	bool generator, missing, vst, wired;
	long notes;
};

struct Census {
	std::vector<Part> parts;
	long notes;             // in the patterns that are played
	long lost;              // of them, for generators that are missing and wired
	std::vector<std::string> generators, effects;   // missing and wired, each name once
	Census() : notes(0), lost(0) {}
};

std::string bare(std::string dll) {
	const std::string::size_type slash = dll.find_last_of("/\\");
	if (slash != std::string::npos) dll = dll.substr(slash + 1);
	if (dll.size() > 4) {
		std::string end = dll.substr(dll.size() - 4);
		for (size_t i = 0; i < end.size(); ++i) end[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(end[i])));
		if (end == ".dll") dll.erase(dll.size() - 4);
	}
	if (dll.size() > 3 && dll.compare(dll.size() - 3, 3, ".so") == 0) dll.erase(dll.size() - 3);
	return trimmed(dll);
}

void add_once(std::vector<std::string>& list, const std::string& name) {
	if (std::find(list.begin(), list.end(), name) == list.end()) list.push_back(name);
}

Census census(Song& song) {
	Census c;
	// what reaches the master
	std::set<const Machine*> wired;
	std::vector<const Machine*> todo;
	if (song._pMachine[MASTER_INDEX]) todo.push_back(song._pMachine[MASTER_INDEX]);
	while (!todo.empty()) {
		const Machine* m = todo.back();
		todo.pop_back();
		if (!m || !wired.insert(m).second) continue;
		for (size_t w = 0; w < m->inWires.size(); ++w) {
			if (m->inWires[w].Enabled()) todo.push_back(&m->inWires[w].GetSrcMachine());
		}
		if (m->_type == MACH_MIXER) {       // and what a mixer sends out comes back to it
			const Mixer* mixer = static_cast<const Mixer*>(m);
			for (int r = 0; r < mixer->numreturns(); ++r) {
				if (mixer->ReturnValid(r)) todo.push_back(&mixer->Return(r).GetWire().GetSrcMachine());
			}
		}
	}
	// the notes of the patterns in the sequence, by machine, as the player reads them
	std::map<int, long> notes;
	std::set<int> seen;
	for (int pos = 0; pos < song.playLength; ++pos) {
		const int pattern = song.playOrder[pos];
		if (pattern >= MAX_PATTERNS || !song.ppPatternData[pattern]) continue;
		const bool again = !seen.insert(pattern).second;
		(void)again;      // a pattern that is played twice counts twice, as it sounds twice
		int previous[MAX_TRACKS];
		for (int t = 0; t < MAX_TRACKS; ++t) previous[t] = 255;
		for (int line = 0; line < song.patternLines[pattern]; ++line) {
			unsigned char* const at = song._ptrackline(pattern, 0, line);
			for (int track = 0; track < song.SONGTRACKS; ++track) {
				const PatternEntry* e = reinterpret_cast<const PatternEntry*>(at + track * EVENT_SIZE);
				if (e->_mach < MAX_VIRTUALINSTS) previous[track] = e->_mach;
				if (e->_note > notecommands::b9 || song._trackMuted[track]) continue;
				const int bus = previous[track];
				if (bus == 255) continue;
				int other = -1;
				const Machine* m = song.GetMachineOfBus(bus, other);
				if (!m) continue;
				++notes[m->_macIndex];
			}
		}
	}
	// a duplicator plays no note itself: its notes are those of the machines it sends to
	for (int i = 0; i < MAX_MACHINES; ++i) {
		Machine* m = song._pMachine[i];
		if (!m || (m->_type != MACH_DUPLICATOR && m->_type != MACH_DUPLICATOR2)) continue;
		const long n = notes.count(i) ? notes[i] : 0;
		if (!n) continue;
		// the machines it sends to are its first parameters: -1 is none
		const int outputs = m->_type == MACH_DUPLICATOR ? 8 : 16;
		std::set<int> targets;
		for (int p = 0; p < outputs; ++p) {
			int other = -1;
			const Machine* to = song.GetMachineOfBus(m->GetParamValue(p), other);
			if (to && to->_macIndex != i) targets.insert(to->_macIndex);
		}
		for (std::set<int>::const_iterator t = targets.begin(); t != targets.end(); ++t) notes[*t] += n;
		wired.insert(m);
		notes[i] = 0;
	}
	std::vector<std::string> mixers;
	for (int i = 0; i < MAX_MACHINES; ++i) {
		Machine* m = song._pMachine[i];
		if (!m || i == MASTER_INDEX) continue;
		Part p;
		p.index = i;
		p.name = trimmed(m->GetEditName());
		p.generator = i < MAX_BUSES;      // the first half of the slots is the generators'
		p.missing = p.vst = false;
		p.wired = wired.count(m) != 0;
		p.notes = notes.count(i) ? notes[i] : 0;
		if (m->_type == MACH_MIXER) {
			// A mixer that plays one column alone, and the column is none that a mixer has:
			// that is data of a mixer of another version, and nothing comes out of it.
			Mixer* mixer = static_cast<Mixer*>(m);
			bool known = !mixer->IsSoloColumn();
			for (int col = 0; col < 2 * MAX_CONNECTIONS && !known; ++col) known = mixer->GetSoloState(col);
			if (!known) {
				p.missing = true;
				p.dll = "Mixer (data of another version)";
				mixers.push_back(p.dll);
			}
		} else if (m->_type == MACH_DUMMY) {
			const Dummy* d = static_cast<const Dummy*>(m);
			if (!d->dllName.empty() || d->wasVST) {
				p.missing = true;
				p.vst = d->wasVST;
				p.dll = bare(d->dllName);
				if (p.dll.empty()) p.dll = p.name.compare(0, 2, "X!") == 0 ? trimmed(p.name.substr(2)) : p.name;
			}
		} else if (m->_type == MACH_PLUGIN) {
			p.dll = bare(m->GetDllName());
		}
		if (m->_type != MACH_DUPLICATOR && m->_type != MACH_DUPLICATOR2) c.notes += p.notes;
		if (p.missing && p.wired && m->_type != MACH_MIXER) {
			const std::string name = p.dll + (p.vst ? " (VST)" : "");
			if (p.generator) { if (p.notes) { c.lost += p.notes; add_once(c.generators, name); } }
			else add_once(c.effects, name);
		}
		c.parts.push_back(p);
	}
	// what goes through a mixer that gives nothing is lost with it
	if (!mixers.empty()) {
		c.lost = c.notes;
		c.generators.insert(c.generators.begin(), mixers.begin(), mixers.end());
	}
	return c;
}

std::string joined(const std::vector<std::string>& a, const std::vector<std::string>& b) {
	std::string out;
	for (size_t i = 0; i < a.size(); ++i) out += (out.empty() ? "" : ", ") + a[i];
	for (size_t i = 0; i < b.size(); ++i) out += (out.empty() ? "" : ", ") + b[i];
	return out;
}

// What may be missing of a song that is played all the same: effects, since a missing
// effect lets the sound through as it comes in, and generators that play no more than
// one note in ten. With more than that missing, what would be played is another song.
const double kMinorShare = 0.10;

bool minor(const Census& c) {
	if (c.generators.empty()) return true;
	return c.notes > 0 && static_cast<double>(c.lost) / c.notes <= kMinorShare;
}

int16_t clip(float v) {
	const long s = std::lround(v);
	return static_cast<int16_t>(std::max(-32768L, std::min(32767L, s)));
}

}  // namespace

int main(int argc, char* argv[]) {
	if (argc < 4) {
		std::fprintf(stderr, "usage: %s <file> <track> <samplerate> [start-ms]\n", argv[0]);
		return 2;
	}
	// Whatever Psycle's code or a plugin prints goes to stderr.
	std::fflush(stdout);
	g_out = dup(STDOUT_FILENO);
	dup2(STDERR_FILENO, STDOUT_FILENO);

	const char* path = argv[1];
	const int track = std::atoi(argv[2]);
	const int rate = std::atoi(argv[3]);
	const long long start_ms = argc > 4 ? std::atoll(argv[4]) : 0;
	if (rate < 8000 || rate > 192000) return fail("unsupported sample rate");
	if (track != 0) return fail("a Psycle song has one track");

	char head[12] = {0};
	if (FILE* f = std::fopen(path, "rb")) {
		const size_t n = std::fread(head, 1, 12, f);
		std::fclose(f);
		if (n != 12) return fail("not a Psycle song");
	} else {
		return fail("can't read the file");
	}
	const bool psy3 = std::memcmp(head, "PSY3SONG", 8) == 0;
	const bool psy2 = std::memcmp(head, "PSY2SONG", 8) == 0;
	if (!psy3 && !psy2) return fail("not a Psycle song");

	// As Psycle's audio threads have it: numbers too small to matter are zero.
#if defined(__x86_64__) || defined(__i386__)
	_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
	_MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#elif defined(__aarch64__)
	{	// the same on ARM: FPCR's FZ bit flushes denormals to zero
		uint64_t fpcr;
		__asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
		__asm__ volatile("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));
	}
#endif

	// The number after "PSY3SONG" is the sum of the versions of the parts of the file.
	// It is 0 in what Psycle wrote up to 1.8: songs of the years in which EQ-3 added
	// its bands, where it has them one after the other since 2010.
	uint32_t written = 0;
	std::memcpy(&written, head + 8, 4);
	psycle::host::PluginDir::Before2010(psy2 || written == 0);

	Host host(plugin_dir(argv[0]), rate);
	const char* newblitz = std::getenv("BBMP_PSYCLE_NEWBLITZ");
	Global::configuration().LoadNewBlitz(newblitz && *newblitz == '1');
	Song& song = Global::song();
	Player& player = Global::player();

	bool loaded = false;
	try {
		OldPsyFile file;
		if (!file.Open(path)) return fail("can't read the file");
		CProgressDialog progress;
		loaded = song.Load(&file, progress, true);
		file.Close();
	} catch (const std::exception& e) {
		return fail(std::string("Psycle can't read this song: ") + e.what());
	} catch (...) {
		return fail("Psycle can't read this song");
	}
	if (!loaded) return fail("Psycle can't read this song");
	if (song.playLength <= 0) return fail("the song has no patterns to play");

	const Census c = census(song);
	const char* info = std::getenv("BBMP_PSYCLE_INFO");
	if (info) {
		for (size_t i = 0; i < c.parts.size(); ++i) {
			const Part& p = c.parts[i];
			std::fprintf(stderr, "machine %02x %-9s %-7s %-5s notes %5ld  %s%s%s\n", p.index,
			             p.generator ? "generator" : "effect", p.missing ? "MISSING" : "here",
			             p.wired ? "wired" : "loose", p.notes, p.name.c_str(),
			             p.dll.empty() ? "" : "  <- ", p.dll.c_str());
		}
		std::fprintf(stderr, "notes %ld, of which for missing generators %ld\n", c.notes, c.lost);
		if (song._pMachine[MASTER_INDEX]) {
			const psycle::host::Master* master = static_cast<const psycle::host::Master*>(song._pMachine[MASTER_INDEX]);
			std::fprintf(stderr, "master volume %d of 256%s\n", master->_outDry, master->decreaseOnClip ? ", less when it clips" : "");
		}
		for (int i = 0; i < MAX_MACHINES; ++i) {
			Machine* m = song._pMachine[i];
			if (!m) continue;
			if (info[0] == '2' && m->_type == MACH_PLUGIN) {
				std::fprintf(stderr, "values %02x:", i);
				for (int k = 0; k < m->GetNumParams(); ++k) std::fprintf(stderr, " %d", m->GetParamValue(k));
				std::fprintf(stderr, "\n");
			}
			for (size_t w = 0; w < m->inWires.size(); ++w) {
				if (!m->inWires[w].Enabled()) continue;
				std::fprintf(stderr, "wire %02x -> %02x  volume %.3f%s%s\n", m->inWires[w].GetSrcMachine()._macIndex, i,
				             m->inWires[w].GetVolume(), m->_mute ? "  (muted)" : "", m->Bypass() ? "  (bypassed)" : "");
			}
			if (m->_type == MACH_MIXER) {
				Mixer* mixer = static_cast<Mixer*>(m);
				std::fprintf(stderr, "mixer %02x: %d inputs, %d returns, %d sends; master volume %.3f gain %.3f drywet %.3f\n", i,
				             mixer->numinputs(), mixer->numreturns(), mixer->numsends(), mixer->Master().Volume(),
				             mixer->Master().Gain(), mixer->Master().DryWetMix());
				for (int k = 0; k < mixer->numinputs(); ++k) {
					std::fprintf(stderr, "  input %d: volume %.3f pan %.3f dry %.3f%s%s%s\n", k, mixer->Channel(k).Volume(),
					             mixer->Channel(k).Panning(), mixer->Channel(k).DryMix(), mixer->Channel(k).Mute() ? " mute" : "",
					             mixer->Channel(k).DryOnly() ? " dryonly" : "", mixer->Channel(k).WetOnly() ? " wetonly" : "");
				}
				for (int k = 0; k < mixer->numreturns(); ++k) {
					std::fprintf(stderr, "  return %d: %s volume %.3f%s%s\n", k, mixer->ReturnValid(k) ? "wired" : "loose",
					             mixer->Return(k).Volume(), mixer->Return(k).Mute() ? " mute" : "",
					             mixer->Return(k).MasterSend() ? " to master" : "");
				}
			}
		}
	}
	if (info && *info == 'x') _exit(0);          // the machines were the question
	const std::string missing = joined(c.generators, c.effects);
	const char* policy = std::getenv("BBMP_PSYCLE_MISSING");
	if (!minor(c) && !(policy && std::strcmp(policy, "play") == 0)) {
		char share[64];
		std::snprintf(share, sizeof share, "%.0f%%", c.notes ? 100.0 * c.lost / c.notes : 100.0);
		return fail("the song needs machines that are not there: " + joined(c.generators, std::vector<std::string>()) +
		            " (" + share + " of its notes)" +
		            (c.effects.empty() ? "" : "; and the effects " + joined(c.effects, std::vector<std::string>())));
	}

	player._loopSong = false;
	player._recording = false;
	int seq = -1, line = -1, ms = -1, lines = -1;
	const long long length = player.CalcPosition(song, seq, line, ms, lines, false);
	const long long duration_ms = length * 1000 / rate;

	std::string names;
	for (size_t i = 0; i < c.parts.size(); ++i) {
		const Part& p = c.parts[i];
		if (!p.generator || !p.wired || p.missing) continue;
		std::string name = p.name;
		if (!p.dll.empty() && strcasecmp(p.dll.c_str(), p.name.c_str()) != 0) name += " (" + p.dll + ")";
		names += (names.empty() ? "" : ",") + json(utf8(name));
	}
	for (int i = 0; i < static_cast<int>(song.samples.size()); ++i) {
		if (!song.samples.IsEnabled(i)) continue;
		const std::string name = trimmed(song.samples[i].WaveName());
		if (!name.empty()) names += (names.empty() ? "" : ",") + json(utf8(name));
	}

	std::string comment = trimmed(song.comments);
	if (comment == "No Comments") comment.clear();
	if (!missing.empty()) comment = "Missing: " + missing + (comment.empty() ? "" : "\n" + comment);
	std::string title = trimmed(song.name), author = trimmed(song.author);
	if (title == "Untitled") title.clear();
	if (author == "Unnamed") author.clear();

	char numbers[96];
	std::snprintf(numbers, sizeof numbers, "\"duration_ms\":%lld", duration_ms);
	const std::string header = std::string("{\"tracks\":1,\"track\":0,\"title\":") + json(utf8(title)) +
		",\"author\":" + json(utf8(author)) + ",\"type\":" + json(psy3 ? "Psycle (PSY3)" : "Psycle (PSY2)") + "," +
		numbers + ",\"comment\":" + json(utf8(comment)) + ",\"names\":[" + names + "]}\n";
	if (!put(header.data(), header.size())) return 1;

	// ---- play ----
	player.Start(0, 0);
	long long skip = start_ms * rate / 1000;
	const int block = 1024;
	std::vector<int16_t> pcm(block * 2);
	long long played = 0;
	// The end is where Psycle's player stops. A song that jumps back onto the place
	// where it is never gets there: that one ends where its length was counted to.
	const long long limit = length + rate;
	const float quiet = 8.0f;                       // of 32768
	const long long tail_max = 10LL * rate, fade_from = 7LL * rate, quiet_for = rate / 2;
	long long tail = 0, silent = 0;
	bool ending = false;
	const bool levels = info && *info == '2';
	std::vector<float> peak(MAX_MACHINES, 0.0f), late(MAX_MACHINES, 0.0f);   // late: 3 s after the end
	for (;;) {
		if (!ending && (!player._playing || played >= limit)) {
			// The keys are let go, and what sounds after that is the end of the song.
			ending = true;
			if (info) std::fprintf(stderr, "the end after %.1f s, at position %d line %d: %s\n", static_cast<double>(played) / rate,
			                       player._playPosition, player._lineCounter, player._playing ? "as long as the song was counted to be" : "the player stopped");
			player._playing = false;
			for (int m = 0; m < MAX_MACHINES; ++m) {
				Machine* mac = song._pMachine[m];
				if (!mac || m == MASTER_INDEX) continue;
				for (int t = 0; t < song.SONGTRACKS; ++t) {
					mac->TriggerDelay[t]._cmd = 0;
					PatternEntry off(notecommands::release, 255, 255, 0, 0);
					try { mac->Tick(t, &off); } catch (...) {}
				}
			}
		}
		if (ending && (tail >= tail_max || silent >= quiet_for)) break;
		float* buffer = 0;
		try {
			buffer = player.Work(block);
		} catch (const std::exception& e) {
			std::fprintf(stderr, "bbmp-psycle: %s\n", e.what());
			break;
		} catch (...) {
			std::fprintf(stderr, "bbmp-psycle: a machine failed\n");
			break;
		}
		if (!buffer) break;
		if (levels) {
			for (int m = 0; m < MAX_MACHINES; ++m) {
				const Machine* mac = song._pMachine[m];
				if (!mac || mac->samplesV.empty() || !mac->samplesV[0]) continue;
				// what is left in the machine's buffer is the last piece of the block
				std::vector<float>& to = ending && tail > 3 * rate ? late : peak;
				for (int i = 0; i < 64; ++i) to[m] = std::max(to[m], std::fabs(mac->samplesV[0][i]));
			}
		}
		for (int i = 0; i < block; ++i) {
			float l = buffer[i * 2], r = buffer[i * 2 + 1];
			if (!(l == l)) l = 0;                        // a plugin gone wrong gives no number
			if (!(r == r)) r = 0;
			if (ending) {
				const long long at = tail + i;
				if (at >= fade_from) {
					const float gain = std::max(0.0f, 1.0f - static_cast<float>(at - fade_from) / (tail_max - fade_from));
					l *= gain;
					r *= gain;
				}
				if (std::fabs(l) < quiet && std::fabs(r) < quiet) ++silent; else silent = 0;
			}
			pcm[i * 2] = clip(l);
			pcm[i * 2 + 1] = clip(r);
		}
		played += block;
		if (ending) tail += block;
		if (skip >= block) { skip -= block; continue; }
		const size_t from = static_cast<size_t>(skip) * 2;
		skip = 0;
		if (!put(pcm.data() + from, (pcm.size() - from) * sizeof(int16_t))) break;
	}
	if (levels) {
		for (int m = 0; m < MAX_MACHINES; ++m) {
			if (song._pMachine[m]) std::fprintf(stderr, "level %02x %-16s peak %.0f, after the end %.0f\n", m, song._pMachine[m]->GetEditName(), peak[m], late[m]);
		}
	}
	// The process ends here, without taking Psycle's objects down.
	_exit(0);
}
