// SPDX-License-Identifier: GPL-2.0-or-later
// bbmp-vgm: the few functions the sound cores call outside themselves (their headers
// emu/logging.h, emu/panning.h and emu/SoundEmu.h declare them), written for bbmp.
#include <math.h>
#include <stdarg.h>
#include <stddef.h>

#include "stdtype.h"
#include "emu/EmuStructs.h"
#include "emu/logging.h"
#include "emu/panning.h"

// The cores' messages are not shown.
void emu_logf(DEV_LOGGER* logger, UINT8 level, const char* format, ...)
{
    (void)logger;
    (void)level;
    (void)format;
}

// Panning by a position from -0x100 (left) to +0x100 (right): the two gains (16.16)
// keep the sum of their squares constant, so a sound in the middle is as loud as one
// at a side. The middle gives both sides the same gain.
void Panning_Calculate(INT32 channels[2], INT16 position)
{
    const double halfPi = 1.5707963267948966;
    double p;
    if (position > 0x100)
        position = 0x100;
    else if (position < -0x100)
        position = -0x100;
    p = (position + 0x100) / 512.0;
    channels[0] = (INT32)(cos(p * halfPi) * PANNING_NORMAL * 1.4142135623730951 + 0.5);
    channels[1] = (INT32)(sin(p * halfPi) * PANNING_NORMAL * 1.4142135623730951 + 0.5);
}

void Panning_Centre(INT32 channels[2])
{
    channels[0] = channels[1] = PANNING_NORMAL;
}

// Find one of a core's read/write functions by kind (used by the YMF278B core to reach
// the OPL3 it is linked to). 0 when found.
UINT8 SndEmu_GetDeviceFunc(const DEV_DEF* devDef, UINT8 funcType, UINT8 rwType, UINT16 user, void** retFuncPtr)
{
    const DEVDEF_RWFUNC* f;
    for (f = devDef->rwFuncs; f != NULL && f->funcPtr != NULL; f++) {
        if (f->funcType == funcType && f->rwType == rwType && (user == 0 || f->user == user)) {
            *retFuncPtr = f->funcPtr;
            return 0x00;
        }
    }
    return 0xFF;
}
