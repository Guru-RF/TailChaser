/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "gate.h"

#include <string.h>

void gate_default_params(gate_params_t *p)
{
    memset(p, 0, sizeof *p);
    p->mode               = GATE_MODE_ON;
    p->sample_rate        = 16000;
    p->frame_ms           = 20;
    p->lookback_ms        = 500;
    p->hold_s             = 10;
    p->min_open_ms        = 100;
    p->voice_deadline_ms  = 3000;
    /* The end of an over that was delayed by the look-back is played out, not
     * blanked: nobody on a talkgroup hears their own voice come back, and the
     * last word belongs to the over. */
    p->tail_silence       = false;
    p->carrier_timeout_ms = 0;
    p->bridge_ms          = 300;
    p->tail_trim_ms       = 0;
    p->dtmf_enabled       = true;
    /* DTMF is a command for this node, not something the talkgroup wants to
     * hear: a transmission with nothing but tones on it stays local. */
    p->dtmf_opens         = false;
    p->dtmf_muting        = true;
    vad_default_params(&p->vad);
}

static int ms_to_frames(const gate_t *g, int ms)
{
    int fm = g->p.frame_ms > 0 ? g->p.frame_ms : 20;
    return (ms + fm - 1) / fm;
}

void gate_init(gate_t *g, const gate_params_t *p, const gate_sink_t *sink)
{
    memset(g, 0, sizeof *g);
    g->p = *p;
    if (g->p.frame_ms <= 0) g->p.frame_ms = 20;
    g->p.vad.sample_rate = g->p.sample_rate;
    /* the ring must hold the look-back plus the tail trim */
    int max_ms = (GATE_MAX_RING - 2) * g->p.frame_ms;
    if (g->p.tail_trim_ms < 0) g->p.tail_trim_ms = 0;
    if (g->p.tail_trim_ms > max_ms / 2) g->p.tail_trim_ms = max_ms / 2;
    if (g->p.lookback_ms < 0) g->p.lookback_ms = 0;
    if (g->p.lookback_ms + g->p.tail_trim_ms > max_ms)
        g->p.lookback_ms = max_ms - g->p.tail_trim_ms;
    if (sink) g->sink = *sink;
    vad_init(&g->vad, &g->p.vad);
    dtmf_init(&g->dtmf, g->p.sample_rate);
}

gate_state_t gate_state(const gate_t *g) { return g->state; }
bool gate_is_open(const gate_t *g) { return g->state == GATE_OPEN || g->state == GATE_DRAIN; }
int  gate_hold_left_ms(const gate_t *g) { return g->hold_left * g->p.frame_ms; }
const vad_frame_t *gate_last_frame(const gate_t *g) { return &g->last; }

const char *gate_state_name(gate_state_t s)
{
    switch (s) {
    case GATE_IDLE:    return "idle";
    case GATE_PENDING: return "listening";
    case GATE_BRIDGE:  return "listening";
    case GATE_OPEN:    return "open";
    case GATE_DRAIN:   return "open";
    }
    return "?";
}

const char *gate_pass_name(gate_pass_t p)
{
    switch (p) {
    case GATE_PASS_NONE:    return "blocked";
    case GATE_PASS_SPEECH:  return "speech";
    case GATE_PASS_DTMF:    return "dtmf";
    case GATE_PASS_HOLD:    return "in-conversation";
    case GATE_PASS_TIMEOUT: return "carrier-timeout";
    case GATE_PASS_UNGATED: return "ungated";
    }
    return "?";
}

const char *gate_mode_name(gate_mode_t m)
{
    return m == GATE_MODE_ON ? "on" : m == GATE_MODE_MONITOR ? "monitor" : "off";
}

/* ---- frame ring ---- */

static void ring_push(gate_t *g, const int16_t *pcm, int n)
{
    if (g->len == GATE_MAX_RING) {              /* full: drop the oldest */
        g->head = (g->head + 1) % GATE_MAX_RING;
        g->len--;
    }
    int idx = (g->head + g->len) % GATE_MAX_RING;
    memcpy(g->ring[idx], pcm, sizeof(int16_t) * (size_t)n);
    g->ring_n[idx] = (int16_t)n;
    g->len++;
}

