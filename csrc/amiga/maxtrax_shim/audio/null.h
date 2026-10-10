// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vector>

// paula.cpp ends with the entry for ScummVM's list of music devices. There is no
// such list here: these make that entry compile and nothing else.
class NullMusicPlugin;
enum { MT_AMIGA = 0 };
struct MusicDevice {
	MusicDevice(const NullMusicPlugin *, const char *, int) {}
};
typedef std::vector<MusicDevice> MusicDevices;
class NullMusicPlugin {
public:
	virtual ~NullMusicPlugin() {}
	virtual const char *getName() const = 0;
	virtual const char *getId() const = 0;
	virtual MusicDevices getDevices() const = 0;
};
#define _s(text) text
#define REGISTER_PLUGIN_STATIC(id, type, plugin)
