/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "tag.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define TAPS_HALF   64                          /* the low-pass: 129 taps      */
#define SYM_LEN     (TAG_CHIPS * TAG_SPC)       /* 256 samples                 */
#define BASIS_LEN   (SYM_LEN + 2 * TAPS_HALF)   /* a symbol and what the filter adds */
#define NSYNC       16
#define NBITS       148
#define CARRIER_CYCLES 4330                     /* per frame: 1650.15 Hz, frames join without a step */

static const char ALPHA[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-/";
static const uint16_t SYNCWORD = 0x9A6D;        /* bit k: symbol k against k-1 */

struct tag {
    float  *basis;              /* TAG_SYMBOLS x BASIS_LEN: each symbol's chips, filtered */
    float  *carrier;            /* TAG_FRAME: the cosine                                  */
    float  *frame;              /* TAG_FRAME: the frame on the air                         */
    float   rms;
    int     pos;
    bool    was_keyed;

    pthread_mutex_t mu;         /* the message that waits for the next frame boundary */
    int8_t  next[TAG_SYMBOLS];
    bool    fresh;
    char    call[TAG_CALL_MAX + 1];
    bool    local, have;
};

/* ---- the message ---- */

static void crc16(const uint8_t *bits, int n, uint8_t out[16])
{
    unsigned r = 0xFFFF;
    for (int i = 0; i < n; i++) {
        r ^= (unsigned)bits[i] << 15;
        r = (r & 0x8000) ? ((r << 1) ^ 0x1021) & 0xFFFF : (r << 1) & 0xFFFF;
    }
    for (int i = 0; i < 16; i++)
        out[i] = (r >> (15 - i)) & 1;
}

static int parity7(unsigned v)
{
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (int)(v & 1);
}

/* Upper case, at most nine characters, what the alphabet does not have
 * becomes a slash; padded with spaces. */
static void clean_call(const char *call, char out[TAG_CALL_MAX + 1])
{
    int n = 0;
    for (const char *p = call ? call : ""; *p && n < TAG_CALL_MAX; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c == ' ') continue;
        out[n++] = strchr(ALPHA, c) ? c : '/';
    }
    while (n < TAG_CALL_MAX) out[n++] = ' ';
    out[TAG_CALL_MAX] = '\0';
}

void tag_signs(const char *call, bool local, int8_t signs[TAG_SYMBOLS])
{
    char c9[TAG_CALL_MAX + 1];
    clean_call(call, c9);
    uint64_t v = 0;
    for (int i = 0; i < TAG_CALL_MAX; i++)
        v = v * 40 + (uint64_t)(strchr(ALPHA, c9[i]) - ALPHA);

    uint8_t msg[68 + 6];
    for (int i = 0; i < 48; i++)
        msg[i] = (uint8_t)((v >> (47 - i)) & 1);
    msg[48] = local ? 1 : 0;
    msg[49] = msg[50] = msg[51] = 0;
    crc16(msg, 52, msg + 52);
    memset(msg + 68, 0, 6);                     /* the tail of the code */

    uint8_t coded[NBITS], il[NBITS];
    unsigned sr = 0;
    for (int i = 0; i < 74; i++) {
        sr = ((sr << 1) | msg[i]) & 0x7F;
        coded[2 * i]     = (uint8_t)parity7(sr & 0171);
        coded[2 * i + 1] = (uint8_t)parity7(sr & 0133);
    }
    for (int i = 0; i < NBITS; i++)
        il[(i * 59) % NBITS] = coded[i];

    int8_t s = 1;
    signs[0] = 1;                               /* the reference */
    for (int k = 1; k < TAG_SYMBOLS; k++) {
        int bit = k < NSYNC ? (SYNCWORD >> k) & 1 : il[k - NSYNC];
        if (bit) s = (int8_t)-s;
        signs[k] = s;
    }
}

/* ---- the waveform ---- */

static void build_frame(tag_t *t, const int8_t signs[TAG_SYMBOLS], float *out)
{
    memset(out, 0, sizeof(float) * TAG_FRAME);
    for (int k = 0; k < TAG_SYMBOLS; k++) {
        const float *b = t->basis + (size_t)k * BASIS_LEN;
        int at = k * SYM_LEN - TAPS_HALF;
        float s = (float)signs[k];
        for (int i = 0; i < BASIS_LEN; i++) {
            int n = at + i;
            if (n < 0) n += TAG_FRAME;
            else if (n >= TAG_FRAME) n -= TAG_FRAME;    /* frames follow each other: a ring */
            out[n] += s * b[i];
        }
    }
    double acc = 0.0;
    for (int n = 0; n < TAG_FRAME; n++) {
        out[n] *= t->carrier[n];
        acc += (double)out[n] * out[n];
    }
    float g = t->rms / (float)sqrt(acc / TAG_FRAME + 1e-20);
    for (int n = 0; n < TAG_FRAME; n++)
        out[n] *= g;
}

