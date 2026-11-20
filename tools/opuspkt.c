/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* opuspkt RATE < raw-s16le > packets: 20 ms Opus packets, each after its
 * length (uint16 LE). For the tests' stand-in UberSDR. */
#include <opus/opus.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    int rate = argc > 1 ? atoi(argv[1]) : 24000, err = 0, n = rate / 50;
    OpusEncoder *e = opus_encoder_create(rate, 1, OPUS_APPLICATION_AUDIO, &err);
    if (!e) return 1;
    opus_encoder_ctl(e, OPUS_SET_BITRATE(48000));
    int16_t pcm[960];
    unsigned char pkt[1500];
    while (fread(pcm, 2, (size_t)n, stdin) == (size_t)n) {
        int len = opus_encode(e, pcm, n, pkt, sizeof pkt);
        if (len < 0) return 1;
        unsigned char h[2] = { (unsigned char)len, (unsigned char)(len >> 8) };
        fwrite(h, 1, 2, stdout);
        fwrite(pkt, 1, (size_t)len, stdout);
    }
    return 0;
}
