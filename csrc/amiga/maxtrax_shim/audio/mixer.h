// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common/mutex.h"

// Paula asks the system's mixer for its lock
namespace Audio {
class Mixer {
public:
	Common::Mutex &mutex() { return _mutex; }
private:
	Common::Mutex _mutex;
};
}
class OSystem {
public:
	Audio::Mixer *getMixer() { return &_mixer; }
private:
	Audio::Mixer _mixer;
};
extern OSystem *g_system;
