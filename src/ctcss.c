/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "ctcss.h"

#include <math.h>
#include <string.h>

#define PI_D 3.14159265358979323846

/* The tone against the bins right next to it: a tone that is 1 Hz off still
 * passes, the next one in the table (2.5 Hz and more) does not. */
#define SEL_OPEN_DB   6.0f
#define SEL_CLOSE_DB  3.0f

void ctcss_default_params(ctcss_params_t *p)
{
    memset(p, 0, sizeof *p);
    p->sample_rate  = 16000;
    p->frame        = 320;
    p->tone_hz      = 88.5f;
    p->window_ms    = 300;
    p->delay_ms     = 300;
    p->open_snr_db  = 15.0f;
    p->close_snr_db = 8.0f;
    p->min_level_db = -50.0f;
    p->open_ms      = 60;
    p->close_ms     = 160;
    p->highpass     = true;
}

/* Butterworth high-pass section (RBJ). */
static void hp_design(ctcss_biquad_t *q, double fc, double rate, double Q)
{
    double w = 2.0 * PI_D * fc / rate, cs = cos(w), al = sin(w) / (2.0 * Q);
    double a0 = 1.0 + al;
    q->b0 = (float)((1.0 + cs) / 2.0 / a0);
    q->b1 = (float)(-(1.0 + cs) / a0);
    q->b2 = q->b0;
    q->a1 = (float)(-2.0 * cs / a0);
    q->a2 = (float)((1.0 - al) / a0);
    q->z1 = q->z2 = 0.0f;
}

void ctcss_init(ctcss_t *c, const ctcss_params_t *p)
{
    memset(c, 0, sizeof *c);
    c->p = *p;
    if (c->p.frame > CTCSS_MAX_FRAME) c->p.frame = CTCSS_MAX_FRAME;
    int frame_ms = c->p.frame * 1000 / c->p.sample_rate;
    if (frame_ms < 1) frame_ms = 1;
    c->window = c->p.window_ms / frame_ms;
    if (c->window < 4) c->window = 4;
    if (c->window > CTCSS_MAX_WINDOW) c->window = CTCSS_MAX_WINDOW;
    c->delay = c->p.delay_ms / frame_ms;
    if (c->delay < 0) c->delay = 0;
    if (c->delay > CTCSS_MAX_DELAY) c->delay = CTCSS_MAX_DELAY;

    /* the tone, and the bins on both sides: a steady tone in the middle
     * leaves nothing there once the window is full of it */
    double bin = (double)c->p.sample_rate / ((double)c->window * c->p.frame);
    static const double off[CTCSS_BINS] = { 0.0, -1.0, 1.0, -2.0, 2.0, -3.0, 3.0, -4.0, 4.0, -5.0, 5.0 };
    for (int k = 0; k < CTCSS_BINS; k++)
        c->step[k] = 2.0 * PI_D * ((double)c->p.tone_hz + off[k] * bin) / c->p.sample_rate;

    hp_design(&c->hp[0], 300.0, c->p.sample_rate, 0.54119610);
    hp_design(&c->hp[1], 300.0, c->p.sample_rate, 1.30656296);
    c->level_db = c->open_level_db = -120.0f;
}

bool ctcss_process(ctcss_t *c, int16_t *pcm, bool line)
{
    int n = c->p.frame;

    /* ---- this frame against the five frequencies ---- */
    for (int k = 0; k < CTCSS_BINS; k++) {
        double cr = cos(c->phase[k]), ci = sin(c->phase[k]);
        double dr = cos(c->step[k]), di = sin(c->step[k]);
        double sr = 0.0, si = 0.0;
        for (int i = 0; i < n; i++) {
            sr += pcm[i] * cr;
            si -= pcm[i] * ci;
            double t = cr * dr - ci * di;
            ci = cr * di + ci * dr;
            cr = t;
        }
        c->re[c->pos][k] = (float)sr;
        c->im[c->pos][k] = (float)si;
        c->phase[k] = fmod(c->phase[k] + c->step[k] * n, 2.0 * PI_D);
    }
    c->pos = (c->pos + 1) % c->window;

    /* ---- the window: the tone, and what lies next to it ---- */
    double pw[CTCSS_BINS];
    for (int k = 0; k < CTCSS_BINS; k++) {
        double sr = 0.0, si = 0.0;
        for (int w = 0; w < c->window; w++) {
            sr += c->re[w][k];
            si += c->im[w][k];
        }
        pw[k] = sr * sr + si * si;
    }
    double total = (double)c->window * n;
    double amp = 2.0 * sqrt(pw[0]) / total / 32768.0;
    double ref = 0.0;
    for (int k = 3; k < CTCSS_BINS; k++) ref += pw[k];
    ref /= CTCSS_BINS - 3;
    double next = pw[1] > pw[2] ? pw[1] : pw[2];
    c->level_db = (float)(20.0 * log10(amp + 1e-9));
    c->snr_db = (float)(10.0 * log10((pw[0] + 1.0) / (ref + 1.0)));
    c->sel_db = (float)(10.0 * log10((pw[0] + 1.0) / (next + 1.0)));
    if (c->snr_db > 60.0f) c->snr_db = 60.0f;

    /* ---- found, or gone ---- */
    int frame_ms = n * 1000 / c->p.sample_rate;
    if (line) c->line_hold = c->delay + 1;
    else if (c->line_hold > 0) c->line_hold--;
    bool present = c->level_db >= c->p.min_level_db &&
                   c->snr_db >= (c->open ? c->p.close_snr_db : c->p.open_snr_db) &&
                   c->sel_db >= (c->open ? SEL_CLOSE_DB : SEL_OPEN_DB) &&
                   c->line_hold > 0;
    if (present) { c->good++; c->bad = 0; }
    else         { c->bad++;  c->good = 0; }
    if (!c->open && c->good * frame_ms >= c->p.open_ms) {
        c->open = true;
        c->open_level_db = c->level_db;
        c->open_snr_db = c->snr_db;
    } else if (c->open && c->bad * frame_ms >= c->p.close_ms) {
        c->open = false;
    }

    /* ---- the audio, as late as the verdict ---- */
    if (c->delay > 0) {
        int16_t tmp[CTCSS_MAX_FRAME];
        memcpy(tmp, c->line[c->line_pos], sizeof(int16_t) * (size_t)n);
        memcpy(c->line[c->line_pos], pcm, sizeof(int16_t) * (size_t)n);
        memcpy(pcm, tmp, sizeof(int16_t) * (size_t)n);
        c->line_pos = (c->line_pos + 1) % c->delay;
    }
    if (c->p.highpass) {
        for (int s = 0; s < 2; s++) {
            ctcss_biquad_t *q = &c->hp[s];
            for (int i = 0; i < n; i++) {
                float x = (float)pcm[i];
                float y = q->b0 * x + q->z1;
                q->z1 = q->b1 * x - q->a1 * y + q->z2;
                q->z2 = q->b2 * x - q->a2 * y;
                pcm[i] = (int16_t)(y > 32767.0f ? 32767 : y < -32768.0f ? -32768 : lrintf(y));
            }
        }
    }
    return c->open;
}
