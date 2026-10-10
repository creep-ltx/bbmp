/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Put in front of every file of ZXTune by build-zxtune.sh and build-ay.sh (its make
 * gives the same flags to C and C++): GCC 16's library headers no longer bring
 * std::intmax_t along, which src/strings/conversion.h uses. */
#include <stdint.h>
#ifdef __cplusplus
#include <cstdint>
#endif
