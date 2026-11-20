/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — speech detector ("is somebody actually talking?").
 *
 * The SA818 squelch tells us a carrier (or noise the chip mistook for one) is
 * present. It says nothing about whether there is a voice on it. This module
 * answers that from the received audio alone, frame by frame, so the squelch
 * gate (gate.c) can keep the repeater closed for dead carriers, kerchunks and
 * noise bursts.
 *
 * What it listens for is VOICED SPEECH — the vowels. A human voice is the only
 * thing on an FM channel that is, all at once:
 *
 *   1. periodic at a pitch between ~75 and ~400 Hz  (vocal-cord vibration);
 *      measured as the peak of the normalised autocorrelation of a 40 ms
 *      window over lags of 2.5..13.3 ms. Squelch noise / a weak carrier is
 *      aperiodic, a full-quieting dead carrier is silent.
 *   2. harmonically rich — NOT a single sine. A 1750 Hz burst, a whistle or a
 *      heterodyne is also periodic, but it has nearly all of its energy in one
 *      spectral line, and (above ~550 Hz) its autocorrelation shows a long
 *      comb of equal peaks where a voice shows one pitch peak.
 *   3. unsteady — pitch and loudness wander from syllable to syllable. Mains
 *      hum, a leaking CTCSS tone or DTMF are periodic but rock steady.
 *
 * All three are evaluated on audio band-limited to 300..3000 Hz, which also
 * removes any CTCSS tone the chip's high-pass let through.
 *
 * Speech is declared once `min_voiced_ms` of voiced frames have accumulated
 * inside a sliding `window_ms` and they are not steady. The verdict latches
 * until vad_reset().
 */
#ifndef SVXRX_VAD_H
#define SVXRX_VAD_H

#include <stdbool.h>
#include <stdint.h>

#define VAD_MAX_FRAME   960     /* samples per frame we accept (60 ms @ 16 kHz) */
#define VAD_MAX_HISTORY 256     /* sliding-window length, frames               */

typedef struct {
    int   sample_rate;      /* Hz (16000)                                        */
    float min_level_db;     /* frames quieter than this (dBFS, 300-3000 Hz) are  */
                            /*   never voiced: full-quieting carrier / silence   */
    float voiced_thr;       /* autocorrelation peak needed to call a frame voiced */
    float tone_purity;      /* share of energy in one spectral line = pure tone  */
    int   min_voiced_ms;    /* voiced audio needed inside the window             */
    int   window_ms;        /* sliding window the voiced audio must fall in      */
} vad_params_t;

/* What one frame looked like (for logging / the status page / tuning). */
typedef struct {
    float level_db;         /* band-limited RMS, dBFS                 */
    float periodicity;      /* autocorrelation peak, 0..1             */
    float purity;           /* share of energy in the strongest line  */
    int   lag;              /* pitch lag in samples (0 = none)        */
    bool  loud;             /* level >= min_level_db                  */
    bool  voiced;           /* loud + periodic + not a tone           */
    bool  tone;             /* loud + single sine                     */
} vad_frame_t;

/* Running totals since the last vad_reset() — one squelch opening. */
typedef struct {
    int    frames;
    int    loud_frames;
    int    voiced_frames;
    int    tone_frames;
    int    steady_frames;   /* voiced frames discarded as hum/steady tone pairs */
    double level_sum_db;    /* sum of level_db over all frames                  */
    float  level_peak_db;
    int    speech_at_ms;    /* when speech was declared (-1 = never)            */
} vad_stats_t;

typedef struct {
    vad_params_t p;
    int   frame_ms_x10;     /* last frame length, 0.1 ms units (set per call)   */

    /* band-pass: two cascaded high-pass biquads + one low-pass biquad */
    double bq[3][5];        /* b0 b1 b2 a1 a2 */
    double bz[3][2];        /* state          */

    float win[2 * VAD_MAX_FRAME];   /* previous + current filtered frame */
    int   prev_n;                   /* valid samples of the previous frame */

    /* sliding window of per-frame results */
    struct { uint8_t voiced; int16_t lag; float level_db; } hist[VAD_MAX_HISTORY];
    int   hist_len, hist_pos, hist_cap;

    bool  speech;           /* latched verdict */
    int   elapsed_ms;
    vad_stats_t st;
} vad_t;

void vad_default_params(vad_params_t *p);
void vad_init(vad_t *v, const vad_params_t *p);

/* Start a new observation (a new squelch opening). Clears the verdict, the
 * window, the stats and the frame held over from the last opening; keeps the
 * filter state running (the sound card's DC offset has not changed, and
 * starting from zero would put a step into the first frame). */
void vad_reset(vad_t *v);

/* Feed one frame of n mono samples (n <= VAD_MAX_FRAME; normally 20 ms).
 * Returns the latched verdict: true once speech has been detected. `out` may be
 * NULL. */
bool vad_process(vad_t *v, const int16_t *pcm, int n, vad_frame_t *out);

/* The same, for a frame the caller knows to be signalling — a DTMF digit — and
 * not a voice: it is counted as a tone, and the two frames before it are no
 * longer counted as voiced, because they carried the beginning of the same
 * digit. (A row of DTMF digits wanders in "pitch" from digit to digit much
 * like speech does.) */
bool vad_process_ex(vad_t *v, const int16_t *pcm, int n, vad_frame_t *out, bool known_tone);

bool               vad_speech(const vad_t *v);
const vad_stats_t *vad_stats(const vad_t *v);

/* Name what an opening that never produced speech most looked like:
 * "silence" (dead carrier), "noise" (weak signal / squelch falsing), "tone". */
const char *vad_describe(const vad_stats_t *st);

#endif /* SVXRX_VAD_H */