static void ring_emit_oldest(gate_t *g)
{
    if (g->len <= 0) return;
    if (g->sink.audio)
        g->sink.audio(g->sink.ctx, g->ring[g->head], g->ring_n[g->head]);
    g->head = (g->head + 1) % GATE_MAX_RING;
    g->len--;
}

static void ring_drop_oldest(gate_t *g, int keep)
{
    while (g->len > keep) {
        g->head = (g->head + 1) % GATE_MAX_RING;
        g->len--;
    }
}

static void ring_drop_newest(gate_t *g, int count)
{
    if (count > g->len) count = g->len;
    g->len -= count;
}

/* Silence the newest `count` frames still waiting in the ring (DTMF muting:
 * the decoder confirms a digit two frames after the tone began, and says
 * nothing before that). */
static void ring_mute_newest(gate_t *g, int count)
{
    for (int i = 1; i <= count && i <= g->len; i++) {
        int idx = (g->head + g->len - i) % GATE_MAX_RING;
        memset(g->ring[idx], 0, sizeof(int16_t) * (size_t)g->ring_n[idx]);
    }
}

/* ---- transitions ---- */

/* closed_frames: for how long the squelch had been closed. */
static void begin_opening(gate_t *g, int closed_frames)
{
    g->open_frames = 0;
    g->gave_up = false;
    g->in_hold = g->hold_left > 0;
    g->pass = GATE_PASS_NONE;
    g->would = GATE_PASS_NONE;
    g->delay_at_open = 0;
    g->dtmf_digits = 0;
    g->head = g->len = 0;
    g->trace_n = 0;
    g->trace[0] = '\0';
    g->analysing = (g->p.mode != GATE_MODE_OFF);
    if (g->analysing)
        vad_reset(&g->vad);
    /* A new transmission, a new DTMF decoder: the key that was held when the
     * last one ended counts again if it is the first thing in this one. After
     * a squelch drop-out it does not — a key held through one is one press,
     * also where the drop-out has ended the opening (mode off and monitor). */
    if (closed_frames > ms_to_frames(g, g->p.bridge_ms))
        dtmf_reset(&g->dtmf);
}

/* Report squelch-open to the repeater and start streaming. The ring already
 * holds the look-back audio; from now on the stream runs that far behind. */
static void open_gate(gate_t *g, gate_pass_t why)
{
    int trim = ms_to_frames(g, g->p.tail_trim_ms);
    ring_drop_oldest(g, ms_to_frames(g, g->p.lookback_ms) + trim);
    g->delay_frames = g->len > trim ? g->len : trim;
    /* A DTMF digit is silenced backwards from the frame that confirms it, so
     * the frames that carried its beginning must not have left yet. */
    if (g->p.dtmf_enabled && g->p.dtmf_muting && g->delay_frames < GATE_DTMF_LOOKBACK)
        g->delay_frames = GATE_DTMF_LOOKBACK;
    g->delay_at_open = g->delay_frames * g->p.frame_ms;
    g->pass = why;
    g->state = GATE_OPEN;
    if (g->sink.squelch)
        g->sink.squelch(g->sink.ctx, true);
}

static void finish_opening(gate_t *g)
{
    gate_report_t r;
    memset(&r, 0, sizeof r);
    r.duration_ms  = g->open_frames * g->p.frame_ms;
    r.pass         = g->pass;
    r.would        = g->p.mode == GATE_MODE_MONITOR ? g->would : g->pass;
    r.analysed     = g->analysing;
    r.speech_at_ms = -1;
    r.looks_like   = "";
    r.delay_ms     = g->delay_at_open;
    r.dtmf_digits  = g->dtmf_digits;
    r.too_late     = g->gave_up && r.would == GATE_PASS_NONE;
    r.trace        = "";
    if (g->analysing) {
        const vad_stats_t *s = vad_stats(&g->vad);
        r.looks_like    = vad_describe(s);
        r.speech_at_ms  = s->speech_at_ms;
        r.voiced_ms     = s->voiced_frames * g->p.frame_ms;
        r.loud_ms       = s->loud_frames * g->p.frame_ms;
        r.level_avg_db  = s->frames ? (float)(s->level_sum_db / s->frames) : -120.0f;
        r.level_peak_db = s->level_peak_db;
        r.trace         = g->trace;
        /* Digits and no voice: that is what it was, however much silence
         * there was between the digits. */
        if (s->speech_at_ms < 0 && g->dtmf_digits > 0)
            r.looks_like = "tone";
    }
    if (g->sink.report)
        g->sink.report(g->sink.ctx, &r);
    g->state = GATE_IDLE;
    g->head = g->len = 0;
    g->delay_frames = 0;
}

