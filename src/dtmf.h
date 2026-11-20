/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — software DTMF decoder (Goertzel).
 *
 * The SA818 does not decode DTMF (only CTCSS squelch), so we decode it in
 * software from the captured RX PCM in the audio thread, on int16 mono blocks
 * (ARCHITECTURE.md §8). This is NOT software CTCSS (req #3 forbids that —
 * sub-audible squelch tones stay in the chip); DTMF is an in-band signalling
 * decode that runs only on audio the chip squelch already gated open, and the
 * digits are consumed locally and never forwarded to the network.
 *
 * dtmf_process() also reports whether a digit is on the channel right now, so
 * the caller can WITHHOLD that audio from the network — tones never leak onto
 * the reflector (req #6), independent of how fast the engine reacts. A digit is
 * known for what it is two blocks after it began; a caller that wants all of
 * it withheld keeps two blocks back (gate.c does).
 *
 * What makes a DTMF digit — and not a vowel that happens to have a harmonic
 * near 770 Hz and another near 1336 Hz — is that the two tones hold nearly ALL
 * of the energy, for 40 ms. See dtmf.c.
 */
#ifndef SMARTSPOT_DTMF_H
#define SMARTSPOT_DTMF_H

#include <stdbool.h>
#include <stdint.h>

#define DTMF_MAX_BLOCK 960  /* samples per block we accept (60 ms @ 16 kHz) */

typedef struct {
    int    sample_rate;
    double coeff[8][3];  /* Goertzel coefficients, 4 low + 4 high tones, each also a
                          * little below and above its nominal frequency          */
    double hp[5];        /* 300 Hz high-pass in front of it: b0 b1 b2 a1 a2 */
    double hz[2];        /*   ... and its state                             */
    float  prev[DTMF_MAX_BLOCK];  /* the block before, filtered: windows overlap by half */
    int    prev_n;
    char   last_emitted; /* digit currently held down (0 = none)  */
    char   cand;         /* candidate digit being confirmed       */
    int    cand_count;   /* consecutive windows of `cand`         */
    int    hang;         /* trailing blocks still reported as tone (release edge) */
    int    silence;      /* consecutive windows without a digit (re-arm)          */
} dtmf_t;

void dtmf_init(dtmf_t *d, int sample_rate);

/* A new transmission: forget the digit that was held and the audio kept for
 * the overlapping window. Without this the same key, pressed first thing in
 * the next transmission, would be taken for the one still held. */
void dtmf_reset(dtmf_t *d);

/* Process one mono int16 block of `n` samples (20 ms). Returns the digit
 * character ('0'-'9','A'-'D','*','#') in the block that confirms a NEW digit —
 * 40 ms after it began — otherwise 0. *tone_present is true from that block on
 * for as long as the digit is held, and for the one block after it in which
 * the tone dies away. The two blocks before the confirmation carried the
 * beginning of the digit and were not flagged: something that looks like a
 * digit for a moment is not a digit yet — speech does that now and then. */
char dtmf_process(dtmf_t *d, const int16_t *pcm, int n, bool *tone_present);

#endif /* SMARTSPOT_DTMF_H */
