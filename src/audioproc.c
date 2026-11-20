/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "audioproc.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265358979f

/* ------------------------------------------------------------------ */
/* the normaliser                                                      */

#define AGC_FLOOR_DB     -65.0f     /* quieter than this is not a talker           */
#define AGC_WINDOW_DB     18.0f     /* frames this far under the level are pauses  */
#define AGC_ATTACK        0.35f     /* level follows louder frames: ~50 ms         */
#define AGC_RELEASE       0.02f     /* ... and quieter ones: ~1 s                  */
#define AGC_DOWN_DB        3.0f     /* gain steps per 20 ms frame: down fast,      */
#define AGC_UP_DB          0.25f    /*   up slowly (12 dB/s) — no pumping          */
#define AGC_KNEE           0.70f    /* soft clip above this share of full scale    */

void agc_default_params(agc_params_t *p)
{
    memset(p, 0, sizeof *p);
    p->enabled     = true;
    p->sample_rate = 16000;
    p->target_db   = -20.0f;
    p->max_gain_db = 30.0f;
    p->min_gain_db = -20.0f;
}

void agc_init(agc_t *a, const agc_params_t *p)
{
    memset(a, 0, sizeof *a);
    a->p = *p;
    a->applied = 1.0f;
}

void agc_begin_over(agc_t *a)
{
    a->have_level = false;
    a->sum_in_db = a->sum_gain_db = 0.0;
    a->measured = 0;
}

bool agc_over_stats(const agc_t *a, float *in_db, float *gain_db)
{
    if (a->measured == 0)
        return false;
    *in_db   = (float)(a->sum_in_db / a->measured);
    *gain_db = (float)(a->sum_gain_db / a->measured);
    return true;
}

void deemph_init(deemph_t *d, int sample_rate)
{
    d->a = 1.0f - expf(-2.0f * PI_F * 300.0f / (float)sample_rate);
    d->gain = sqrtf(1.0f + (1000.0f / 300.0f) * (1000.0f / 300.0f));
    d->y = 0.0f;
}

void deemph_process(deemph_t *d, int16_t *pcm, int n)
{
    float y = d->y;
    for (int i = 0; i < n; i++) {
        y += d->a * ((float)pcm[i] - y);
        float v = y * d->gain;
        pcm[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : lrintf(v));
    }
    d->y = y;
}

void noisemeter_init(noisemeter_t *m, int sample_rate)
{
    memset(m, 0, sizeof *m);
    /* 4th-order Butterworth high-pass at 5.5 kHz */
    static const double Q[2] = { 0.54119610, 1.30656296 };
    double w = 2.0 * 3.14159265358979 * 5500.0 / sample_rate, cs = cos(w);
    for (int s = 0; s < 2; s++) {
        double al = sin(w) / (2.0 * Q[s]), a0 = 1.0 + al;
        m->b0[s] = m->b2[s] = (float)((1.0 + cs) / 2.0 / a0);
        m->b1[s] = (float)(-(1.0 + cs) / a0);
        m->a1[s] = (float)(-2.0 * cs / a0);
        m->a2[s] = (float)((1.0 - al) / a0);
    }
    m->db = -120.0f;
}

float noisemeter_process(noisemeter_t *m, const int16_t *pcm, int n)
{
    double acc = 0.0;
    for (int i = 0; i < n; i++) {
        float x = (float)pcm[i] / 32768.0f;
        for (int s = 0; s < 2; s++) {
            float y = m->b0[s] * x + m->z1[s];
            m->z1[s] = m->b1[s] * x - m->a1[s] * y + m->z2[s];
            m->z2[s] = m->b2[s] * x - m->a2[s] * y;
            x = y;
        }
        acc += (double)x * x;
    }
    float db = 10.0f * log10f((float)(acc / (n > 0 ? n : 1)) + 1e-12f);
    if (!m->have) { m->db = db; m->have = true; }
    else          m->db += (db - m->db) * 0.15f;        /* about 130 ms at 20 ms frames */
    return m->db;
}

float noisemeter_siglev(float db, float open_db, float full_db)
{
    float v = 100.0f * (open_db - db) / (open_db - full_db);
    return v < 0.0f ? 0.0f : v > 100.0f ? 100.0f : v;
}

void lvl_reset(lvl_t *l) { memset(l, 0, sizeof *l); }

void lvl_add(lvl_t *l, const int16_t *pcm, int n)
{
    if (n <= 0)
        return;
    double acc = 0.0;
    for (int i = 0; i < n; i++) {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > l->peak) l->peak = v;
        acc += (double)pcm[i] * pcm[i];
    }
    acc /= (double)n * 32768.0 * 32768.0;
    if (acc > 1e-5) {                   /* -50 dBFS */
        l->sum += acc;
        l->frames++;
    }
}

