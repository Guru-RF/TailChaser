/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — a tone squelch in software.
 *
 * For a receiver that has no CTCSS decoder of its own, or no squelch line at
 * all: a set that only gives its audio to the sound card. The sub-audible tone
 * is looked for in the received audio and, once found, the squelch is open;
 * when it is gone, the squelch closes.
 *
 * How: the audio is correlated with the tone over a sliding window, and with
 * the frequencies next to it, whole window bins away — where a steady tone in
 * the middle leaves nothing. The tone must stand out above the bins two to
 * five away (a signal-to-noise ratio), above the two right next to it (it is
 * this tone, not the next one in the CTCSS table) and be loud enough. Noise, a
 * voice without the tone and another tone do not pass.
 *
 * Finding a tone takes most of a window, so the audio is delayed by as much:
 * the start of the over is not lost, and the noise burst after the carrier
 * has dropped is not passed on. The tone itself is filtered out of what goes
 * on (a 300 Hz high-pass).
 */
#ifndef TAILCHASER_CTCSS_H
#define TAILCHASER_CTCSS_H

#include <stdbool.h>
#include <stdint.h>

#define CTCSS_MAX_FRAME   960
#define CTCSS_MAX_WINDOW  64        /* frames */
#define CTCSS_MAX_DELAY   64        /* frames */
#define CTCSS_BINS        11        /* the tone, and five on each side */

typedef struct {
    int   sample_rate;      /* Hz */
    int   frame;            /* samples per call */
    float tone_hz;
    int   window_ms;        /* longer: more selective and slower            */
    int   delay_ms;         /* the audio is held back this long             */
    float open_snr_db;      /* the tone this far above the noise opens      */
    float close_snr_db;     /* ...and below this it is gone                 */
    float min_level_db;     /* dBFS: a tone weaker than this is not one     */
    int   open_ms;          /* found for this long before the squelch opens */
    int   close_ms;         /* gone for this long before it closes          */
    bool  highpass;         /* take the tone out of the audio               */
} ctcss_params_t;

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } ctcss_biquad_t;

typedef struct {
    ctcss_params_t p;
    int     window, delay;                          /* frames */
    double  step[CTCSS_BINS], phase[CTCSS_BINS];    /* radians */
    float   re[CTCSS_MAX_WINDOW][CTCSS_BINS], im[CTCSS_MAX_WINDOW][CTCSS_BINS];
    int     pos;
    int16_t line[CTCSS_MAX_DELAY][CTCSS_MAX_FRAME];
    int     line_pos;
    ctcss_biquad_t hp[2];
    bool    open;
    int     good, bad;                              /* frames in a row */
    int     line_hold;
    float   level_db, snr_db, sel_db;               /* the last frame */
    float   open_level_db, open_snr_db;             /* when it last opened */
} ctcss_t;

void ctcss_default_params(ctcss_params_t *p);
void ctcss_init(ctcss_t *c, const ctcss_params_t *p);

/* One captured frame in, one (delayed, filtered) frame out — in place.
 * line: the squelch line of the radio, true where there is none; the tone
 * only counts while the line is (or just was) open.
 * Returns the squelch for the frame that comes out. */
bool ctcss_process(ctcss_t *c, int16_t *pcm, bool line);

#endif /* TAILCHASER_CTCSS_H */
