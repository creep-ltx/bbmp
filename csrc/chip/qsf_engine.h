// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef QSF_ENGINE_H
#define QSF_ENGINE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct qsfw_info {
  const char *title, *author;
  const char *game, *year, *copyright, *ripper, *comment;  /* never NULL */
  long length_ms, fade_ms;  /* -1 when the file doesn't say */
};
#define QSFW_RATE 44100
const char *qsfw_open(const char *path, struct qsfw_info *info);  /* NULL on success */
int qsfw_render(int16_t *buf, unsigned frames);  /* stereo at QSFW_RATE; frames made, <0 on error */
void qsfw_close(void);
#ifdef __cplusplus
}
#endif
#endif
