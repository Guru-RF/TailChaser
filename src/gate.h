/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — the squelch gate.
 *
 * Sits between the radio (chip squelch + captured audio) and the uplink to the
 * repeater. The chip squelch only says "there is a carrier" — and at squelch
 * level 1 it says so for every noise burst too. The gate decides what the
 * REPEATER gets to see:
 *
 *   radio squelch opens ──► PENDING: audio is analysed (vad.c) and kept in a
 *                           look-back buffer; the repeater sees nothing.
 *        speech / DTMF  ──► OPEN: squelch-open is reported and the audio is
 *                           streamed, starting `lookback_ms` in the past so the
 *                           first syllable is not lost. The stream therefore
 *                           runs that much behind the air for this one over.
 *        squelch closes ──► without speech: the opening is dropped. The
 *                           repeater never knew. That is a kerchunk, a dead
 *                           carrier or a noise burst.
 *
 * After an over that did pass, the channel is "in conversation": for `hold_s`
 * seconds the next opening passes at once, with no analysis delay, so a QSO is
 * never slowed down or clipped — only the first transmission on an idle
 * repeater has to prove there is a voice.
 *
 * Pure logic: no I/O, no clock. gate_process() is called once per captured
 * frame (20 ms), whether or not the squelch is open, and that is its clock.
 * Everything it wants to tell the world goes through the sink callbacks, which
 * makes it testable against WAV files (tools/gatetest).
 */
#ifndef SVXRX_GATE_H
#define SVXRX_GATE_H

#include <stdbool.h>
#include <stdint.h>

#include "dtmf.h"
#include "vad.h"

#define GATE_MAX_FRAME      VAD_MAX_FRAME
#define GATE_MAX_RING       160     /* frames of look-back (3.2 s @ 20 ms) */
#define GATE_DTMF_LOOKBACK  2       /* frames kept back so that a DTMF digit can be
                                     * silenced from its first millisecond: it is
                                     * known for a digit two frames after it began */
#define GATE_TRACE_MAX      250     /* frames of an opening kept as a trace (5 s) */

typedef enum {
    GATE_MODE_OFF = 0,      /* plain remote RX: every squelch opening is passed       */
    GATE_MODE_MONITOR,      /* pass everything, and work out what ON would have done
                             * with each opening: the same decision, only noted.
                             * One difference: a squelch drop-out ends the opening
                             * here, where ON listens on for bridge_ms — the pieces
                             * of a fluttering signal are judged one by one.          */
    GATE_MODE_ON,           /* hold the repeater closed until speech (or DTMF)        */
} gate_mode_t;

typedef struct {
    gate_mode_t mode;
    int  sample_rate;           /* Hz                                               */
    int  frame_ms;              /* length of the frames fed to gate_process()        */
    int  lookback_ms;           /* audio kept from before the decision               */
    int  hold_s;                /* "in conversation" time after a passed over        */
    int  min_open_ms;           /* in conversation: the squelch must stay open this
                                 * long before the opening is passed (blips are not) */
    int  voice_deadline_ms;     /* a voice must be found within this much carrier,
                                 * or the opening stays closed (0 = no limit)        */
    int  carrier_timeout_ms;    /* pass anyway after this much carrier (0 = never)   */
    int  bridge_ms;             /* squelch drop-outs shorter than this do not restart */
                                /*   the analysis (mobile flutter), and a DTMF key   */
                                /*   held through one is still one press             */
    int  tail_trim_ms;          /* cut this much off the end of every over           */
    bool tail_silence;          /* after the squelch closes, send silence instead of
                                 * the audio still delayed (the talker would hear the
                                 * end of their own over come back)                  */
                                /*   (squelch crash); costs the same in delay        */
    bool dtmf_enabled;          /* decode DTMF and report digits                     */
    bool dtmf_opens;            /* a DTMF digit opens the gate like speech does      */
    bool dtmf_muting;           /* replace DTMF tones in the audio with silence; the
                                 * stream then runs at least GATE_DTMF_LOOKBACK
                                 * frames (40 ms) behind                              */
    vad_params_t vad;
} gate_params_t;

