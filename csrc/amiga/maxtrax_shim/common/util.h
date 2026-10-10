// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common/scummsys.h"

#define ARRAYSIZE(x) ((int)(sizeof(x) / sizeof(x[0])))

template<typename T> inline T MIN(T a, T b) { return a < b ? a : b; }
template<typename T> inline T MAX(T a, T b) { return a > b ? a : b; }
template<typename T> inline T CLIP(T v, T amin, T amax) { return v < amin ? amin : v > amax ? amax : v; }
#define ARRAYEND(x) ((x) + ARRAYSIZE((x)))
