/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* agctest — run the receive normaliser over WAV files, offline.
 *
 *   agctest [-t target_db] [-g max_gain_db] [-o out.wav] file.wav [...]
 *
 * Each file is one over. Prints the level that went in and the level that
 * came out (RMS of the loud frames, dBFS), the gain that was applied on
 * average, and the peak — so that "does every station come out equally loud,
 * and does nothing clip" can be read off a recording of the real receiver.
 * With -o the result of the last file is written as a WAV.
 */
#include "audioproc.h"
#include "wav.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* RMS of the frames within 18 dB of the loudest one: the speech, not the pauses. */
static double speech_db(const int16_t *pcm, int n, int fr)
{
    double top = 0.0;
    for (int off = 0; off + fr <= n; off += fr) {
        double s = 0.0;
        for (int i = 0; i < fr; i++) s += (double)pcm[off + i] * pcm[off + i];
        if (s > top) top = s;
    }
    if (top <= 0.0)
        return -120.0;
    double sum = 0.0;
    long cnt = 0;
    for (int off = 0; off + fr <= n; off += fr) {
        double s = 0.0;
        for (int i = 0; i < fr; i++) s += (double)pcm[off + i] * pcm[off + i];
        if (s >= top / 63.0) { sum += s; cnt += fr; }
    }
    return 10.0 * log10(sum / (double)cnt / (32768.0 * 32768.0));
}

int main(int argc, char **argv)
{
    agc_params_t p;
    agc_default_params(&p);
    const char *out = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "t:g:o:")) != -1) {
        switch (opt) {
        case 't': p.target_db = (float)atof(optarg); break;
        case 'g': p.max_gain_db = (float)atof(optarg); break;
        case 'o': out = optarg; break;
        default:
            fprintf(stderr, "usage: %s [-t target_db] [-g max_gain_db] [-o out.wav] file.wav...\n", argv[0]);
            return 2;
        }
    }
    agc_t a;
    agc_init(&a, &p);       /* one normaliser for all files: overs follow each other */
    for (int f = optind; f < argc; f++) {
        int n = 0, rate = 0;
        int16_t *pcm = wav_load(argv[f], &n, &rate);
        if (!pcm) continue;
        int fr = rate / 50;
        double in_db = speech_db(pcm, n, fr);
        agc_begin_over(&a);
        for (int off = 0; off + fr <= n; off += fr)
            agc_process(&a, pcm + off, fr);
        n -= n % fr;
        double out_db = speech_db(pcm, n, fr);
        /* the level once the gain has settled: what the listener gets after the first second */
        int skip = n > 2 * rate ? rate : 0;
        double late_db = speech_db(pcm + skip, n - skip, fr);
        int peak = 0;
        for (int i = 0; i < n; i++) {
            int v = abs(pcm[i]);
            if (v > peak) peak = v;
        }
        float si = -120.0f, sg = 0.0f;
        agc_over_stats(&a, &si, &sg);
        printf("%-34s in %6.1f  out %6.1f  settled %6.1f dBFS  gain %+5.1f dB  peak %5d\n",
               argv[f], in_db, out_db, late_db, (double)sg, peak);
        if (out && f == argc - 1)
            wav_save(out, pcm, n, rate);
        free(pcm);
    }
    return 0;
}
