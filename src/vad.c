/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "vad.h"

#include <math.h>
#include <string.h>

/* Pitch search range. 75 Hz (deep male voice) .. 400 Hz (child / high female). */
#define PITCH_MIN_HZ 75.0
#define PITCH_MAX_HZ 400.0

/* "Unsteady" test over the voiced frames in the window: real speech moves its
 * pitch lag by a few samples or its level by a few dB within a syllable or
 * two; hum, CTCSS leakage and DTMF do neither. */
#define TONE_COMB_PEAKS     6      /* >= this many equal autocorrelation peaks = a sine */
#define STEADY_LAG_SPREAD   3      /* samples */
#define STEADY_LEVEL_SPREAD 4.0f   /* dB      */

void vad_default_params(vad_params_t *p)
{
    p->sample_rate   = 16000;
    p->min_level_db  = -50.0f;
    p->voiced_thr    = 0.50f;
    p->tone_purity   = 0.90f;
    /* Five voiced frames. Squelch noise produces one in three thousand, a
     * dead carrier none; a single short word ("test", "break") has five or
     * six, and with seven asked for it was not heard. */
    p->min_voiced_ms = 100;
    p->window_ms     = 1000;
}

/* RBJ biquads, Butterworth sections. */
static void biquad_hp(double *c, double fs, double f0, double q)
{
    double w = 2.0 * M_PI * f0 / fs, cw = cos(w), al = sin(w) / (2.0 * q);
    double a0 = 1.0 + al;
    c[0] = (1.0 + cw) / 2.0 / a0;
    c[1] = -(1.0 + cw) / a0;
    c[2] = (1.0 + cw) / 2.0 / a0;
    c[3] = -2.0 * cw / a0;
    c[4] = (1.0 - al) / a0;
}

static void biquad_lp(double *c, double fs, double f0, double q)
{
    double w = 2.0 * M_PI * f0 / fs, cw = cos(w), al = sin(w) / (2.0 * q);
    double a0 = 1.0 + al;
    c[0] = (1.0 - cw) / 2.0 / a0;
    c[1] = (1.0 - cw) / a0;
    c[2] = (1.0 - cw) / 2.0 / a0;
    c[3] = -2.0 * cw / a0;
    c[4] = (1.0 - al) / a0;
}

void vad_init(vad_t *v, const vad_params_t *p)
{
    memset(v, 0, sizeof *v);
    v->p = *p;
    if (v->p.sample_rate <= 0) v->p.sample_rate = 16000;
    double fs = v->p.sample_rate;
    /* 4th-order Butterworth high-pass at 300 Hz (Q = 0.5412, 1.3066): a CTCSS
     * tone at 131.8 Hz is down ~29 dB, 250.3 Hz ~7 dB on top of the chip's own
     * high-pass. 2nd-order low-pass at 3 kHz. */
    biquad_hp(v->bq[0], fs, 300.0, 0.5412);
    biquad_hp(v->bq[1], fs, 300.0, 1.3066);
    biquad_lp(v->bq[2], fs, 3000.0, 0.7071);
    vad_reset(v);
}

void vad_reset(vad_t *v)
{
    v->hist_len = v->hist_pos = 0;
    /* The frame kept as "previous" is the end of the opening before this one,
     * however long ago that was. Analysed together with the first frame of
     * this opening it made that frame voiced whenever the last over had ended
     * on a vowel. */
    v->prev_n = 0;
    v->speech = false;
    v->elapsed_ms = 0;
    memset(&v->st, 0, sizeof v->st);
    v->st.speech_at_ms = -1;
    v->st.level_peak_db = -120.0f;
}

bool vad_speech(const vad_t *v) { return v->speech; }
const vad_stats_t *vad_stats(const vad_t *v) { return &v->st; }

const char *vad_describe(const vad_stats_t *st)
{
    if (st->speech_at_ms >= 0)
        return "speech";
    if (st->frames <= 0)
        return "silence";
    if (st->tone_frames * 2 > st->frames)
        return "tone";
    if (st->loud_frames * 2 < st->frames)
        return "silence";       /* quiet: a dead, full-quieting carrier */
    return "noise";             /* loud but no voice: weak signal / falsing */
}

static void bandpass(vad_t *v, const int16_t *in, float *out, int n)
{
    for (int i = 0; i < n; i++) {
        double x = (double)in[i] / 32768.0;
        for (int s = 0; s < 3; s++) {
            const double *c = v->bq[s];
            double *z = v->bz[s];
            double y = c[0] * x + z[0];
            z[0] = c[1] * x - c[3] * y + z[1];
            z[1] = c[2] * x - c[4] * y;
            x = y;
        }
        out[i] = (float)x;
    }
}

