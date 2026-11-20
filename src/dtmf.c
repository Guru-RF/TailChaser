/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "dtmf.h"

#include <math.h>
#include <string.h>

/* Standard DTMF frequency grid. Index 0..3 = low group, 4..7 = high group. */
static const double DTMF_FREQ[8] = {
    697.0, 770.0, 852.0, 941.0,        /* low  */
    1209.0, 1336.0, 1477.0, 1633.0,    /* high */
};

/* digit = MATRIX[low_idx][high_idx] */
static const char DTMF_MATRIX[4][4] = {
    { '1', '2', '3', 'A' },
    { '4', '5', '6', 'B' },
    { '7', '8', '9', 'C' },
    { '*', '0', '#', 'D' },
};

/* A digit is two sines and nothing else. A voice is not — but a vowel has a
 * harmonic near 697 Hz and another near 1209 Hz often enough, and a decoder
 * that only asks "are those two tones there?" hears digits in speech: the one
 * this replaces heard seven in three minutes of a recorded voice, and one
 * every 16 seconds in a QSO on the air (each of which cut 100 ms out of the
 * talker's audio). So the question asked here is "is there anything else?".
 *
 * The audio is looked at in windows of one block (20 ms), half a block apart.
 * A window shows a digit when
 *
 *   - DTMF_PAIR_SHARE: the strongest low-group tone and the strongest
 *     high-group tone together hold at least this share of the window's
 *     energy. A digit holds all of it, less the noise that rides along (0.8 at
 *     6 dB signal-to-noise, 0.9 at 10 dB). The rest of a vowel's harmonics
 *     keep a voice well below: in three minutes each of a recorded female and
 *     male voice no window reached 0.8, and no three in a row 0.7.
 *   - DTMF_MAX_TWIST: neither tone is more than this many times stronger than
 *     the other (16 = 12 dB; a transmitter with pre-emphasis into a receiver
 *     without de-emphasis makes 7 dB of it on its own). This is what keeps a
 *     single whistle near 941 Hz from being half of a digit.
 *   - DTMF_DOMINANCE: the winning tone of each group is this many times
 *     stronger than the runner-up of that group.
 *   - DTMF_FREQ_SLACK: each tone is also looked for this far below and above
 *     its nominal frequency, so a keypad that is 1.5 % off still fills the
 *     window. (20 ms is 50 Hz wide: at 1633 Hz, 1.5 % is half of that.)
 *
 * and it is a digit once DTMF_CONFIRM windows in a row have shown the same
 * one — 40 ms of nothing but those two tones. Because the windows overlap,
 * that does not depend on where in a block the tone began: digits of 50 ms and
 * longer are always caught, shorter ones mostly.
 *
 * The energy is measured behind a 300 Hz high-pass, so a DC offset of the
 * sound card, mains hum or a CTCSS tone under the audio do not count against
 * the digit.
 *
 * What it costs: a digit needs about 7 dB of signal-to-noise, where the old
 * test still worked at 0 dB. A station that noisy is hard to copy as well. */
#define DTMF_PAIR_SHARE   0.80
#define DTMF_MAX_TWIST    16.0
#define DTMF_DOMINANCE    4.0
#define DTMF_FREQ_SLACK   0.010
#define DTMF_CONFIRM      3     /* windows in a row with the same digit                       */
#define DTMF_REARM        3     /* windows without a digit before the same one counts again   */
#define DTMF_HANG_BLOCKS  2     /* "on the channel" for one more block: the tone dying away   */

void dtmf_init(dtmf_t *d, int sample_rate)
{
    memset(d, 0, sizeof *d);
    d->sample_rate = sample_rate;
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 3; k++) {
            double f = DTMF_FREQ[i] * (1.0 + (k - 1) * DTMF_FREQ_SLACK);
            d->coeff[i][k] = 2.0 * cos(2.0 * M_PI * f / (double)sample_rate);
        }
    /* RBJ high-pass, Butterworth, 300 Hz */
    double w = 2.0 * M_PI * 300.0 / (double)sample_rate, cw = cos(w), al = sin(w) / (2.0 * M_SQRT1_2);
    double a0 = 1.0 + al;
    d->hp[0] = (1.0 + cw) / 2.0 / a0;
    d->hp[1] = -(1.0 + cw) / a0;
    d->hp[2] = (1.0 + cw) / 2.0 / a0;
    d->hp[3] = -2.0 * cw / a0;
    d->hp[4] = (1.0 - al) / a0;
}

void dtmf_reset(dtmf_t *d)
{
    d->prev_n = 0;
    d->last_emitted = 0;
    d->cand = 0;
    d->cand_count = 0;
    d->hang = 0;
    d->silence = 0;
}

