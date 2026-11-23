/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* tagtest — the callsign tag by itself (docs/TAG.md).
 *
 *   tagtest signs CALL local|network            the 164 symbol signs
 *   tagtest wav   CALL local|network SECONDS OUT.wav [LEVEL_DB]
 *                                               the tag alone, 16 kHz mono */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/tag.h"

static void put32(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc((v >> 24) & 255, f); }
static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "signs")) {
        int8_t s[TAG_SYMBOLS];
        tag_signs(argv[2], !strcmp(argv[3], "local"), s);
        for (int i = 0; i < TAG_SYMBOLS; i++)
            putchar(s[i] > 0 ? '+' : '-');
        putchar('\n');
        return 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "wav")) {
        double level = argc > 6 ? atof(argv[6]) : -26.0;
        long n = (long)(atof(argv[4]) * TAG_RATE);
        tag_t *t = tag_new(32767.0f * (float)pow(10.0, level / 20.0));
        FILE *f = fopen(argv[5], "wb");
        if (!t || !f || n <= 0) {
            fprintf(stderr, "tagtest: cannot write %s\n", argv[5]);
            return 1;
        }
        tag_set(t, argv[2], !strcmp(argv[3], "local"));
        fwrite("RIFF", 1, 4, f); put32(f, (unsigned)(36 + n * 2)); fwrite("WAVEfmt ", 1, 8, f);
        put32(f, 16); put16(f, 1); put16(f, 1); put32(f, TAG_RATE); put32(f, TAG_RATE * 2); put16(f, 2); put16(f, 16);
        fwrite("data", 1, 4, f); put32(f, (unsigned)(n * 2));
        int16_t buf[320];
        for (long done = 0; done < n; done += 320) {
            int k = n - done < 320 ? (int)(n - done) : 320;
            memset(buf, 0, sizeof buf);
            tag_mix(t, buf, k, true);
            fwrite(buf, 2, (size_t)k, f);
        }
        fclose(f);
        tag_free(t);
        return 0;
    }
    fprintf(stderr, "usage: tagtest signs CALL local|network\n       tagtest wav CALL local|network SECONDS OUT.wav [LEVEL_DB]\n");
    return 2;
}
