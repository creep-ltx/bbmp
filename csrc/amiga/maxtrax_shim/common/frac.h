// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common/scummsys.h"

// as ScummVM's common/frac.h: fixed point numbers with 16 bits after the point
typedef int32 frac_t;
enum { FRAC_BITS = 16, FRAC_LO_MASK = ((1L << FRAC_BITS) - 1), FRAC_HI_MASK = ((1L << FRAC_BITS) - 1) << FRAC_BITS,
       FRAC_ONE = (1L << FRAC_BITS), FRAC_HALF = (1L << (FRAC_BITS - 1)) };
inline frac_t doubleToFrac(double value) { return (frac_t)(value * FRAC_ONE); }
inline double fracToDouble(frac_t value) { return ((double)value) / FRAC_ONE; }
inline frac_t intToFrac(int16 value) { return value * (1 << FRAC_BITS); }
inline int16 fracToInt(frac_t value) { return value / (1 << FRAC_BITS); }
