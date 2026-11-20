/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — what is done to the audio itself.
 *
 * Two small processors, both pure (no I/O), both working on 20 ms frames:
 *
 *   agc_t      the normaliser on RECEIVED audio. Stations arrive anywhere
 *              between -55 and -15 dBFS depending on their deviation, their
 *              microphone and the receiver's sound card. Before the audio goes
 *              to the talkgroup (and to the repeater's transmitter) every over
 *              is brought to the same speech level, so that all repeaters on a
 *              reflector sound equally loud.
 *
 *   txshape_t  the last step before the transmitter's sound card: optional
 *              6 dB/octave pre-emphasis (for a radio whose own pre-emphasis is
 *              bypassed) and a limiter, so that nothing drives the modulator
 *              harder than `limit_db`.
 */
#ifndef TAILCHASER_AUDIOPROC_H
#define TAILCHASER_AUDIOPROC_H

#include <stdbool.h>
#include <stdint.h>

/* ---- the normaliser ---- */

typedef struct {
    bool  enabled;
    int   sample_rate;
    float target_db;        /* level the loud parts of speech are brought to (dBFS RMS) */
    float max_gain_db;      /* a weak station is amplified by this much at most        */
    float min_gain_db;      /* a hot one is attenuated by this much at most (negative) */
} agc_params_t;

typedef struct {
    agc_params_t p;
    bool   have_level;      /* a level was measured in this over        */
    float  level_db;        /* the speech level: follows the loud frames */
    float  gain_db;         /* gain aimed at                             */
    float  applied;         /* linear gain at the end of the last frame  */
    float  hp_x, hp_y;      /* measuring filter (hum, CTCSS leak)        */
    /* statistics of the over, for the log */
    double sum_in_db, sum_gain_db;
    int    measured;
} agc_t;

void  agc_default_params(agc_params_t *p);
void  agc_init(agc_t *a, const agc_params_t *p);
/* A new over: the level of the previous talker says nothing about this one.
 * The gain is kept as the starting point until the first loud frame. */
void  agc_begin_over(agc_t *a);
/* One frame, in place. */
void  agc_process(agc_t *a, int16_t *pcm, int n);
/* The same in two steps, for look-ahead: measure the newest frame, apply the
 * gain to one from a little earlier — the gain is already down when a loud
 * passage arrives, instead of a moment after. */
void  agc_measure(agc_t *a, const int16_t *pcm, int n);
void  agc_apply(agc_t *a, int16_t *pcm, int n);
/* Averages over the frames measured since agc_begin_over(); false if none. */
bool  agc_over_stats(const agc_t *a, float *in_db, float *gain_db);

/* ---- de-emphasis: for audio taken straight from a discriminator ----
 *
 * 6 dB per octave down from 300 Hz, with 1 kHz left where it was. */
typedef struct { float a, gain, y; } deemph_t;

void deemph_init(deemph_t *d, int sample_rate);
void deemph_process(deemph_t *d, int16_t *pcm, int n);

/* ---- a signal meter from the noise ----
 *
 * An FM receiver hisses, and the stronger the signal the less it does. Above
 * the speech band (5.5 kHz and up) there is nothing but that hiss, so its
 * level tells how strong the station is — for a receiver that has no RSSI
 * output. Wants the audio as the discriminator gives it: before any
 * de-emphasis or low-pass. */
typedef struct {
    float b0[2], b1[2], b2[2], a1[2], a2[2], z1[2], z2[2];
    float db;               /* smoothed level in the band, dBFS */
    bool  have;
} noisemeter_t;

void  noisemeter_init(noisemeter_t *m, int sample_rate);
float noisemeter_process(noisemeter_t *m, const int16_t *pcm, int n);     /* the smoothed level */
/* 0..100 between the level without a signal and the one at full quieting. */
float noisemeter_siglev(float db, float open_db, float full_db);

/* ---- a level meter: how loud was the speech of an over ----
 *
 * The RMS of the frames that carry sound (above -50 dBFS), pauses left out —
 * one number that compares an over from the receiver with one from the
 * talkgroup. */
typedef struct {
    double sum;             /* of the mean squares of the frames counted */
    int    frames;
    int    peak;
} lvl_t;

void  lvl_reset(lvl_t *l);
void  lvl_add(lvl_t *l, const int16_t *pcm, int n);
float lvl_db(const lvl_t *l);       /* dBFS RMS; -120 when nothing was heard */
float lvl_peak_db(const lvl_t *l);

/* ---- the transmit shaper ---- */

typedef struct {
    bool  preemphasis;
    float limit;            /* linear ceiling, 0..1 */
    float a, norm;          /* pre-emphasis coefficient and its 1 kHz normalisation */
    float x1;
} txshape_t;

void txshape_init(txshape_t *t, int sample_rate, bool preemphasis, float limit_db);
void txshape_process(txshape_t *t, int16_t *pcm, int n);

#endif /* TAILCHASER_AUDIOPROC_H */