/* Decide from the sliding window whether speech has been heard. */
static bool window_says_speech(vad_t *v, int frame_ms, int *steady)
{
    int need = (v->p.min_voiced_ms + frame_ms - 1) / frame_ms;
    if (need < 1) need = 1;

    int voiced = 0, lag_min = 1 << 30;
    float lv_min = 1e9f, lv_max = -1e9f;
    for (int i = 0; i < v->hist_len; i++) {
        if (!v->hist[i].voiced) continue;
        voiced++;
        if (v->hist[i].lag < lag_min) lag_min = v->hist[i].lag;
        if (v->hist[i].level_db < lv_min) lv_min = v->hist[i].level_db;
        if (v->hist[i].level_db > lv_max) lv_max = v->hist[i].level_db;
    }
    *steady = 0;
    if (voiced < need)
        return false;
    /* Steady = every voiced frame sits on the same pitch (or, since a noisy
     * tone makes the pitch pick jump between octaves, on a whole multiple of
     * the shortest one) at the same loudness. */
    bool same_pitch = true;
    for (int i = 0; i < v->hist_len && same_pitch; i++) {
        if (!v->hist[i].voiced) continue;
        int k = (v->hist[i].lag + lag_min / 2) / lag_min;
        int off = v->hist[i].lag - k * lag_min;
        if (off < 0) off = -off;
        if (off >= STEADY_LAG_SPREAD + k - 1)
            same_pitch = false;
    }
    if (same_pitch && lv_max - lv_min < STEADY_LEVEL_SPREAD) {
        *steady = 1;            /* periodic but rock steady: hum / a tone */
        return false;
    }
    return true;
}

bool vad_process(vad_t *v, const int16_t *pcm, int n, vad_frame_t *out)
{
    return vad_process_ex(v, pcm, n, out, false);
}

