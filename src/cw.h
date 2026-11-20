/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — Morse (CW) audio for the identification.
 *
 * Standard timing: a dot is 1200/wpm ms, a dash three dots, one dot between
 * the elements of a character, three between characters, seven between words.
 * Every element gets a 5 ms raised-cosine edge, so the keying does not click.
 */
#ifndef SVXSR_CW_H
#define SVXSR_CW_H

#include <stdint.h>

/* Length of `text` in dot units, without leading or trailing silence.
 * "ON0ORA" is 73 units: 3.65 s at 24 wpm. Unknown characters count as a word
 * gap. */
int cw_units(const char *text);

/* Render `text` as mono 16-bit PCM. level_db is the tone peak in dBFS
 * (negative; -12 is a quarter of full scale). lead_ms/tail_ms of silence are
 * put before and after. Returns a malloc'd buffer of *n_out samples (the
 * caller frees it), or NULL when there is nothing to send. */
int16_t *cw_render(const char *text, int wpm, int pitch_hz, float level_db,
                   int sample_rate, int lead_ms, int tail_ms, int *n_out);

/* Copy `in` to `out` keeping only what can be sent in Morse (upper-cased).
 * Returns the number of characters kept. */
int cw_sanitize(const char *in, char *out, int cap);

#endif /* SVXSR_CW_H */
