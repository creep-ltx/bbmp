// SPDX-License-Identifier: GPL-3.0-or-later
// What the helper makes of Psycle's host: see host.cpp.
#pragma once
#include <psycle/host/detail/project.hpp>
#include <psycle/host/AudioDriver.hpp>
#include <psycle/host/Configuration.hpp>
#include <psycle/host/Global.hpp>
#include <psycle/host/machineloader.hpp>

#include <string>

namespace psycle { namespace host {

/// The native plugins are libraries in one folder, named as Psycle names them:
/// "Arguru Synth 2f.dll" of a song is arguru-synth-2f.so there.
class PluginDir : public MachineLoader {
 public:
	explicit PluginDir(const std::string& dir) : dir_(dir) {}
	static std::string Key(const std::string& name);
	/// For a song of before Psycle's revision of its plugins in 2010: the plugins in
	/// the folder before-2010 go before the others.
	static void Before2010(bool on) { before2010_ = on; }
	virtual bool lookupDllName(const std::string& name, std::string& result, MachineType type, int32_t& shellIdx);
	virtual bool TestFilename(const std::string& name, const int32_t shellIdx);
	virtual void LoadPluginInfo(bool = true) {}
	virtual void ReScan(bool = true) {}
	virtual bool IsLoaded() { return true; }
 private:
	std::string dir_;
	static bool before2010_;
};

/// Psycle's global objects. They are made once and never taken down: the helper
/// plays one song and ends.
class Host : public Global {
 public:
	Host(const std::string& plugins, int rate);
	virtual ~Host() throw();
 private:
	SilentSettings settings_;
	AudioDriver* driver_;
};

}}