/* Why an opening was passed on to the repeater. */
typedef enum {
    GATE_PASS_NONE = 0,     /* it was not: blocked                    */
    GATE_PASS_SPEECH,       /* a voice was detected                   */
    GATE_PASS_DTMF,         /* a DTMF digit was detected              */
    GATE_PASS_HOLD,         /* in conversation (hold time)            */
    GATE_PASS_TIMEOUT,      /* carrier_timeout_ms elapsed             */
    GATE_PASS_UNGATED,      /* mode off / monitor                     */
} gate_pass_t;

/* Summary of one squelch opening, delivered when it is over. */
typedef struct {
    int         duration_ms;    /* how long the radio squelch was open        */
    gate_pass_t pass;           /* why it was passed, or GATE_PASS_NONE       */
    gate_pass_t would;          /* what mode ON makes of it: the same as `pass`
                                 * in mode ON; in monitor mode why ON would
                                 * have passed it, or GATE_PASS_NONE          */
    bool        analysed;       /* the fields below are valid                 */
    const char *looks_like;     /* "speech" | "silence" | "noise" | "tone"    */
    int         speech_at_ms;   /* when speech was detected, -1 = never       */
    int         voiced_ms;
    int         loud_ms;        /* frames above the level floor               */
    int         delay_ms;       /* stream delay used for this over            */
    float       level_avg_db;
    float       level_peak_db;
    int         dtmf_digits;
    bool        too_late;       /* blocked (or would be): no voice within
                                 * voice_deadline_ms                          */
    const char *trace;          /* what the detector made of each frame, one
                                 * character per frame (valid in the callback):
                                 *   .    below the level floor
                                 *   0-9  loud; how periodic, in tenths — not a
                                 *        voice at that (5 and up: see T)
                                 *   V    voiced
                                 *   T    a single tone
                                 *   D    a DTMF digit
                                 * Ends in "+" when the opening went on longer. */
} gate_report_t;

typedef struct {
    void *ctx;
    void (*squelch)(void *ctx, bool open);
    void (*audio)(void *ctx, const int16_t *pcm, int n);
    void (*dtmf)(void *ctx, char digit, int duration_ms);
    void (*report)(void *ctx, const gate_report_t *r);
} gate_sink_t;

typedef enum { GATE_IDLE = 0, GATE_PENDING, GATE_BRIDGE, GATE_OPEN, GATE_DRAIN } gate_state_t;

typedef struct {
    gate_params_t p;
    gate_sink_t   sink;
    gate_state_t  state;
    vad_t         vad;
    dtmf_t        dtmf;

    /* look-back / delay ring of whole frames */
    int16_t ring[GATE_MAX_RING][GATE_MAX_FRAME];
    int16_t ring_n[GATE_MAX_RING];
    int     head, len;
    int     delay_frames;       /* frames the stream runs behind while OPEN */

    int     hold_left;          /* frames of "in conversation" remaining */
    bool    gave_up;            /* voice deadline passed for this opening */
    bool    in_hold;            /* this opening began in conversation     */
    int     bridge_left;
    int     closed_frames;      /* frames since the squelch was last open (saturates) */
    int     open_frames;        /* radio-squelch-open frames in this opening */
    gate_pass_t pass;
    gate_pass_t would;          /* monitor mode: what ON would have done */
    bool    analysing;
    int     delay_at_open;
    int     dtmf_digits;

    char    dtmf_digit;         /* digit currently held (0 = none) */
    int     dtmf_frames;
    char    trace[GATE_TRACE_MAX + 2];
    int     trace_n;
    vad_frame_t last;           /* most recent frame analysis (status page) */
} gate_t;

void gate_default_params(gate_params_t *p);
void gate_init(gate_t *g, const gate_params_t *p, const gate_sink_t *sink);

/* One captured frame of n samples; cos = the radio squelch is open. */
void gate_process(gate_t *g, const int16_t *pcm, int n, bool cos);

gate_state_t gate_state(const gate_t *g);
bool         gate_is_open(const gate_t *g);      /* squelch-open reported to the repeater */
int          gate_hold_left_ms(const gate_t *g);
const vad_frame_t *gate_last_frame(const gate_t *g);
const char  *gate_state_name(gate_state_t s);
const char  *gate_pass_name(gate_pass_t p);
const char  *gate_mode_name(gate_mode_t m);

#endif /* SVXRX_GATE_H */
