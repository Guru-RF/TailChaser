/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* gatetest — run the squelch gate over WAV files, offline.
 *
 *   gatetest [-o out.wav] [-m on|monitor|off] [-k lookback_ms] [-H hold_s]
 *            [-T carrier_timeout_ms] [-g gap_ms] file.wav [...]
 *
 * Every file is one squelch opening: the radio squelch opens at its first
 * sample and closes at its last, with gap_ms (default 15000) of closed squelch
 * between files. Prints what the talkgroup would be sent, with timestamps, and
 * with -o writes the audio the talkgroup would receive so you can listen to
 * what the gate does to the start of an over.
 */
#include "gate.h"
#include "wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int      g_ms;                 /* test clock */
static int16_t *g_out;
static int      g_out_n, g_out_cap;

static void on_squelch(void *ctx, bool open)
{
    (void)ctx;
    printf("  %8.2fs  talkgroup: over %s\n", g_ms / 1000.0, open ? "BEGINS" : "ENDS");
}

static void on_audio(void *ctx, const int16_t *pcm, int n)
{
    (void)ctx;
    if (g_out_n + n > g_out_cap) {
        g_out_cap = (g_out_n + n) * 2;
        g_out = realloc(g_out, sizeof(int16_t) * (size_t)g_out_cap);
        if (!g_out) exit(1);
    }
    memcpy(g_out + g_out_n, pcm, sizeof(int16_t) * (size_t)n);
    g_out_n += n;
}

static void on_dtmf(void *ctx, char digit, int duration_ms)
{
    (void)ctx;
    printf("  %8.2fs  node: DTMF %c (%d ms)\n", g_ms / 1000.0, digit, duration_ms);
}

static void on_report(void *ctx, const gate_report_t *r)
{
    (void)ctx;
    printf("  %8.2fs  opening %.2fs -> %s", g_ms / 1000.0, r->duration_ms / 1000.0,
           gate_pass_name(r->pass));
    if (r->pass == GATE_PASS_UNGATED && r->analysed)        /* monitor: what "on" would have done */
        printf(" (on: %s%s)", gate_pass_name(r->would), r->too_late ? ", too late" : "");
    if (r->analysed)
        printf("  [looks like %s, voiced %d ms, level avg %.0f peak %.0f dBFS]",
               r->looks_like, r->voiced_ms, r->level_avg_db, r->level_peak_db);
    if (r->speech_at_ms >= 0)
        printf("  speech after %d ms, stream delay %d ms", r->speech_at_ms, r->delay_ms);
    printf("\n");
    /* kept off the talkgroup, or would be: what the detector made of each 20 ms (legend: gate.h) */
    if (r->analysed && r->would == GATE_PASS_NONE && r->trace[0])
        printf("             heard: %s\n", r->trace);
}

int main(int argc, char **argv)
{
    gate_params_t p;
    gate_default_params(&p);
    const char *outpath = NULL;
    int gap_ms = 15000, opt;
    while ((opt = getopt(argc, argv, "o:m:k:H:T:g:t:")) != -1) {
        switch (opt) {
        case 'o': outpath = optarg; break;
        case 'm': p.mode = !strcmp(optarg, "off") ? GATE_MODE_OFF
                         : !strcmp(optarg, "monitor") ? GATE_MODE_MONITOR : GATE_MODE_ON; break;
        case 'k': p.lookback_ms = atoi(optarg); break;
        case 'H': p.hold_s = atoi(optarg); break;
        case 'T': p.carrier_timeout_ms = atoi(optarg); break;
        case 'g': gap_ms = atoi(optarg); break;
        case 't': p.tail_trim_ms = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-o out.wav] [-m on|monitor|off] [-k lookback_ms] "
                            "[-H hold_s] [-T timeout_ms] [-t tail_trim_ms] [-g gap_ms] file.wav...\n",
                    argv[0]);
            return 2;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "usage: %s [-o out.wav] [-m on|monitor|off] [-k lookback_ms] "
                        "[-H hold_s] [-T timeout_ms] [-t tail_trim_ms] [-g gap_ms] file.wav...\n",
                argv[0]);
        return 2;
    }
    gate_sink_t sink = { NULL, on_squelch, on_audio, on_dtmf, on_report };
    static gate_t g;
    int rate = 0;
    static const int16_t silence[GATE_MAX_FRAME];
    for (int a = optind; a < argc; a++) {
        int n = 0, r = 0;
        int16_t *pcm = wav_load(argv[a], &n, &r);
        if (!pcm) continue;
        if (!rate) {
            rate = r;
            p.sample_rate = rate;
            gate_init(&g, &p, &sink);
        } else if (r != rate) {
            fprintf(stderr, "%s: sample rate differs, skipped\n", argv[a]);
            free(pcm);
            continue;
        }
        int fr = rate / 50;
        printf("%s (%.2fs)\n  %8.2fs  radio squelch opens\n", argv[a], n / (double)rate, g_ms / 1000.0);
        for (int off = 0; off + fr <= n; off += fr, g_ms += 20)
            gate_process(&g, pcm + off, fr, true);
        printf("  %8.2fs  radio squelch closes\n", g_ms / 1000.0);
        for (int i = 0; i < gap_ms / 20; i++, g_ms += 20)
            gate_process(&g, silence, fr, false);
        free(pcm);
    }
    if (outpath && rate) {
        wav_save(outpath, g_out ? g_out : silence, g_out_n, rate);
        printf("wrote %s (%.2fs of audio passed to the talkgroup)\n", outpath, g_out_n / (double)rate);
    }
    return 0;
}
