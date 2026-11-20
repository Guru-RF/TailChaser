/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — minimal 16-bit PCM WAV reader/writer (test tools + file input). */
#ifndef SVXRX_WAV_H
#define SVXRX_WAV_H

#include <stdint.h>

/* Load the first channel of a 16-bit PCM WAV. Returns a malloc'd buffer of
 * *n_out samples (caller frees) and the sample rate, or NULL on error. */
int16_t *wav_load(const char *path, int *n_out, int *rate_out);

/* Write mono 16-bit PCM. Returns 0 on success. */
int wav_save(const char *path, const int16_t *pcm, int n, int rate);

#endif /* SVXRX_WAV_H */
