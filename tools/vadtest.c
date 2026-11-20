/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* vadtest — run the speech detector over a WAV file, offline.
 *
 *   vadtest [-v] [-c chunk_ms] [-l min_level_db] [-t voiced_thr] [-m min_voiced_ms]
 *           [-g tone_purity] file.wav [...]
 *
 * The WAV must be 16-bit PCM; the first channel is used. Without -c the whole
 * file is one squelch opening. With -c the file is cut into openings of
 * chunk_ms each (detector reset in between) and the tool reports how many of
 * them were judged speech — the way to measure false accepts on a long noise
 * recording, or the hit rate on a long speech recording.
 *
 * -v prints one line per 20 ms frame: time, level, periodicity, spectral purity, lag,
 * flags (L loud, V voiced, T tone, S speech latched).
 */
#include "vad.h"
#include "wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    vad_params_t p;
    vad_default_params(&p);
    int verbose = 0, chunk_ms = 0, opt;
    while ((opt = getopt(argc, argv, "vc:l:t:m:g:w:")) != -1) {
        switch (opt) {
        case 'v': verbose = 1; break;
        case 'c': chunk_ms = atoi(optarg); break;
        case 'l': p.min_level_db = (float)atof(optarg); break;
        case 't': p.voiced_thr = (float)atof(optarg); break;
        case 'm': p.min_voiced_ms = atoi(optarg); break;
        case 'g': p.tone_purity = (float)atof(optarg); break;
        case 'w': p.window_ms = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-v] [-c chunk_ms] [-l dB] [-t thr] [-m ms] [-g purity] [-w ms] file.wav...\n", argv[0]);
            return 2;
        }
    }
    for (int a = optind; a < argc; a++) {
        int n = 0, rate = 0;
        int16_t *pcm = wav_load(argv[a], &n, &rate);
        if (!pcm) continue;
        p.sample_rate = rate;
        vad_t v;
        vad_init(&v, &p);
        int fr = rate / 50;                       /* 20 ms */
        int per_chunk = chunk_ms > 0 ? chunk_ms / 20 : 0;
        int chunks = 0, hits = 0, in_chunk = 0;
        long lat_sum = 0;
        for (int off = 0; off + fr <= n; off += fr) {
            vad_frame_t f;
            bool sp = vad_process(&v, pcm + off, fr, &f);
            if (verbose)
                printf("%7.2f  %6.1f dB  per %.2f  purity %.2f  lag %3d  %c%c%c%c\n",
                       off / (double)rate, f.level_db, f.periodicity, f.purity,
                       f.lag, f.loud ? 'L' : '-', f.voiced ? 'V' : '-',
                       f.tone ? 'T' : '-', sp ? 'S' : '-');
            if (per_chunk && ++in_chunk == per_chunk) {
                chunks++;
                if (vad_speech(&v)) { hits++; lat_sum += vad_stats(&v)->speech_at_ms; }
                vad_reset(&v);
                in_chunk = 0;
            }
        }
        if (per_chunk) {
            printf("%-34s %5d openings of %4d ms: %5d speech (%5.1f%%)", argv[a],
                   chunks, chunk_ms, hits, chunks ? 100.0 * hits / chunks : 0.0);
            if (hits) printf("  avg detect %ld ms", lat_sum / hits);
            printf("\n");
        } else {
            const vad_stats_t *s = vad_stats(&v);
            printf("%-34s %6.2fs  %-7s", argv[a], n / (double)rate, vad_describe(s));
            if (s->speech_at_ms >= 0) printf(" @%4d ms", s->speech_at_ms);
            else                      printf("         ");
            printf("  voiced %4d ms  tone %4d ms  steady %4d ms  level avg %5.1f peak %5.1f dBFS\n",
                   s->voiced_frames * 20, s->tone_frames * 20, s->steady_frames * 20,
                   s->frames ? s->level_sum_db / s->frames : -120.0, s->level_peak_db);
        }
        free(pcm);
    }
    return 0;
}