tag_t *tag_new(float rms)
{
    tag_t *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->basis   = malloc(sizeof(float) * (size_t)TAG_SYMBOLS * BASIS_LEN);
    t->carrier = malloc(sizeof(float) * TAG_FRAME);
    t->frame   = calloc(TAG_FRAME, sizeof(float));
    if (!t->basis || !t->carrier || !t->frame) {
        tag_free(t);
        return NULL;
    }
    t->rms = rms;
    pthread_mutex_init(&t->mu, NULL);

    /* The low-pass of the chips, +-1150 Hz: the response 1/(1+(f/1150)^12),
     * no phase, cut to 129 taps under a Hann window. */
    float h[2 * TAPS_HALF + 1];
    const int N = 1024;
    for (int n = -TAPS_HALF; n <= TAPS_HALF; n++) {
        double acc = 0.0;
        for (int k = -N / 2; k < N / 2; k++) {
            double f = (double)k * TAG_RATE / N;
            double g = 1.0 / (1.0 + pow(fabs(f) / 1150.0, 12.0));
            acc += g * cos(2.0 * M_PI * (double)k * (double)n / N);
        }
        double w = 0.5 + 0.5 * cos(M_PI * (double)n / (TAPS_HALF + 1));
        h[n + TAPS_HALF] = (float)(acc / N * w);
    }

    /* The chips of the whole frame: one shift register sequence, so that
     * nothing repeats inside a frame. */
    unsigned r = 0x1ACE5;
    for (int k = 0; k < TAG_SYMBOLS; k++) {
        float chips[SYM_LEN];
        for (int c = 0; c < TAG_CHIPS; c++) {
            unsigned bit = (r ^ (r >> 2) ^ (r >> 3) ^ (r >> 5)) & 1;
            r = (r >> 1) | (bit << 16);
            float v = (r & 1) ? 1.0f : -1.0f;
            for (int i = 0; i < TAG_SPC; i++)
                chips[c * TAG_SPC + i] = v;
        }
        float *b = t->basis + (size_t)k * BASIS_LEN;
        for (int i = 0; i < BASIS_LEN; i++) {           /* output sample i is input sample i - TAPS_HALF */
            float acc = 0.0f;
            for (int j = 0; j <= 2 * TAPS_HALF; j++) {
                int src = i - j;                        /* chips[src] * h[j], centred */
                if (src >= 0 && src < SYM_LEN)
                    acc += chips[src] * h[j];
            }
            b[i] = acc;
        }
    }
    for (int n = 0; n < TAG_FRAME; n++)
        t->carrier[n] = (float)cos(2.0 * M_PI * CARRIER_CYCLES * (double)n / TAG_FRAME);
    return t;
}

void tag_free(tag_t *t)
{
    if (!t)
        return;
    free(t->basis);
    free(t->carrier);
    free(t->frame);
    pthread_mutex_destroy(&t->mu);
    free(t);
}

void tag_set(tag_t *t, const char *call, bool local)
{
    if (!t)
        return;
    char c9[TAG_CALL_MAX + 1];
    clean_call(call, c9);
    if (t->have && t->local == local && !strcmp(t->call, c9))
        return;                                 /* the engine says it every frame */
    int8_t s[TAG_SYMBOLS];
    tag_signs(c9, local, s);
    pthread_mutex_lock(&t->mu);
    memcpy(t->next, s, sizeof s);
    t->fresh = true;
    pthread_mutex_unlock(&t->mu);
    memcpy(t->call, c9, sizeof c9);
    t->local = local;
    t->have = true;
}

void tag_mix(tag_t *t, int16_t *pcm, int n, bool keyed)
{
    if (!t)
        return;
    if (!keyed) {
        t->was_keyed = false;
        return;
    }
    if (!t->was_keyed) {                        /* the frame clock starts with the carrier */
        t->was_keyed = true;
        t->pos = 0;
    }
    for (int i = 0; i < n; i++) {
        if (t->pos == 0) {                      /* a frame boundary: is there another message? */
            int8_t s[TAG_SYMBOLS];
            bool take = false;
            if (pthread_mutex_trylock(&t->mu) == 0) {
                if (t->fresh) {
                    memcpy(s, t->next, sizeof s);
                    t->fresh = false;
                    take = true;
                }
                pthread_mutex_unlock(&t->mu);
            }
            if (take)
                build_frame(t, s, t->frame);
        }
        float v = (float)pcm[i] + t->frame[t->pos];
        pcm[i] = v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int16_t)lrintf(v);
        if (++t->pos >= TAG_FRAME)
            t->pos = 0;
    }
}
