// SPDX-License-Identifier: GPL-3.0-or-later
// The parts of Psycle's host that belong to the Windows program and not to the
// player, written again for this helper: the global objects, the configuration
// and the list of plugins.

#include <psycle/host/detail/project.private.hpp>
#include "host.hpp"

#include <psycle/host/Machine.hpp>
#include <psycle/host/Player.hpp>
#include <psycle/host/Song.hpp>
#include "VstHost24.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>

VstTimeInfo seib::vst::CVSTHost::vstTimeInfo;
seib::vst::CVSTHost* seib::vst::CVSTHost::pHost = 0;

namespace psycle { namespace host {

// ---- Global ----

Configuration* Global::pConfig(0);
Song* Global::pSong(0);
Player* Global::pPlayer(0);
vst::Host* Global::pVstHost(0);
MachineLoader* Global::pMacLoad(0);
FAvSetMmThreadCharacteristics Global::pAvSetMmThreadCharacteristics(0);
FAvRevertMmThreadCharacteristics Global::pAvRevertMmThreadCharacteristics(0);

BOOL Is_Vista_or_Later() { return FALSE; }

Global::Global() {}
Global::~Global() throw() {}

// ---- Configuration ----

Configuration::Configuration() : _pOutputDriver(0) { SetDefaultSettings(); }
Configuration::~Configuration() throw() {}

void Configuration::SetDefaultSettings(bool) {
	UseJBridge(false);
	UsePsycleVstBridge(false);
	SetDefaultPatLines(64);
	UseAutoStopMachines(false);
	LoadNewBlitz(false);
	SetNumThreads(1);
}

void Configuration::Load(ConfigStorage&) {}
void Configuration::Save(ConfigStorage&) {}

// One thread does everything: the songs are small, and a helper that is run once per
// song gains nothing from a pool of threads.
void Configuration::RefreshSettings() {}

std::string Configuration::AsAbsolute(std::string dir) const { return dir; }
std::string Configuration::GetAbsolutePluginDir() const { return plugin_dir_; }
std::string Configuration::GetAbsoluteLuaDir() const { return lua_dir_; }
std::string Configuration::GetAbsoluteLadspaDir() const { return ladspa_dir_; }
std::string Configuration::GetAbsoluteVst32Dir() const { return vst32_dir_; }
std::string Configuration::GetAbsoluteVst64Dir() const { return vst64_dir_; }
bool Configuration::SupportsJBridge() const { return false; }
void Configuration::UseJBridge(bool) {}
bool Configuration::UsesJBridge() const { return false; }
void Configuration::UsePsycleVstBridge(bool) {}
bool Configuration::UsesPsycleVstBridge() const { return false; }
void Configuration::SetDefaultPatLines(int lines) { Song::SetDefaultPatLines(lines); }
int Configuration::GetDefaultPatLines() const { return Song::GetDefaultPatLines(); }
void Configuration::UseAutoStopMachines(bool use) { Machine::autoStopMachine = use; }
bool Configuration::UsesAutoStopMachines() const { return Machine::autoStopMachine; }
void Configuration::LoadNewBlitz(bool use) { prefferNewBlitz = use; }
bool Configuration::LoadsNewBlitz() const { return prefferNewBlitz; }
void Configuration::SetNumThreads(int threads) { numThreads_ = threads; }
int Configuration::GetNumThreads() const { return numThreads_; }

// ---- the plugins ----

bool PluginDir::before2010_ = false;

std::string PluginDir::Key(const std::string& name) {
	std::string key = name;
	const std::string::size_type slash = key.find_last_of("/\\");
	if (slash != std::string::npos) key = key.substr(slash + 1);
	for (std::string::size_type i = 0; i < key.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(key[i]);
		key[i] = (c == ' ' || c == '_') ? '-' : static_cast<char>(std::tolower(c));
	}
	const char* endings[] = {".dll", ".so"};
	for (int i = 0; i < 2; ++i) {
		const std::string::size_type at = key.find(endings[i]);
		if (at != std::string::npos) key = key.substr(0, at);
	}
	return key;
}

bool PluginDir::lookupDllName(const std::string& name, std::string& result, MachineType type, int32_t& shellIdx) {
	shellIdx = 0;
	if (type != MACH_PLUGIN) return false;
	struct stat st;
	std::string path = dir_ + "/before-2010/" + Key(name) + ".so";
	if (!before2010_ || ::stat(path.c_str(), &st) != 0) path = dir_ + "/" + Key(name) + ".so";
	if (::stat(path.c_str(), &st) != 0) return false;
	result = path;
	return true;
}

bool PluginDir::TestFilename(const std::string& name, const int32_t) {
	struct stat st;
	return ::stat(name.c_str(), &st) == 0;
}

// ---- the objects, in the order in which Psycle makes them ----

Host::Host(const std::string& plugins, int rate) {
	settings_.setSamplesPerSec(rate);
	settings_.setValidBitDepth(16);
	settings_.setChannelMode(stereo);
	driver_ = settings_.NewDriver();
	pConfig = new Configuration();
	pConfig->SetPluginDir(plugins);
	pConfig->_pOutputDriver = driver_;
	pMacLoad = new PluginDir(plugins);
	pVstHost = new vst::Host();
	pSong = new Song();
	pPlayer = new Player();
	pPlayer->SetSampleRate(rate);
}

Host::~Host() throw() {}

}}