float lvl_db(const lvl_t *l)
{
    return l->frames > 0 ? 10.0f * log10f((float)(l->sum / l->frames)) : -120.0f;
}

float lvl_peak_db(const lvl_t *l)
{
    return l->peak > 0 ? 20.0f * log10f((float)l->peak / 32768.0f) : -120.0f;
}

/* Everything above the knee is bent towards full scale, never over it. */
static inline float soft_clip(float v)
{
    float m = fabsf(v);
    if (m <= AGC_KNEE)
        return v;
    float y = AGC_KNEE + (1.0f - AGC_KNEE) * tanhf((m - AGC_KNEE) / (1.0f - AGC_KNEE));
    return v < 0 ? -y : y;
}

void agc_process(agc_t *a, int16_t *pcm, int n)
{
    if (!a->p.enabled || n <= 0)
        return;

    /* ---- how loud is this frame (above ~200 Hz) ---- */
    float k = expf(-2.0f * PI_F * 200.0f / (float)a->p.sample_rate);
    double acc = 0.0;
    float x1 = a->hp_x, y1 = a->hp_y;
    for (int i = 0; i < n; i++) {
        float x = (float)pcm[i] / 32768.0f;
        float y = k * (y1 + x - x1);
        x1 = x;
        y1 = y;
        acc += (double)y * y;
    }
    a->hp_x = x1;
    a->hp_y = y1;
    float frame_db = 10.0f * log10f((float)(acc / n) + 1e-12f);

    /* ---- the speech level: the loud frames of this over ---- */
    bool first = false;
    if (frame_db > AGC_FLOOR_DB && (!a->have_level || frame_db > a->level_db - AGC_WINDOW_DB)) {
        if (!a->have_level) {
            a->level_db = frame_db;
            a->have_level = true;
            first = true;
        } else {
            float c = frame_db > a->level_db ? AGC_ATTACK : AGC_RELEASE;
            a->level_db += (frame_db - a->level_db) * c;
        }
        float want = a->p.target_db - a->level_db;
        if (want > a->p.max_gain_db) want = a->p.max_gain_db;
        if (want < a->p.min_gain_db) want = a->p.min_gain_db;
        if (first)                          /* a new talker: no ramp from the last one */
            a->gain_db = want;
        else if (want < a->gain_db)
            a->gain_db = want < a->gain_db - AGC_DOWN_DB ? a->gain_db - AGC_DOWN_DB : want;
        else
            a->gain_db = want > a->gain_db + AGC_UP_DB ? a->gain_db + AGC_UP_DB : want;
        a->sum_in_db   += frame_db;
        a->sum_gain_db += a->gain_db;
        a->measured++;
    }

    /* ---- apply, ramped across the frame so a gain step never clicks ---- */
    float g0 = a->applied, g1 = powf(10.0f, a->gain_db / 20.0f);
    for (int i = 0; i < n; i++) {
        float g = g0 + (g1 - g0) * (float)(i + 1) / (float)n;
        float v = soft_clip((float)pcm[i] / 32768.0f * g) * 32767.0f;
        pcm[i] = (int16_t)lrintf(v);
    }
    a->applied = g1;
}

/* ------------------------------------------------------------------ */
/* the transmit shaper                                                 */

void txshape_init(txshape_t *t, int sample_rate, bool preemphasis, float limit_db)
{
    memset(t, 0, sizeof *t);
    t->preemphasis = preemphasis;
    t->limit = powf(10.0f, limit_db / 20.0f);
    if (t->limit > 1.0f) t->limit = 1.0f;
    /* y[n] = x[n] - a x[n-1]: rises 6 dB/octave from a 300 Hz corner, scaled to
     * unity at 1 kHz (-8 dB at 300 Hz, +9 dB at 3 kHz at 16 kHz sampling). */
    t->a = expf(-2.0f * PI_F * 300.0f / (float)sample_rate);
    float w = 2.0f * PI_F * 1000.0f / (float)sample_rate;
    t->norm = 1.0f / sqrtf(1.0f + t->a * t->a - 2.0f * t->a * cosf(w));
}

void txshape_process(txshape_t *t, int16_t *pcm, int n)
{
    if (!t->preemphasis && t->limit >= 1.0f)
        return;                             /* nothing to do: the audio is not touched */
    float lim = t->limit, knee = 0.8f * lim;
    for (int i = 0; i < n; i++) {
        float x = (float)pcm[i] / 32768.0f, v = x;
        if (t->preemphasis) {
            v = (x - t->a * t->x1) * t->norm;
            t->x1 = x;
        }
        float m = fabsf(v);
        if (m > knee) {
            float y = knee + (lim - knee) * tanhf((m - knee) / (lim - knee));
            v = v < 0 ? -y : y;
        }
        pcm[i] = (int16_t)lrintf(v * 32767.0f);
    }
}