static void close_gate(gate_t *g)
{
    if (g->sink.squelch)
        g->sink.squelch(g->sink.ctx, false);
    /* Only a real over keeps the conversation going. An opening that slipped
     * through on the hold time but had no voice on it (a noise burst between
     * two overs) must not extend the hold, or noise could keep itself alive. */
    /* Monitor mode keeps the clock of mode ON: an opening that ON would have
     * kept off the talkgroup (digits only, a voice after the deadline) does
     * not start a conversation there either. */
    gate_pass_t as = g->p.mode == GATE_MODE_MONITOR ? g->would : g->pass;
    bool real = !g->analysing ||
                (as != GATE_PASS_NONE &&
                 (vad_speech(&g->vad) || g->dtmf_digits > 0 || as == GATE_PASS_TIMEOUT));
    if (real)
        g->hold_left = ms_to_frames(g, g->p.hold_s * 1000);
    finish_opening(g);
}

/* One character per analysed frame: what the detector made of it (gate.h). */
static void trace_frame(gate_t *g, const vad_frame_t *f, bool dtmf_tone, bool dtmf_digit)
{
    if (dtmf_digit)                 /* ...and so were the two frames before it */
        for (int i = 1; i <= GATE_DTMF_LOOKBACK && i <= g->trace_n; i++)
            g->trace[g->trace_n - i] = 'D';
    if (g->trace_n >= GATE_TRACE_MAX) {
        g->trace[GATE_TRACE_MAX] = '+';
        g->trace[GATE_TRACE_MAX + 1] = '\0';
        return;
    }
    char c;
    if (dtmf_tone)       c = 'D';
    else if (!f->loud)   c = '.';
    else if (f->voiced)  c = 'V';
    else if (f->tone)    c = 'T';
    else {
        int tenths = (int)(f->periodicity * 10.0f);
        c = (char)('0' + (tenths < 0 ? 0 : tenths > 9 ? 9 : tenths));
    }
    g->trace[g->trace_n++] = c;
    g->trace[g->trace_n] = '\0';
}

/* DTMF decode for one squelch-open frame. Returns true if a digit was newly
 * confirmed; *tone tells whether a DTMF digit is on the channel right now. */
static bool run_dtmf(gate_t *g, const int16_t *pcm, int n, bool *tone)
{
    *tone = false;
    if (!g->p.dtmf_enabled)
        return false;
    char d = dtmf_process(&g->dtmf, pcm, n, tone);
    bool fresh = false;
    if (d) {
        g->dtmf_digit = d;
        g->dtmf_frames = 2;         /* confirmed after two blocks of tone */
        g->dtmf_digits++;
        fresh = true;
    } else if (g->dtmf_digit) {
        if (*tone) {
            g->dtmf_frames++;
        } else {
            if (g->sink.dtmf)
                g->sink.dtmf(g->sink.ctx, g->dtmf_digit, g->dtmf_frames * g->p.frame_ms);
            g->dtmf_digit = 0;
        }
    }
    return fresh;
}

/* A digit still held when the squelch drops is reported as it stands. */
static void flush_dtmf(gate_t *g)
{
    if (g->dtmf_digit && g->sink.dtmf)
        g->sink.dtmf(g->sink.ctx, g->dtmf_digit, g->dtmf_frames * g->p.frame_ms);
    g->dtmf_digit = 0;
}

/* What mode ON makes of the opening as it stands: why it passes, or
 * GATE_PASS_NONE — not yet, or never. Monitor mode asks the same question and
 * only notes the answer. */
