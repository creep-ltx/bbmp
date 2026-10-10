// SPDX-License-Identifier: GPL-2.0-only
// bbmp-tiatracker: stands in for TIATracker's SoundSDL2 (Stella's SDL sound output).
// Written for bbmp, not Stella's code. The replay code writes the six TIA sound
// registers and ends each frame; here a frame's registers are only kept, and the
// helper (bbmp-tiatracker.cpp) hands them to the TIA sound emulation, as SoundSDL2
// does at each frame's sample, then makes that frame's samples.
#ifndef SOUNDSDL2_H
#define SOUNDSDL2_H

#include "emulation/TIASnd.h"

namespace Emulation {

class SoundSDL2 {
public:
    explicit SoundSDL2(TIASound *tiasound) : myTIASound(tiasound) {}

    void setFrameRate(float framerate) { myFrameRate = framerate; }
    float frameRate() const { return myFrameRate; }
    void open() {}
    void close() {}
    void mute(bool) {}
    void setEnabled(bool) {}
    void setVolume(Int32) {}

    void set(uInt16 addr, uInt8 value) {
        if (addr >= AUDC0 && addr <= AUDV1) myRegisters[addr - AUDC0] = value;
    }
    void endFrame() { ++myFrames; }

    // AUDC0, AUDC1, AUDF0, AUDF1, AUDV0, AUDV1 as the last frame left them
    const uInt8 *registers() const { return myRegisters; }
    long frames() const { return myFrames; }

private:
    TIASound *myTIASound;
    float myFrameRate = 50.0f;
    uInt8 myRegisters[6] = {0, 0, 0, 0, 0, 0};
    long myFrames = 0;
};

}

#endif
