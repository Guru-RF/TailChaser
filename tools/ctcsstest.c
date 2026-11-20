/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* ctcsstest — run the tone squelch over a WAV file, offline.
 *
 *   ctcsstest [-f tone_hz] [-w window_ms] [-d delay_ms] [-o out.wav] file.wav
 *
 * Prints every opening (from, to, in seconds of the output) with the tone
 * level and signal-to-noise ratio it opened with, and the strongest tone seen
 * in the file — to set [ctcss] from a recording of the real receiver. With -o
 * what would be passed on is written as a WAV (closed squelch = silence).
 */
#include "audioproc.h"
#include "ctcss.h"
#include "wav.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    ctcss_params_t p;
    ctcss_default_params(&p);
    const char *out = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "f:w:d:o:")) != -1) {
        switch (opt) {
        case 'f': p.tone_hz = (float)atof(optarg); break;
        case 'w': p.window_ms = atoi(optarg); break;
        case 'd': p.delay_ms = atoi(optarg); break;
        case 'o': out = optarg; break;
        default:
            fprintf(stderr, "usage: %s [-f tone_hz] [-w window_ms] [-d delay_ms] [-o out.wav] file.wav\n", argv[0]);
            return 2;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "%s: no file\n", argv[0]);
        return 2;
    }
    int n = 0, rate = 0;
    int16_t *pcm = wav_load(argv[optind], &n, &rate);
    if (!pcm) {
        fprintf(stderr, "%s: cannot read %s\n", argv[0], argv[optind]);
        return 1;
    }
    p.sample_rate = rate;
    p.frame = rate / 50;
    ctcss_t c;
    ctcss_init(&c, &p);
    noisemeter_t nm;
    noisemeter_init(&nm, rate, 5500);
    double n_open = 0.0, n_closed = 0.0, lo = 0.0;
    long c_open = 0, c_closed = 0;
    bool open = false;
    int openings = 0, from = 0;
    float best_level = -120.0f, best_snr = -120.0f;
    for (int off = 0; off + p.frame <= n; off += p.frame) {
        float ndb = noisemeter_process(&nm, pcm + off, p.frame);
        bool o = ctcss_process(&c, pcm + off, true);
        /* the tone is there now (not: the delayed squelch) — and well away from the edges */
        bool tone = c.snr_db >= 25.0f && c.level_db >= p.min_level_db;
        if (tone)                   { n_open += ndb; c_open++; if (ndb < lo) lo = ndb; }
        else if (c.snr_db < 6.0f && !o) { n_closed += ndb; c_closed++; }
        if (c.snr_db > best_snr) { best_snr = c.snr_db; best_level = c.level_db; }
        if (o && !open) {
            from = off;
            openings++;
        } else if (!o && open) {
            printf("open %.2f to %.2f s  level %.1f dBFS  snr %.1f dB\n", (double)from / rate,
                   (double)off / rate, (double)c.open_level_db, (double)c.open_snr_db);
        }
        if (!o) memset(pcm + off, 0, sizeof(int16_t) * (size_t)p.frame);
        open = o;
    }
    if (open)
        printf("open %.2f to %.2f s  level %.1f dBFS  snr %.1f dB\n", (double)from / rate,
               (double)n / rate, (double)c.open_level_db, (double)c.open_snr_db);
    printf("openings %d  best snr %.1f dB at level %.1f dBFS  (%.1f Hz)\n", openings,
           (double)best_snr, (double)best_level, (double)p.tone_hz);
    if (c_open > 0 && c_closed > 0)
        printf("noise above 5.5 kHz: %.1f dBFS without a signal, %.1f dBFS with the tone (lowest %.1f)"
               " — [siglev] noise_open_db / noise_full_db\n", n_closed / (double)c_closed,
               n_open / (double)c_open, lo);
    if (out && wav_save(out, pcm, n, rate) != 0)
        fprintf(stderr, "%s: cannot write %s\n", argv[0], out);
    free(pcm);
    return 0;
}
