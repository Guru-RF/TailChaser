/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* dtmftest — run the DTMF decoder over WAV files, offline.
 *
 *   dtmftest file.wav [...]
 *
 * The WAV must be 16-bit PCM; the first channel is used. Prints, per file, the
 * digits the decoder finds and for how much of the file it says a digit is on
 * the channel (that much would be cut out of the audio by `[dtmf] muting`).
 * A recording of people talking should give no digits at all.
 */
#include "dtmf.h"
#include "wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s file.wav...\n", argv[0]);
        return 2;
    }
    for (int a = 1; a < argc; a++) {
        int n = 0, rate = 0;
        int16_t *pcm = wav_load(argv[a], &n, &rate);
        if (!pcm) continue;
        dtmf_t d;
        dtmf_init(&d, rate);
        int fr = rate / 50, on = 0, nd = 0;                   /* 20 ms */
        char digits[512] = "";
        for (int off = 0; off + fr <= n; off += fr) {
            bool tone = false;
            char c = dtmf_process(&d, pcm + off, fr, &tone);
            if (tone) on++;
            if (c && nd + 1 < (int)sizeof digits) { digits[nd++] = c; digits[nd] = '\0'; }
        }
        printf("%s\t%.2f s\t%d digit%s\t%s\t(a digit on the channel for %d ms)\n", argv[a],
               n / (double)rate, nd, nd == 1 ? "" : "s", nd ? digits : "-", on * 20);
        free(pcm);
    }
    return 0;
}
