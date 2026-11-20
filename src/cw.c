/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "cw.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define EDGE_MS 5

static const char *morse_of(char c)
{
    switch (toupper((unsigned char)c)) {
    case 'A': return ".-";    case 'B': return "-...";  case 'C': return "-.-.";
    case 'D': return "-..";   case 'E': return ".";     case 'F': return "..-.";
    case 'G': return "--.";   case 'H': return "....";  case 'I': return "..";
    case 'J': return ".---";  case 'K': return "-.-";   case 'L': return ".-..";
    case 'M': return "--";    case 'N': return "-.";    case 'O': return "---";
    case 'P': return ".--.";  case 'Q': return "--.-";  case 'R': return ".-.";
    case 'S': return "...";   case 'T': return "-";     case 'U': return "..-";
    case 'V': return "...-";  case 'W': return ".--";   case 'X': return "-..-";
    case 'Y': return "-.--";  case 'Z': return "--..";
    case '0': return "-----"; case '1': return ".----"; case '2': return "..---";
    case '3': return "...--"; case '4': return "....-"; case '5': return ".....";
    case '6': return "-...."; case '7': return "--..."; case '8': return "---..";
    case '9': return "----.";
    case '/': return "-..-.";  case '.': return ".-.-.-"; case ',': return "--..--";
    case '?': return "..--.."; case '-': return "-....-"; case '=': return "-...-";
    default:  return NULL;
    }
}

int cw_sanitize(const char *in, char *out, int cap)
{
    int o = 0;
    for (; in && *in && o + 1 < cap; in++) {
        if (*in == ' ' ? (o > 0 && out[o - 1] != ' ') : morse_of(*in) != NULL)
            out[o++] = (char)toupper((unsigned char)*in);
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    if (cap > 0) out[o] = '\0';
    return o;
}

/* One walk over the text for both the length and the samples: `key` is called
 * for every stretch of tone or silence, in dot units. */
typedef void (*key_fn)(void *ctx, int units, int on);

static int walk(const char *text, key_fn key, void *ctx)
{
    int total = 0, gap = 0;         /* gap = silence owed before the next element */
    for (const char *p = text; p && *p; p++) {
        const char *m = morse_of(*p);
        if (!m) {                   /* a space, or something Morse has no code for */
            if (total > 0) gap = 7;
            continue;
        }
        for (const char *e = m; *e; e++) {
            if (gap) {
                if (key) key(ctx, gap, 0);
                total += gap;
            }
            int len = *e == '-' ? 3 : 1;
            if (key) key(ctx, len, 1);
            total += len;
            gap = 1;                /* between the elements of a character */
        }
        gap = 3;                    /* between characters */
    }
    return total;
}

int cw_units(const char *text) { return walk(text, NULL, NULL); }

typedef struct {
    int16_t *out;
    int      pos, dot, rate, edge;
    double   amp, w;
} render_t;

static void key_samples(void *ctx, int units, int on)
{
    render_t *r = ctx;
    int n = units * r->dot;
    if (on) {
        int edge = r->edge < n / 2 ? r->edge : n / 2;
        for (int i = 0; i < n; i++) {
            double env = 1.0;
            if (edge > 0) {
                if (i < edge)           env = 0.5 * (1.0 - cos(M_PI * i / edge));
                else if (i >= n - edge) env = 0.5 * (1.0 - cos(M_PI * (n - 1 - i) / edge));
            }
            /* the phase restarts with every element: each one begins at a zero crossing */
            r->out[r->pos + i] = (int16_t)lrint(sin(r->w * i) * r->amp * env);
        }
    }
    r->pos += n;                    /* silence: the buffer is already zero */
}

int16_t *cw_render(const char *text, int wpm, int pitch_hz, float level_db,
                   int sample_rate, int lead_ms, int tail_ms, int *n_out)
{
    if (n_out) *n_out = 0;
    if (wpm < 5) wpm = 5;
    if (wpm > 60) wpm = 60;
    if (pitch_hz < 200) pitch_hz = 200;
    if (pitch_hz > 3000) pitch_hz = 3000;
    if (level_db > 0.0f) level_db = 0.0f;
    int units = cw_units(text);
    if (units <= 0 || sample_rate <= 0)
        return NULL;

    render_t r;
    memset(&r, 0, sizeof r);
    r.rate = sample_rate;
    r.dot  = (int)lrint(1.2 * sample_rate / wpm);       /* 1200/wpm ms */
    r.edge = EDGE_MS * sample_rate / 1000;
    r.amp  = 32767.0 * pow(10.0, level_db / 20.0);
    r.w    = 2.0 * M_PI * pitch_hz / sample_rate;

    int lead = lead_ms > 0 ? lead_ms * sample_rate / 1000 : 0;
    int tail = tail_ms > 0 ? tail_ms * sample_rate / 1000 : 0;
    int n = lead + units * r.dot + tail;
    r.out = calloc((size_t)n, sizeof(int16_t));
    if (!r.out)
        return NULL;
    r.pos = lead;
    walk(text, key_samples, &r);
    if (n_out) *n_out = n;
    return r.out;
}
