/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* iqgen — a stand-in for rtl_sdr / hydrasdr_rx, for the tests.
 *
 *   iqgen u8|s16|f32 <rate> <offset_hz> [<tone_hz>]
 *
 * Samples on stdout, in real time, for ever: 6 s of an empty channel, 3 s of
 * a carrier <offset_hz> from the centre, FM with a 1 kHz tone (1.5 kHz
 * deviation) and, when asked, a CTCSS tone under it — and again.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double gauss(void)
{
    double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2.0 * log(u)) * cos(2.0 * M_PI * v);
}

int main(int argc, char **argv)
{
    if (argc < 4)
        return 2;
    int fmt = !strcmp(argv[1], "u8") ? 0 : !strcmp(argv[1], "s16") ? 1 : 2;
    int rate = atoi(argv[2]);
    double off = atof(argv[3]), sub = argc > 4 ? atof(argv[4]) : 0.0;
    int blk = rate / 50;
    uint8_t *out = malloc((size_t)blk * 8);
    double carrier = 0.0, t1 = 0.0, t2 = 0.0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    srand(1);
    fprintf(stderr, "iqgen: %s at %d S/s\n", argv[1], rate);
    for (long n = 0;; n++) {
        double sec = fmod(n / 50.0, 12.0);
        int on = sec >= 6.0 && sec < 9.0;
        for (int i = 0; i < blk; i++) {
            t1 += 2.0 * M_PI * 1000.0 / rate;
            t2 += 2.0 * M_PI * sub / rate;
            carrier += 2.0 * M_PI * (off + 1500.0 * sin(t1) + (sub > 0 ? 400.0 * sin(t2) : 0.0)) / rate;
            double a = on ? 0.4 : 0.0;
            double I = a * cos(carrier) + 0.02 * gauss(), Q = a * sin(carrier) + 0.02 * gauss();
            if (fmt == 0) {
                out[2 * i] = (uint8_t)lrint(127.5 + 127.0 * I);
                out[2 * i + 1] = (uint8_t)lrint(127.5 + 127.0 * Q);
            } else if (fmt == 1) {
                int16_t v[2] = { (int16_t)lrint(32000.0 * I), (int16_t)lrint(32000.0 * Q) };
                memcpy(out + 4 * i, v, 4);
            } else {
                float v[2] = { (float)I, (float)Q };
                memcpy(out + 8 * i, v, 8);
            }
        }
        size_t len = (size_t)blk * (fmt == 0 ? 2 : fmt == 1 ? 4 : 8);
        if (fwrite(out, 1, len, stdout) != len || fflush(stdout) != 0)
            return 0;
        struct timespec due = t0;
        long ms = (n + 1) * 20;
        due.tv_sec += ms / 1000;
        due.tv_nsec += (ms % 1000) * 1000000L;
        if (due.tv_nsec >= 1000000000L) { due.tv_nsec -= 1000000000L; due.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &due, NULL);
    }
}
