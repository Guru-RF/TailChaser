/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int16_t *wav_load(const char *path, int *n_out, int *rate_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    unsigned char h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: not a WAV file\n", path);
        fclose(f);
        return NULL;
    }
    int channels = 1, rate = 16000, bits = 16;
    int16_t *pcm = NULL;
    int n = 0;
    unsigned char ch[8];
    while (fread(ch, 1, 8, f) == 8) {
        unsigned sz = ch[4] | ch[5] << 8 | ch[6] << 16 | (unsigned)ch[7] << 24;
        if (!memcmp(ch, "fmt ", 4)) {
            unsigned char fm[16];
            if (sz < 16 || fread(fm, 1, 16, f) != 16) break;
            channels = fm[2] | fm[3] << 8;
            rate = fm[4] | fm[5] << 8 | fm[6] << 16 | fm[7] << 24;
            bits = fm[14] | fm[15] << 8;
            fseek(f, (long)(sz - 16 + (sz & 1)), SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) {
            if (bits != 16 || channels < 1) {
                fprintf(stderr, "%s: need 16-bit PCM\n", path);
                break;
            }
            long start = ftell(f);
            fseek(f, 0, SEEK_END);
            long avail = ftell(f) - start;
            fseek(f, start, SEEK_SET);
            if (sz > (unsigned long)avail) sz = (unsigned)avail;   /* streamed WAV */
            int frames = (int)(sz / (2u * (unsigned)channels));
            int16_t *raw = malloc(sizeof(int16_t) * (size_t)frames * (size_t)channels);
            pcm = malloc(sizeof(int16_t) * (size_t)(frames > 0 ? frames : 1));
            if (!raw || !pcm) { free(raw); free(pcm); pcm = NULL; break; }
            size_t got = fread(raw, 2u * (unsigned)channels, (size_t)frames, f);
            for (size_t i = 0; i < got; i++) pcm[i] = raw[i * (size_t)channels];
            free(raw);
            n = (int)got;
            break;
        } else {
            fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
        }
    }
    fclose(f);
    *n_out = n;
    *rate_out = rate;
    return pcm;
}

int wav_save(const char *path, const int16_t *pcm, int n, int rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    unsigned data = (unsigned)n * 2u, riff = 36u + data, r = (unsigned)rate, br = r * 2u;
    unsigned char h[44] = {
        'R','I','F','F', (unsigned char)riff, (unsigned char)(riff >> 8),
        (unsigned char)(riff >> 16), (unsigned char)(riff >> 24),
        'W','A','V','E','f','m','t',' ', 16,0,0,0, 1,0, 1,0,
        (unsigned char)r, (unsigned char)(r >> 8), (unsigned char)(r >> 16), (unsigned char)(r >> 24),
        (unsigned char)br, (unsigned char)(br >> 8), (unsigned char)(br >> 16), (unsigned char)(br >> 24),
        2,0, 16,0, 'd','a','t','a',
        (unsigned char)data, (unsigned char)(data >> 8),
        (unsigned char)(data >> 16), (unsigned char)(data >> 24),
    };
    int ok = fwrite(h, 1, 44, f) == 44 && fwrite(pcm, 2, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok ? 0 : -1;
}