/* One window, given as two pieces that follow each other: the digit it shows,
 * or 0. */
static char window_digit(const dtmf_t *d, const float *a, int na, const float *b, int nb)
{
    int n = na + nb;
    double energy = 1.0;            /* +1 avoids divide-by-zero on silence */
    for (int i = 0; i < na; i++) energy += (double)a[i] * a[i];
    for (int i = 0; i < nb; i++) energy += (double)b[i] * b[i];

    /* Goertzel: tone power as a share of the window's energy. A sine that
     * fills the window measures n/2 times the energy, so 2/n scales it to 1. */
    double m[8];
    for (int t = 0; t < 8; t++) {
        double best = 0.0;
        for (int k = 0; k < 3; k++) {
            double c = d->coeff[t][k], s0, s1 = 0.0, s2 = 0.0;
            for (int i = 0; i < na; i++) { s0 = (double)a[i] + c * s1 - s2; s2 = s1; s1 = s0; }
            for (int i = 0; i < nb; i++) { s0 = (double)b[i] + c * s1 - s2; s2 = s1; s1 = s0; }
            double p = s1 * s1 + s2 * s2 - c * s1 * s2;
            if (p > best) best = p;
        }
        m[t] = best / energy * 2.0 / (double)n;
    }

    /* Strongest + runner-up in each group. */
    int lo = 0, hi = 4;
    double lo2 = 0.0, hi2 = 0.0;
    for (int i = 1; i < 4; i++) {
        if (m[i] > m[lo]) { lo2 = m[lo]; lo = i; }
        else if (m[i] > lo2) lo2 = m[i];
    }
    for (int i = 5; i < 8; i++) {
        if (m[i] > m[hi]) { hi2 = m[hi]; hi = i; }
        else if (m[i] > hi2) hi2 = m[i];
    }

    bool pair =
        m[lo] + m[hi] >= DTMF_PAIR_SHARE &&
        m[lo] <= DTMF_MAX_TWIST * m[hi] && m[hi] <= DTMF_MAX_TWIST * m[lo] &&
        m[lo] > DTMF_DOMINANCE * lo2 && m[hi] > DTMF_DOMINANCE * hi2;
    return pair ? DTMF_MATRIX[lo][hi - 4] : 0;
}

/* Take the next window's verdict. Returns true while a confirmed digit is
 * held; *fresh gets the digit in the window that confirms a new one. */
static bool next_window(dtmf_t *d, char w, char *fresh)
{
    if (!w) {
        if (++d->silence >= DTMF_REARM) {
            d->cand = 0;
            d->cand_count = 0;
            d->last_emitted = 0;
        }
        return false;
    }
    d->silence = 0;
    if (w == d->cand) {
        d->cand_count++;
    } else {
        d->cand = w;
        d->cand_count = 1;
    }
    if (d->cand_count < DTMF_CONFIRM)
        return false;       /* looks like a digit, but a voice does that for a moment too */
    if (d->cand_count == DTMF_CONFIRM && w != d->last_emitted) {
        d->last_emitted = w;                    /* one emission per held tone burst */
        *fresh = w;
    }
    return true;
}

char dtmf_process(dtmf_t *d, const int16_t *pcm, int n, bool *tone_present)
{
    *tone_present = false;
    if (n <= 1 || n > DTMF_MAX_BLOCK)
        return 0;

    /* high-pass (direct form II transposed), state carried across blocks */
    static _Thread_local float x[DTMF_MAX_BLOCK];
    for (int i = 0; i < n; i++) {
        double in = (double)pcm[i];
        double out = d->hp[0] * in + d->hz[0];
        d->hz[0] = d->hp[1] * in - d->hp[3] * out + d->hz[1];
        d->hz[1] = d->hp[2] * in - d->hp[4] * out;
        x[i] = (float)out;
    }

    /* Two windows end in this block: the one that straddles the boundary with
     * the block before, and the block itself. */
    int h = n / 2;
    char fresh = 0;
    bool held = false;
    if (d->prev_n == n)
        held |= next_window(d, window_digit(d, d->prev + h, n - h, x, h), &fresh);
    held |= next_window(d, window_digit(d, x, n, x, 0), &fresh);
    memcpy(d->prev, x, sizeof(float) * (size_t)n);
    d->prev_n = n;

    if (held)
        d->hang = DTMF_HANG_BLOCKS;
    else if (d->hang > 0)
        d->hang--;
    *tone_present = held || d->hang > 0;
    return fresh;
}
