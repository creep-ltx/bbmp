/* SPDX-License-Identifier: MIT */
/* bbmp-snsf: the SNSF loader and snsf9x behind a tiny C interface (snsf_engine.cpp). */
#ifndef SNSF_ENGINE_H
#define SNSF_ENGINE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct snsfw_info {
  const char *title, *author;
  const char *game, *year, *copyright, *ripper, *comment;  /* never NULL */
  long length_ms, fade_ms;  /* -1 when the file doesn't say */
};
const char *snsfw_open(const char *path, int rate, struct snsfw_info *info);  /* NULL on success */
int snsfw_render(int16_t *buf, unsigned frames);  /* stereo at the requested rate */
void snsfw_close(void);
#ifdef __cplusplus
}
#endif
#endif