bool vad_process_ex(vad_t *v, const int16_t *pcm, int n, vad_frame_t *out, bool known_tone)
{
    vad_frame_t f;
    memset(&f, 0, sizeof f);
    f.level_db = -120.0f;
    if (n <= 0 || n > VAD_MAX_FRAME) {
        if (out) *out = f;
        return v->speech;
    }
    int fs = v->p.sample_rate;
    int frame_ms = (n * 1000 + fs / 2) / fs;
    if (frame_ms < 1) frame_ms = 1;

    /* window = previous frame + this frame, band-limited */
    float *cur = v->win + v->prev_n;
    bandpass(v, pcm, cur, n);
    int N = v->prev_n + n;
    const float *x = v->win;

    double e_cur = 0.0;
    for (int i = 0; i < n; i++) e_cur += (double)cur[i] * cur[i];
    double rms = sqrt(e_cur / n);
    f.level_db = (float)(20.0 * log10(rms + 1e-9));
    f.loud = f.level_db >= v->p.min_level_db;

    if (f.loud && N >= n + n / 2) {
        /* --- pure-tone test: how much of the energy sits in ONE spectral line ---
         * Hann-windowed DFT of the 40 ms window, 25 Hz apart, over the audio
         * band. A sine — steady, or gliding like a whistle — has nearly all of
         * its energy within +-75 Hz of its peak; a vowel spreads it over its
         * harmonics. */
        {
            static _Thread_local float hann[2 * VAD_MAX_FRAME];
            static _Thread_local int hann_n;
            if (hann_n != N) {
                for (int i = 0; i < N; i++)
                    hann[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (float)(N - 1));
                hann_n = N;
            }
            double df = (double)fs / N;
            int k0 = (int)(250.0 / df), k1 = (int)(3200.0 / df);
            if (k1 > N / 2 - 1) k1 = N / 2 - 1;
            static _Thread_local double pw[2 * VAD_MAX_FRAME];
            double total = 0.0;
            int kp = k0;
            for (int k = k0; k <= k1; k++) {
                /* Goertzel on the windowed samples */
                double c = 2.0 * cos(2.0 * M_PI * k / N), s1 = 0.0, s2 = 0.0;
                for (int i = 0; i < N; i++) {
                    double s0 = (double)(x[i] * hann[i]) + c * s1 - s2;
                    s2 = s1;
                    s1 = s0;
                }
                pw[k] = s1 * s1 + s2 * s2 - c * s1 * s2;
                total += pw[k];
                if (pw[k] > pw[kp]) kp = k;
            }
            int span = (int)(75.0 / df + 0.5);
            double line = 0.0;
            for (int k = kp - span; k <= kp + span; k++)
                if (k >= k0 && k <= k1) line += pw[k];
            f.purity = total > 0.0 ? (float)(line / total) : 0.0f;
        }
        f.tone = f.purity >= v->p.tone_purity;

        /* --- pitch periodicity: normalised autocorrelation peak --- */
        int lmin = (int)(fs / PITCH_MAX_HZ);
        int lmax = (int)(fs / PITCH_MIN_HZ);
        if (lmax > N / 2) lmax = N / 2;
        /* prefix energies so each lag's normalisation is O(1) */
        static _Thread_local double pre[2 * VAD_MAX_FRAME + 1];
        pre[0] = 0.0;
        for (int i = 0; i < N; i++) pre[i + 1] = pre[i] + (double)x[i] * x[i];

        double best = 0.0;
        int best_lag = 0;
        static _Thread_local float rr[2 * VAD_MAX_FRAME];
        for (int L = lmin; L <= lmax; L++) {
            int M = N - L;
            double s = 0.0;
            for (int i = 0; i < M; i++) s += (double)x[i] * x[i + L];
            double ea = pre[M], eb = pre[N] - pre[L];
            double r = (ea > 0 && eb > 0) ? s / sqrt(ea * eb) : 0.0;
            rr[L] = (float)r;
            if (r > best) { best = r; best_lag = L; }
        }
        /* octave fix: take the shortest lag that is nearly as good as the best,
         * so a steady signal always reports the same lag */
        for (int L = lmin; L < best_lag; L++) {
            if (rr[L] >= 0.9 * best &&
                (L == lmin || rr[L] >= rr[L - 1]) && rr[L] >= rr[L + 1]) {
                best_lag = L;
                break;
            }
        }
        /* A voice has ONE pitch period in the search range, plus at most a
         * few multiples of it (4 at 400 Hz). A sine above ~550 Hz — a whistle,
         * a heterodyne, even one buried in noise — repeats many more times, so its autocorrelation shows a long
         * comb of equally good peaks. */
        int peaks = 0;
        for (int L = lmin + 1; L < lmax; L++)
            if (rr[L] >= 0.8 * best && rr[L] > rr[L - 1] && rr[L] >= rr[L + 1])
                peaks++;
        if (best >= v->p.voiced_thr && peaks >= TONE_COMB_PEAKS)
            f.tone = true;
        f.periodicity = (float)best;
        f.lag = best_lag;
        f.voiced = !f.tone && best >= v->p.voiced_thr;
    }

    if (known_tone) {
        f.voiced = false;
        f.tone = f.loud;
        /* ...and so were the two frames before it: the caller knows a digit
         * for what it is two frames after it began. */
        for (int back = 1; back <= 2 && v->hist_cap > 0 && back <= v->hist_len; back++) {
            int prev = (v->hist_pos + v->hist_cap - back) % v->hist_cap;
            if (v->hist[prev].voiced) {
                v->hist[prev].voiced = 0;
                if (v->st.voiced_frames > 0) v->st.voiced_frames--;
            }
        }
    }

    /* keep this frame as "previous" for the next call */
    memmove(v->win, cur, sizeof(float) * (size_t)n);
    v->prev_n = n;

    /* stats */
    v->st.frames++;
    v->st.level_sum_db += f.level_db;
    if (f.level_db > v->st.level_peak_db) v->st.level_peak_db = f.level_db;
    if (f.loud)   v->st.loud_frames++;
    if (f.voiced) v->st.voiced_frames++;
    if (f.tone)   v->st.tone_frames++;

    /* sliding window */
    int cap = v->p.window_ms / frame_ms;
    if (cap < 1) cap = 1;
    if (cap > VAD_MAX_HISTORY) cap = VAD_MAX_HISTORY;
    if (cap != v->hist_cap) { v->hist_cap = cap; v->hist_len = v->hist_pos = 0; }
    v->hist[v->hist_pos].voiced   = f.voiced;
    v->hist[v->hist_pos].lag      = (int16_t)f.lag;
    v->hist[v->hist_pos].level_db = f.level_db;
    v->hist_pos = (v->hist_pos + 1) % cap;
    if (v->hist_len < cap) v->hist_len++;

    v->elapsed_ms += frame_ms;
    if (!v->speech) {
        int steady = 0;
        if (window_says_speech(v, frame_ms, &steady)) {
            v->speech = true;
            v->st.speech_at_ms = v->elapsed_ms;
        } else if (steady && f.voiced) {
            v->st.steady_frames++;
        }
    }
    if (out) *out = f;
    return v->speech;
}
