// SPDX-License-Identifier: GPL-3.0-or-later
// What Psycle's precompiled header gives the plugins on Windows: the standard
// headers that their sources use without including them.
#pragma once
#ifdef __cplusplus
#include <algorithm>
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Psycle's code is written for Microsoft's rand(), which gives 15 bits: "rand() - 16384"
// is noise around zero there, and with the 31 bits of glibc's rand() it is a roar.
// This is Microsoft's generator. Nothing seeds it, so that a song sounds the same
// every time it is played.
inline int modland_rand() {
  static unsigned int seed = 1;
  seed = seed * 214013u + 2531011u;
  return static_cast<int>((seed >> 16) & 0x7fff);
}
inline void modland_srand(unsigned int) {}
namespace std { using ::modland_rand; using ::modland_srand; }
#undef RAND_MAX
#define RAND_MAX 0x7fff
#define rand modland_rand
#define srand modland_srand
#else
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif
