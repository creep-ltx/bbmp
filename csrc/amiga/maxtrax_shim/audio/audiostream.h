// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common/scummsys.h"
#include "common/util.h"

namespace Audio {
class AudioStream {
public:
	virtual ~AudioStream() {}
	virtual int readBuffer(int16 *buffer, const int numSamples) = 0;
	virtual bool isStereo() const = 0;
	virtual int getRate() const = 0;
	virtual bool endOfData() const = 0;
};
}
