// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef FMP_ENGINE_H
#define FMP_ENGINE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* returns 0 on success, else an error message */
const char *fmpw_open(const char *path, int *is_fmp);
const char *fmpw_comment(int line);  /* Shift-JIS, may be NULL */
void fmpw_render(int16_t *buf, unsigned frames);  /* stereo at 55467 Hz */
int fmpw_playing(void);
int fmpw_loops(void);
void fmpw_close(void);
/* 0 on success; -1 no ROM file, -2 not a YM2608 rhythm ROM, -3 can't write */
int fmpw_write_rhythm(const char *rom_path, const char *dest_dir);
#ifdef __cplusplus
}
#endif
#endif