static gate_pass_t verdict(gate_t *g, bool speech, bool digit)
{
    int open_ms = g->open_frames * g->p.frame_ms;
    /* In conversation the over passes without proof of a voice — but only
     * once the squelch has stayed open for min_open_ms, so a squelch blip
     * after an over does not key the repeater again. */
    if (g->in_hold && open_ms >= g->p.min_open_ms)
        return GATE_PASS_HOLD;
    if (speech && !g->gave_up)
        return GATE_PASS_SPEECH;
    if (digit && g->p.dtmf_opens)
        return GATE_PASS_DTMF;
    if (g->p.carrier_timeout_ms > 0 && open_ms >= g->p.carrier_timeout_ms)
        return GATE_PASS_TIMEOUT;
    /* No voice within the deadline: this carrier stays closed until it drops,
     * whatever comes later. (DTMF still opens.) */
    if (g->p.voice_deadline_ms > 0 && !g->in_hold && open_ms >= g->p.voice_deadline_ms)
        g->gave_up = true;
    return GATE_PASS_NONE;
}

void gate_process(gate_t *g, const int16_t *pcm, int n, bool cos)
{
    if (n <= 0 || n > GATE_MAX_FRAME)
        return;
    static const int16_t zeros[GATE_MAX_FRAME];

    /* the "in conversation" clock runs on from the end of the last real over */
    if (g->hold_left > 0 && g->state != GATE_OPEN && g->state != GATE_DRAIN)
        g->hold_left--;

    int closed = g->closed_frames;
    g->closed_frames = cos ? 0 : closed < 1000000 ? closed + 1 : closed;

    if (g->state == GATE_IDLE) {
        if (!cos)
            return;
        begin_opening(g, closed);
        if (g->p.mode != GATE_MODE_ON)
            open_gate(g, GATE_PASS_UNGATED);
        else
            g->state = GATE_PENDING;
    }

    /* ---- squelch closed while something is in progress ---- */
    if (!cos) {
        switch (g->state) {
        case GATE_PENDING:
            flush_dtmf(g);
            g->bridge_left = ms_to_frames(g, g->p.bridge_ms);
            g->state = GATE_BRIDGE;
            /* fall through */
        case GATE_BRIDGE:
            if (g->bridge_left-- <= 0)
                finish_opening(g);          /* never proved a voice: dropped */
            return;
        case GATE_OPEN:
            flush_dtmf(g);
            ring_drop_newest(g, ms_to_frames(g, g->p.tail_trim_ms));
            g->state = GATE_DRAIN;
            /* fall through */
        case GATE_DRAIN:
            if (g->len > 0) {
                /* What is still delayed is played out — as silence by default:
                 * the talker has unkeyed and is listening, and would hear the
                 * end of their own over come back from the repeater. */
                if (g->p.tail_silence)
                    memset(g->ring[g->head], 0, sizeof(int16_t) * (size_t)g->ring_n[g->head]);
                ring_emit_oldest(g);
            }
            if (g->len == 0)
                close_gate(g);
            return;
        default:
            return;
        }
    }

    /* ---- squelch open ---- */
    if (g->state == GATE_BRIDGE)
        g->state = GATE_PENDING;            /* it came back: same opening */
    if (g->state == GATE_DRAIN)
        g->state = GATE_OPEN;               /* same over continues        */

    g->open_frames++;
    bool tone = false;
    bool digit = run_dtmf(g, pcm, n, &tone);
    bool speech = false;
    if (g->analysing) {     /* a DTMF tone is not a voice, however much its "pitch" wanders */
        speech = vad_process_ex(&g->vad, pcm, n, &g->last, tone);
        trace_frame(g, &g->last, tone, digit);
    }

    bool mute = g->p.dtmf_muting && tone;
    if (g->p.dtmf_muting && digit)
        ring_mute_newest(g, GATE_DTMF_LOOKBACK);
    ring_push(g, mute ? zeros : pcm, n);

    if (g->state == GATE_PENDING) {
        gate_pass_t why = verdict(g, speech, digit);
        if (why != GATE_PASS_NONE)
            open_gate(g, why);
        else
            ring_drop_oldest(g, GATE_MAX_RING - 1);
    } else if (g->p.mode == GATE_MODE_MONITOR && g->would == GATE_PASS_NONE) {
        g->would = verdict(g, speech, digit);
    }
    if (g->state == GATE_OPEN) {
        while (g->len > g->delay_frames)
            ring_emit_oldest(g);
    }
}
