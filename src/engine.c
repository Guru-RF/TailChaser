/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "engine.h"
#include "tag.h"
#include "monitor.h"
#include "wav.h"
#include <sys/stat.h>
#include "ctcss.h"
#include "audioproc.h"
#include "cw.h"
#include "gate.h"
#include "log.h"
#include "opus_codec.h"
#include "util.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define NETQ_FRAMES   512       /* 10 s of decoded talkgroup audio                    */
#define FIFO_TARGET   3         /* frames kept ahead in the sound card queue          */
#define JITTER_FRAMES 3         /* gathered before audio starts on a keyed transmitter */
#define NET_GAP_MS    500       /* no audio for this long: the over has ended         */
#define DTMF_IDLE_MS  10000     /* a half-entered command is forgotten after this     */
#define OPUS_BITRATE  20000
#define NET_LOOK      5         /* frames the talkgroup normaliser looks ahead: 100 ms */
#define MAX_DECODE    1920      /* 120 ms: the longest Opus packet                    */
#define RPTQ_FRAMES   150       /* 3 s of received audio waiting for the transmitter  */

typedef enum { TX_OFF = 0, TX_DELAY, TX_ON, TX_TAIL } tx_state_t;

/* What the receiver is doing, as far as the transmitter is concerned. */
typedef enum {
    RX_FREE = 0,    /* nothing on the frequency (or a carrier that never spoke) */
    RX_UNSURE,      /* the squelch is open; no verdict yet                      */
    RX_BUSY,        /* a station is being relayed to the talkgroup              */
} rx_class_t;

/* One over coming in from the talkgroup. */
typedef struct {
    uint32_t tg;
    char     call[32];
    int      frames;        /* received                                  */
    int      lost;          /* not transmitted: channel in use, time-out */
    bool     timed_out;
} net_over_t;

struct engine {
    const config_t *cfg;
    const role_t   *role;
    audio_t        *audio;
    reflector_t    *ref;            /* NULL: no talkgroup link (a receiver)        */
    nettrx_t       *uplink;         /* the repeater this receiver serves, or NULL  */
    /* The receivers elsewhere (a transmitter's own one, and those there to
     * vote with), and who has the over: -1 nobody, 0 the receiver in this
     * box, 1 + i the i-th of these. */
    struct rxl {
        netrx_t *link;
        char     name[24];
        float    offset, siglev;
        bool     open, ignore;          /* ignore: this opening is not listened to */
        uint64_t opened_at, wait_until, better_since;
        int      lag_ms;                /* how late it is, as seen so far (0 = not seen) */
        bool     lag_set;               /* ...or as measured and set: [receiverN] lag_ms */
        int16_t  acc[AUDIO_MAX_FRAME];  /* its audio, being framed */
        int      acc_n;
        unsigned long won;
    } rxl[CFG_MAX_RECEIVERS];
    int             n_rxl;
    struct monitor *monitor;            /* somebody may be listening in */
    int             active;
    bool            own_open;           /* the receiver in this box has an over */
    uint64_t        own_at, own_better, switched_at, tx_seen_at;
    int             last_src;           /* who had the over before: -1 nobody yet */
    bool            lag_said;           /* this over: said that a better receiver is too far behind */
    uint64_t        last_began, last_end;
    bool            tx_seen;
    gpio_t         *gpio;
    status_t       *status;
    opus_chan_t    *codec;
    gate_t          gate;

    int      frame;             /* samples per frame */
    int      frame_ms;
    uint64_t t_ms;              /* frame clock       */
    uint64_t tx_wall, busy_wall;        /* the same moments as tx_since/busy_since, wall clock */

    /* ---- receiver ---- */
    bool     cos_open;          /* radio squelch as the gate sees it          */
    int      blank_left;        /* frames still ignored after a transmission  */
    int      recover_frames;
    int      rssi, rssi_peak;
    bool     local_over;        /* the gate is open: an over goes to the talkgroup */
    uint32_t local_tg;
    int      local_sent, local_lost;
    float    preamp;
    bool     ctcss_on, line_on, deemph_on;  /* software tone squelch; a squelch line; de-emphasis */
    ctcss_t  ctcss;
    deemph_t deemph;
    bool     noise_on;              /* the signal level comes from the receiver noise */
    bool     sdr_open;              /* a web SDR: the squelch made from its hiss      */
    int      sdr_settle;
    int      sdr_run;               /* ...frames the hiss has been on the other side  */
    noisemeter_t noise;
    int      noise_tick;
    agc_t    agc;               /* the normaliser on received audio            */
    agc_t    net_agc;           /* ...and the one on talkgroup audio           */
    int16_t  net_look[NET_LOOK][AUDIO_MAX_FRAME];   /* ...which looks this far ahead */
    int      look_head, look_n;
    uint64_t over_since;        /* when the over on the receiver began         */

    /* ---- repeater: received audio on its way to the transmitter ---- */
    int16_t *rptq;
    int      rq_head, rq_len;
    int      rpt_hold;          /* jitter gathering, frames left                */
    int      rpt_frames;        /* frames of this over handed to the transmitter */
    bool     rpt_lockout;       /* this over ran into the time-out               */
    bool     beep_pending, beep_net;    /* a roger beep is due; after a talkgroup over */
    bool     clip_mute;                 /* test tones: nothing else goes out with them */
    bool     call_due, clip_solo;       /* Morse for who is coming from the talkgroup; it plays alone */
    bool     beep_call, clip_after;     /* after the over: that Morse in place of the beep; it is playing */
    char     beep_who[32];
    char     call_last[32];
    bool     own_tx;                    /* keyed for Morse or an announcement, nobody has talked since */
    bool     ann_due;                   /* a spoken announcement is waiting           */
    time_t   ann_next;
    int      ann_hhmm;
    uint64_t ann_until;                 /* ...until then, after that it is skipped    */
    unsigned long n_announce;
    float    duck, duck_to;             /* gain on Morse/announcement: now, with audio */
    bool     block_said;
    bool     block_rx, block_tx, hw_keyed;  /* admin: receiver ignored, transmitter never keyed */
    bool     pa_pending;                /* the amplifier is still to be keyed (pa_delay_ms) */
    uint64_t pa_at;
    bool     calltest_due;              /* the Morse of our own call was asked for (SIGRTMIN+1) */
    bool     test_due;                  /* a test transmission was asked for (SIGUSR1) */
    uint64_t beep_at;
    uint64_t busy_since;        /* the transmitter has had something to send
                                 * without a pause since then                   */

    /* ---- transmitter ---- */
    tx_state_t  tx;
    const char *tx_why;
    uint64_t    tx_since;
    int         delay_left, tail_left, tail_frames;
    bool        lockout;        /* timed out: off until the over has ended */
    float       tx_gain, tx_level;
    struct tag *tag;                    /* the callsign tag; the audio thread adds it */
    char        tag_own[32];            /* this station, for an over on its own input */
    txshape_t   shape;
    void      (*unkey_hook)(void *);
    void       *unkey_ctx;

    /* ---- what the reflector was last told (report_tick) ---- */
    refmsg_rx_t rep_rx[1 + CFG_MAX_RECEIVERS];
    int         rep_n;          /* -1 = nothing yet, or logged in again: say it all */
    int         rep_tx;         /* -1 likewise, else 0/1 */
    int         rep_tick;
    uint64_t    rep_at;

    int16_t *netq;              /* decoded talkgroup audio waiting to be transmitted */
    int      nq_head, nq_len;
    int      net_hold;          /* jitter gathering, frames left */
    uint64_t net_fed_at;        /* when talkgroup audio last went to the sound card */
    int16_t  acc[AUDIO_MAX_FRAME];
    int      acc_n;

    bool       net_active;      /* an over is coming in */
    uint64_t   net_last;
    net_over_t over;
    lvl_t    net_lvl, rx_lvl;       /* how loud: the talkgroup over, the received one */

    /* a Morse clip: the identification, or the answer to a DTMF command */
    int16_t *clip;
    int      clip_n, clip_pos;
    bool     ident_due;
    char     ident_text[32];
    char     ack[16];
    time_t   ident_next, ident_last;

    /* ---- talkgroups ---- */
    uint32_t home_tg, sel_tg, prev_tg;
    uint64_t tg_idle_at;        /* the selected talkgroup counts as silent from here */
    char     talker[32];

    char     dtmf_buf[24];
    int      dtmf_n;
    uint64_t dtmf_last;

    /* ---- totals (the status page) ---- */
    unsigned long n_tx, tx_total_ms, n_timeouts, n_idents;
    unsigned long n_net_overs, n_net_blocked, n_dtmf_digits, n_dtmf_commands;

    bool     ptt_failed;        /* the PTT line could not be set: see ptt() */
};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */

static int ms_to_frames(const engine_t *e, int ms) { return (ms + e->frame_ms - 1) / e->frame_ms; }

static inline int16_t sat16(float v)
{
    return v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int16_t)lrintf(v);
}

static void netq_clear(engine_t *e) { e->nq_head = e->nq_len = 0; e->net_hold = 0; }

static void rptq_clear(engine_t *e) { e->rq_head = e->rq_len = 0; e->rpt_hold = 0; }

static void rptq_push(engine_t *e, const int16_t *pcm)
{
    if (e->rq_len == RPTQ_FRAMES) {             /* the transmitter is not taking it: drop the oldest */
        e->rq_head = (e->rq_head + 1) % RPTQ_FRAMES;
        e->rq_len--;
    }
    int idx = (e->rq_head + e->rq_len) % RPTQ_FRAMES;
    memcpy(e->rptq + (size_t)idx * (size_t)e->frame, pcm, sizeof(int16_t) * (size_t)e->frame);
    e->rq_len++;
}

static void rptq_pop(engine_t *e, int16_t *out)
{
    memcpy(out, e->rptq + (size_t)e->rq_head * (size_t)e->frame, sizeof(int16_t) * (size_t)e->frame);
    e->rq_head = (e->rq_head + 1) % RPTQ_FRAMES;
    e->rq_len--;
}

static bool netq_push(engine_t *e, const int16_t *pcm)
{
    if (e->nq_len == NETQ_FRAMES)
        return false;
    int idx = (e->nq_head + e->nq_len) % NETQ_FRAMES;
    memcpy(e->netq + (size_t)idx * (size_t)e->frame, pcm, sizeof(int16_t) * (size_t)e->frame);
    e->nq_len++;
    return true;
}

static void netq_pop(engine_t *e, int16_t *out)
{
    memcpy(out, e->netq + (size_t)e->nq_head * (size_t)e->frame, sizeof(int16_t) * (size_t)e->frame);
    e->nq_head = (e->nq_head + 1) % NETQ_FRAMES;
    e->nq_len--;
}

static rx_class_t rx_class(const engine_t *e)
{
    /* A repeater transmits on another frequency than it listens on: what its
     * receiver hears never holds the transmitter back. */
    if (e->role->repeater)
        return e->local_over ? RX_BUSY : RX_FREE;
    if (e->active > 0)                  /* a station heard by a receiver elsewhere */
        return RX_BUSY;
    switch (gate_state(&e->gate)) {
    case GATE_IDLE:  return RX_FREE;
    case GATE_OPEN:
    case GATE_DRAIN: return RX_BUSY;
    default:         return e->gate.gave_up ? RX_FREE : RX_UNSURE;
    }
}

/* The signal level a receiver reports to its repeater: 0..100. */
static float siglev_now(const engine_t *e)
{
    const siglev_config_t *s = &e->cfg->siglev;
    if (s->mode == SIGLEV_FIXED || e->rssi < 0)
        return (float)s->fixed;
    float v = 100.0f * (float)(e->rssi - s->rssi_lo) / (float)(s->rssi_hi - s->rssi_lo);
    return v < 1.0f ? 1.0f : v > 100.0f ? 100.0f : v;
}

static void tg_touch(engine_t *e)
{
    e->tg_idle_at = e->t_ms + (uint64_t)e->cfg->link.tg_select_timeout_s * 1000;
}

/* ------------------------------------------------------------------ */
/* the transmitter                                                     */

static void ptt(engine_t *e, bool on)
{
    /* The amplifier line follows the PTT: up before the exciter is keyed, down
     * after it has been released. */
    /* With pa_delay_ms it is the other way round: the exciter first, the
     * amplifier that much later (tx_tick), and off again before the exciter. */
    /* A blocked transmitter: the logic goes on as always, the lines stay
     * where they are. */
    e->hw_keyed = on;
    if (on && e->block_tx) {
        if (!e->block_said)
            log_warn("tx: the transmitter is BLOCKED (admin) — not keyed");
        e->block_said = true;
        return;
    }
    bool pa = e->gpio && e->cfg->radio.pa_line >= 0;
    int  pa_delay = e->cfg->radio.pa_delay_ms;
    bool pa_bad = false;
    e->pa_pending = false;
    if (pa && on) {
        if (pa_delay > 0) {
            e->pa_pending = true;
            e->pa_at = e->t_ms + (uint64_t)pa_delay;
        } else {
            pa_bad = gpio_ptt_set(e->gpio, 1, true) != 0;
        }
    }
    if (pa && !on && pa_delay > 0)
        pa_bad = gpio_ptt_set(e->gpio, 1, false) != 0;
    bool ptt_bad = e->gpio && gpio_ptt_set(e->gpio, 0, on) != 0;
    if (pa && !on && pa_delay == 0)
        pa_bad = gpio_ptt_set(e->gpio, 1, false) != 0;
    if (ptt_bad || pa_bad) {
        /* A PTT line that no longer does what it is told is the one thing
         * this daemon cannot work around from the inside. Say so: main()
         * exits, and the service's ExecStopPost takes the line afresh and
         * drives it inactive (-u). */
        log_error("tx: cannot %s the %s line", on ? "key" : "release", ptt_bad ? "PTT" : "PA");
        e->ptt_failed = true;
    }
    audio_set_ptt(e->audio, on);
}

static void key(engine_t *e, const char *why)
{
    e->own_tx = false;
    /* nobody yet: the station itself, until an over says who (docs/TAG.md) */
    tag_set(e->tag, e->tag_own, true);
    ptt(e, true);
    e->delay_left = ms_to_frames(e, e->cfg->tx.delay_ms);
    e->tx = e->delay_left > 0 ? TX_DELAY : TX_ON;
    e->tx_since = e->busy_since = e->t_ms;
    e->tx_wall = e->busy_wall = mono_ms();
    e->tx_why = why;
    e->n_tx++;
    log_info("tx: on — %s", why);
}

static void unkey(engine_t *e)
{
    ptt(e, false);
    uint64_t ms = e->t_ms - e->tx_since;
    e->tx = TX_OFF;
    e->tx_total_ms += (unsigned long)ms;
    log_info("tx: off after %.1f s", (double)ms / 1000.0);
    if (e->unkey_hook)
        e->unkey_hook(e->unkey_ctx);
}

static void clip_drop(engine_t *e)
{
    free(e->clip);
    e->clip = NULL;
    e->clip_solo = false;
    e->clip_after = false;
    e->clip_mute = false;
    e->clip_n = e->clip_pos = 0;
}

/* The roger beep: one or a few short tones when an over has been played out.
 * "blop" after a station on the input — the repeater heard you — and "bleep
 * blop" after an over from the talkgroup: that one came in over the network. */
static void beep_begin(engine_t *e, bool net)
{
    const repeater_config_t *rp = &e->cfg->repeater;
    const int *tones = net ? rp->net_tones : rp->roger_tones;
    int count = net ? rp->net_n : rp->roger_n;
    int rate = e->cfg->audio.sample_rate;
    int tone = rate * rp->roger_ms / 1000, gap = rate * rp->roger_gap_ms / 1000;
    int edge = rate * 5 / 1000;
    if (edge > tone / 2) edge = tone / 2;
    int n = count * tone + (count - 1) * gap;
    int frames = (n + e->frame - 1) / e->frame;
    if (count < 1 || frames < 1)
        return;
    int16_t *pcm = calloc((size_t)frames * (size_t)e->frame, sizeof(int16_t));
    if (!pcm)
        return;
    float amp = 32767.0f * powf(10.0f, rp->roger_level_db / 20.0f);
    for (int k = 0; k < count; k++) {
        int16_t *p = pcm + k * (tone + gap);
        float w = 2.0f * 3.14159265f * (float)tones[k] / (float)rate;
        for (int i = 0; i < tone; i++) {
            float env = 1.0f;           /* raised-cosine edges: no clicks */
            if (i < edge)              env = 0.5f - 0.5f * cosf(3.14159265f * (float)i / (float)edge);
            else if (i >= tone - edge) env = 0.5f - 0.5f * cosf(3.14159265f * (float)(tone - i) / (float)edge);
            p[i] = sat16(amp * env * sinf(w * (float)i));
        }
    }
    e->clip = pcm;
    e->clip_n = frames * e->frame;
    e->clip_pos = 0;
}

/* A test transmission, for setting the transmit audio with a deviation meter
 * or against another repeater on a receiver: 1000 Hz alone, then two tones
 * (700 + 1900 Hz, each 6 dB down so the peaks are the same), half the time
 * each. It takes the same path to the transmitter as speech does. */
#define TEST_F0   1000.0f
#define TEST_F1    700.0f
#define TEST_F2   1900.0f

static void test_begin(engine_t *e)
{
    e->test_due = false;
    int rate = e->cfg->audio.sample_rate;
    int n = rate * e->cfg->tx.test_s, half = n / 2, edge = rate / 50;
    int frames = (n + e->frame - 1) / e->frame;
    int16_t *pcm = calloc((size_t)frames * (size_t)e->frame, sizeof(int16_t));
    if (!pcm)
        return;
    float amp = 32767.0f * powf(10.0f, e->cfg->tx.test_level_db / 20.0f);
    float k = 2.0f * 3.14159265f / (float)rate;
    for (int i = 0; i < n; i++) {
        int j = i < half ? i : i - half, len = i < half ? half : n - half;
        float env = 1.0f;
        if (j < edge)             env = 0.5f - 0.5f * cosf(3.14159265f * (float)j / (float)edge);
        else if (j >= len - edge) env = 0.5f - 0.5f * cosf(3.14159265f * (float)(len - j) / (float)edge);
        float v = i < half ? sinf(k * TEST_F0 * (float)j)
                           : 0.5f * (sinf(k * TEST_F1 * (float)j) + sinf(k * TEST_F2 * (float)j));
        pcm[i] = sat16(amp * env * v);
    }
    e->clip = pcm;
    e->clip_n = frames * e->frame;
    e->clip_pos = 0;
    e->clip_mute = true;
    log_info("tx: test transmission — %d s of 1000 Hz, %d s of 700 + 1900 Hz, peak %.0f dBFS",
             e->cfg->tx.test_s / 2, e->cfg->tx.test_s - e->cfg->tx.test_s / 2,
             (double)e->cfg->tx.test_level_db);
    if (e->tx == TX_OFF)
        key(e, "test tones");
}

/* The announcement for this slot: <dir>/HHMM.wav, made today by
 * tailchaser-announce. Brought to one speech level, whatever the voice. */
static void announce_begin(engine_t *e)
{
    const announce_config_t *an = &e->cfg->announce;
    e->ann_due = false;
    char path[300];
    snprintf(path, sizeof path, "%s/%04d.wav", an->dir, e->ann_hhmm);
    struct stat st;
    time_t now = util_wall_time();
    struct tm a, b;
    if (stat(path, &st) != 0) {
        log_info("announce: no %s — nothing said (tailchaser-announce makes it)", path);
        return;
    }
    localtime_r(&now, &a);
    time_t made = st.st_mtime;
    localtime_r(&made, &b);
    (void)a; (void)b;
    if (getenv("TAILCHASER_FAKE_EPOCH") == NULL && now - made > 26 * 3600) {
        log_warn("announce: %s is more than a day old — not said (did tailchaser-announce run?)", path);
        return;
    }
    int n = 0, rate = 0;
    int16_t *pcm = wav_load(path, &n, &rate);
    if (!pcm || n < e->frame || rate != e->cfg->audio.sample_rate) {
        log_warn("announce: %s is not a %d Hz 16-bit WAV — not said", path, e->cfg->audio.sample_rate);
        free(pcm);
        return;
    }
    if (n > an->max_s * rate) n = an->max_s * rate;
    lvl_t l;
    lvl_reset(&l);
    for (int off = 0; off + e->frame <= n; off += e->frame)
        lvl_add(&l, pcm + off, e->frame);
    float gain = powf(10.0f, (an->level_db - lvl_db(&l)) / 20.0f);
    int frames = (n + e->frame - 1) / e->frame;
    int16_t *whole = calloc((size_t)frames * (size_t)e->frame, sizeof(int16_t));
    if (!whole) {
        free(pcm);
        return;
    }
    /* A synthetic voice has peaks 20 dB above its level: holding the whole
     * file back for them leaves the speech far under what a station is
     * brought to. So the level is what is asked, and the peaks are held under
     * the stops by a limiter that sees them coming: the gain of each 4 ms is
     * the lowest of it and its two neighbours on either side, with a straight
     * line between the middles — never over the ceiling, and no clicks. */
    const float ceil_v = 0.9f * 32767.0f;
    int blk = e->cfg->audio.sample_rate / 250;
    if (blk < 1) blk = 1;
    int nb = (n + blk - 1) / blk;
    float *bg = malloc(sizeof(float) * (size_t)nb * 2);
    if (bg) {
        float *bm = bg + nb;
        for (int q = 0; q < nb; q++) {
            int pk = 0;
            for (int i = q * blk; i < n && i < (q + 1) * blk; i++) {
                int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
                if (v > pk) pk = v;
            }
            float out = (float)pk * gain;
            bg[q] = out > ceil_v ? ceil_v / out : 1.0f;
        }
        for (int q = 0; q < nb; q++) {
            float m = bg[q];
            for (int k = q - 2; k <= q + 2; k++)
                if (k >= 0 && k < nb && bg[k] < m) m = bg[k];
            bm[q] = m;
        }
        for (int i = 0; i < n; i++) {
            float pos = ((float)i - (float)blk * 0.5f) / (float)blk;
            int b0 = pos < 0 ? 0 : (int)pos;
            if (b0 > nb - 1) b0 = nb - 1;
            int b1 = b0 + 1 < nb ? b0 + 1 : b0;
            float f = pos - (float)b0;
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            whole[i] = sat16((float)pcm[i] * gain * (bm[b0] + (bm[b1] - bm[b0]) * f));
        }
        free(bg);
    } else {
        float top = ceil_v / (float)(l.peak > 0 ? l.peak : 1);
        if (gain > top) gain = top;
        for (int i = 0; i < n; i++)
            whole[i] = sat16((float)pcm[i] * gain);
    }
    free(pcm);
    e->clip = whole;
    e->clip_n = frames * e->frame;
    e->clip_pos = 0;
    e->n_announce++;
    log_info("announce: %02d:%02d, %.1f s%s", e->ann_hhmm / 100, e->ann_hhmm % 100, (double)n / rate,
             e->tx == TX_OFF ? "" : e->own_tx ? ", after the Morse" : ", on the transmission that is on the air");
    if (e->tx == TX_OFF) {
        key(e, "announcement");
        e->own_tx = true;
    }
}

static time_t next_slot(time_t now, int interval_min);

void engine_call_test(engine_t *e)
{
    if (!e->role->tx)
        log_warn("Morse call asked for — this program has no transmitter");
    else
        e->calltest_due = true;
}

void engine_announce_now(engine_t *e)
{
    const announce_config_t *an = &e->cfg->announce;
    if (!e->role->tx || !an->enabled) {
        log_warn("announcement asked for — %s", e->role->tx ? "[announce] enabled is not true"
                                                            : "this program has no transmitter");
        return;
    }
    time_t slot = next_slot(util_wall_time(), an->interval_min);
    struct tm tm;
    localtime_r(&slot, &tm);
    e->ann_hhmm = tm.tm_hour * 100 + tm.tm_min;
    e->ann_due = true;
    e->ann_until = e->t_ms + (uint64_t)an->wait_s * 1000;
    log_info("announce: asked for by hand — the one of %02d:%02d, now", tm.tm_hour, tm.tm_min);
}

void engine_set_block(engine_t *e, bool rx, bool tx)
{
    if (rx != e->block_rx) {
        e->block_rx = rx;
        for (int i = 0; rx && i < e->n_rxl; i++)        /* a local one closes with the next frame */
            if (e->rxl[i].open)
                engine_remote_squelch(e, i, false, e->rxl[i].siglev);
        log_warn("rx: the receiver is %s", rx ? "BLOCKED (admin): nothing it hears goes anywhere" : "released");
    }
    if (tx != e->block_tx) {
        bool keyed = e->tx != TX_OFF;
        if (tx && keyed) {
            e->block_tx = false;
            ptt(e, false);                      /* off the air, now */
            e->hw_keyed = true;
        }
        e->block_tx = tx;
        e->block_said = false;
        log_warn("tx: the transmitter is %s", tx ? "BLOCKED (admin): it is not keyed, whatever comes in" : "released");
        if (!tx && keyed)
            ptt(e, true);                       /* in the middle of an over: on the air again */
    }
}

bool engine_idle(const engine_t *e)
{
    return e->tx == TX_OFF && !e->cos_open && !e->net_active && !e->local_over;
}

void engine_test_tx(engine_t *e)
{
    if (!e->role->tx)
        log_warn("test transmission asked for — this program has no transmitter");
    else
        e->test_due = true;
}

/* Start the Morse clip that is waiting: the identification first. */
static void clip_begin(engine_t *e)
{
    const ident_config_t *id = &e->cfg->ident;
    bool ident = e->ident_due;
    char text[32];
    snprintf(text, sizeof text, "%s", ident ? e->ident_text : e->ack);
    if (ident) e->ident_due = false;
    else       e->ack[0] = '\0';

    int n = 0;
    int16_t *pcm = cw_render(text, id->wpm, id->pitch_hz, id->level_db,
                             e->cfg->audio.sample_rate, 0, 0, &n);
    if (!pcm)
        return;
    /* whole frames, the last one padded with silence */
    int frames = (n + e->frame - 1) / e->frame;
    int16_t *whole = calloc((size_t)frames * (size_t)e->frame, sizeof(int16_t));
    if (!whole) {
        free(pcm);
        return;
    }
    memcpy(whole, pcm, sizeof(int16_t) * (size_t)n);
    free(pcm);
    e->clip = whole;
    e->clip_n = frames * e->frame;
    e->clip_pos = 0;

    bool over_audio = e->tx != TX_OFF;
    if (ident) {
        e->n_idents++;
        e->ident_last = util_wall_time();
        log_info("ident: %s in Morse%s", text, over_audio ? ", over the audio on the air" : "");
    } else {
        log_info("dtmf: answering \"%s\" in Morse", text);
    }
    if (e->tx == TX_OFF) {
        key(e, ident ? "identification" : "DTMF answer");
        e->own_tx = true;
    }
}

/* Top the sound card queue up: talkgroup audio, with the Morse clip mixed in
 * while one is running. */
/* The station that is about to be heard from the talkgroup, in fast Morse:
 * a repeater on the reflector (ON0OST) by the letters after the prefix
 * ("OST"), anybody else as "INET". Its audio waits in the queue meanwhile, so
 * nothing is lost and nothing is added after the over. */
static void call_send(engine_t *e, const char *call);

static void call_begin(engine_t *e)
{
    e->call_due = false;
    const char *call = e->over.call[0] ? e->over.call : e->talker;
    if (e->cfg->repeater.net_call_once && call[0] && !strcasecmp(call, e->call_last))
        return;                                 /* the repeater that was announced last: not again */
    call_send(e, call);
    if (e->clip)                                /* it was announced: remembered until another one comes */
        snprintf(e->call_last, sizeof e->call_last, "%s", call);
}

static void call_send(engine_t *e, const char *call)
{
    const repeater_config_t *rp = &e->cfg->repeater;
    char text[16];
    size_t pl = strlen(rp->net_call_prefix);
    if (pl > 0 && !strncasecmp(call, rp->net_call_prefix, pl) && call[pl] && call[pl] != '-') {
        size_t o = 0;
        for (const char *p = call + pl; *p && *p != '-' && o + 1 < sizeof text; p++)
            text[o++] = *p;
        text[o] = '\0';
    } else if (rp->net_call_full) {
        /* not a repeater: the station itself, without what follows a dash
         * (ON4ABC-P is ON4ABC: the dash names the set, not the station) */
        size_t o = 0;
        for (const char *p = call; *p && *p != '-' && o + 1 < sizeof text; p++)
            text[o++] = *p;
        text[o] = '\0';
    } else {
        snprintf(text, sizeof text, "%s", rp->net_call_other);     /* empty: nothing is sent */
    }
    char clean[16];
    if (cw_sanitize(text, clean, (int)sizeof clean) == 0)
        return;
    int n = 0;
    /* A transmitter that was off: the listeners' squelch has to open first,
     * or the first letter is gone. */
    int lead = e->tx == TX_OFF || e->tx == TX_DELAY ? rp->net_call_lead_ms : 0;
    int16_t *pcm = cw_render(clean, rp->net_call_wpm, rp->net_call_pitch_hz, rp->net_call_level_db,
                             e->cfg->audio.sample_rate, lead, 150, &n);
    if (!pcm)
        return;
    int frames = (n + e->frame - 1) / e->frame;
    int16_t *whole = calloc((size_t)frames * (size_t)e->frame, sizeof(int16_t));
    if (!whole) {
        free(pcm);
        return;
    }
    memcpy(whole, pcm, sizeof(int16_t) * (size_t)n);
    free(pcm);
    e->clip = whole;
    e->clip_n = frames * e->frame;
    e->clip_pos = 0;
    e->clip_solo = true;
    log_debug("net: %s — \"%s\" in Morse", call[0] ? call : "?", clean);
}

/* The tag says which station an over from the talkgroup came from: its
 * callsign without what follows a dash (ON4ABC-KNOB is ON4ABC), as in Morse. */
static void tag_who(engine_t *e, const char *call)
{
    if (!e->tag)
        return;
    char who[32];
    snprintf(who, sizeof who, "%s", call ? call : "");
    who[strcspn(who, "-")] = '\0';
    if (who[0])
        tag_set(e->tag, who, false);
    else
        tag_set(e->tag, e->tag_own, true);
}

static void feed(engine_t *e)
{
    int16_t out[AUDIO_MAX_FRAME];
    while (audio_tx_queued(e->audio) < FIFO_TARGET) {
        bool have = false;
        /* A test transmission is for measuring: the receiver and the
         * talkgroup are kept out of it until the tones are over. */
        if (e->clip && e->clip_mute) {
            rptq_clear(e);
            netq_clear(e);
        }
        /* A repeater sends what its receiver hears first. Talkgroup audio that
         * came in at the same time is not sent: the station on the input wins. */
        if (e->rq_len > 0 && (e->rpt_hold == 0 || e->rq_len >= JITTER_FRAMES || !e->local_over)) {
            rptq_pop(e, out);
            e->own_tx = false;
            tag_set(e->tag, e->tag_own, true);          /* a station on the input */
            e->rpt_hold = 0;
            have = true;
            if (e->nq_len > 0) {
                e->over.lost += e->nq_len;
                netq_clear(e);
            }
        }
        if (!have && e->nq_len > 0 && e->call_due && !e->clip)
            call_begin(e);
        if (!have && e->nq_len > 0 && !e->clip_solo && (e->net_hold == 0 || e->nq_len >= JITTER_FRAMES)) {
            netq_pop(e, out);
            e->own_tx = false;
            tag_who(e, e->over.call[0] ? e->over.call : e->talker);
            e->net_hold = 0;
            e->net_fed_at = e->t_ms;
            have = true;
        }
        if (e->clip) {
            const int16_t *c = e->clip + e->clip_pos;
            /* somebody is talking: the Morse or the announcement goes under
             * them, quieter — and comes back up when they stop */
            float g0 = e->duck, g1 = have ? e->duck_to : 1.0f;
            if (g1 > g0 * 1.26f) g1 = g0 * 1.26f;           /* back up at 2 dB a frame */
            for (int i = 0; i < e->frame; i++) {
                float g = g0 + (g1 - g0) * (float)(i + 1) / (float)e->frame;
                out[i] = sat16((have ? (float)out[i] : 0.0f) + g * (float)c[i]);
            }
            e->duck = g1;
            e->clip_pos += e->frame;
            if (e->clip_pos >= e->clip_n)
                clip_drop(e);
            have = true;
        }
        if (!have)
            break;
        if (e->tx_level != 1.0f)
            for (int i = 0; i < e->frame; i++)
                out[i] = sat16((float)out[i] * e->tx_level);
        txshape_process(&e->shape, out, e->frame);
        if (audio_tx_push(e->audio, out, e->frame) != 0)
            break;
        monitor_feed(e->monitor, MONITOR_TX, out, e->frame);
    }
    if (e->net_hold > 0)
        e->net_hold--;
    if (e->rpt_hold > 0)
        e->rpt_hold--;
}

static void net_over_end(engine_t *e);
static void net_out(engine_t *e, int16_t *pcm);

static void tx_tick(engine_t *e)
{
    const tx_config_t *t = &e->cfg->tx;

    if (e->pa_pending && e->t_ms >= e->pa_at) {         /* the amplifier follows the exciter */
        e->pa_pending = false;
        if (e->gpio && gpio_ptt_set(e->gpio, 1, true) != 0) {
            log_error("tx: cannot key the PA line");
            e->ptt_failed = true;
        }
    }

    /* ---- time-out: a transmission may not go on for ever ---- */
    /* A simplex node counts the carrier. A repeater's carrier stays up through
     * a whole QSO, so there it is the time without a pause that counts: one
     * over, from the input or from the talkgroup. */
    uint64_t since = e->role->repeater ? e->busy_since : e->tx_since;
    /* Counted in audio frames, and on the clock on the wall as well: a loop
     * that was held up must not make a time-out longer. */
    uint64_t wall = e->role->repeater ? e->busy_wall : e->tx_wall;
    if (e->tx != TX_OFF && t->timeout_s > 0 &&
        (e->t_ms - since >= (uint64_t)t->timeout_s * 1000 ||
         mono_ms() - wall >= (uint64_t)t->timeout_s * 1000 + 2000)) {
        log_warn("tx: TIME-OUT after %d s — transmitter off%s", t->timeout_s,
                 e->net_active || e->local_over ? " until this over has ended" : "");
        e->n_timeouts++;
        if (e->net_active) {
            e->lockout = true;
            e->over.timed_out = true;
            e->over.lost += e->nq_len;
        }
        if (e->role->repeater && e->local_over)
            e->rpt_lockout = true;
        rptq_clear(e);
        e->beep_pending = false;
        netq_clear(e);
        clip_drop(e);
        audio_tx_flush(e->audio);
        unkey(e);
        return;
    }

    rx_class_t rx = rx_class(e);

    /* ---- talkgroup audio that was waiting for a verdict on the squelch ---- */
    if (e->tx == TX_OFF && e->nq_len > 0) {
        if (rx == RX_BUSY) {                    /* it is a voice: the station wins */
            e->over.lost += e->nq_len;
            netq_clear(e);
        } else if (rx == RX_FREE) {
            key(e, "talkgroup audio");
        }
    }

    /* ---- a repeater: what the receiver hears goes out ---- */
    if (e->tx == TX_OFF && e->rq_len > 0)
        key(e, "a station on the input");

    /* ---- a test transmission, when nobody is using the transmitter ---- */
    if (e->calltest_due && !e->clip && e->nq_len == 0 && e->rq_len == 0 && !e->net_active && !e->local_over) {
        e->calltest_due = false;
        const char *own = e->cfg->ident.callsign;
        call_send(e, own);
        if (e->clip) {
            log_info("tx: Morse call by hand — %s as others hear it coming, %d wpm", own,
                     e->cfg->repeater.net_call_wpm);
            if (e->tx == TX_OFF)
                key(e, "Morse call test");
        } else {
            log_warn("tx: Morse call by hand — %s does not start with [repeater] net_call_prefix \"%s\": "
                     "nothing would be sent for it", own, e->cfg->repeater.net_call_prefix);
        }
    }
    if (e->test_due && !e->clip && e->tx == TX_OFF && e->nq_len == 0 && e->rq_len == 0 &&
        !e->net_active && !e->local_over && rx == RX_FREE)
        test_begin(e);

    /* ---- a spoken announcement: in a quiet moment, or under the conversation ---- */
    if (e->ann_due && !e->clip && !e->ident_due && !e->ack[0]) {
        /* quiet: the transmitter is off — or on only for our own Morse */
        bool quiet = (e->tx == TX_OFF || e->own_tx) && e->nq_len == 0 && e->rq_len == 0 && !e->net_active &&
                     !e->local_over && rx == RX_FREE;
        bool can = quiet || (e->cfg->announce.duck && (e->role->repeater || rx == RX_FREE));
        if (can)
            announce_begin(e);
        else if (e->t_ms >= e->ann_until) {
            e->ann_due = false;
            log_info("announce: %02d:%02d skipped — the frequency was in use", e->ann_hhmm / 100, e->ann_hhmm % 100);
        }
    }

    /* ---- Morse waiting for the frequency (a repeater sends it over the audio) ---- */
    if (!e->clip && (e->ident_due || e->ack[0]) && (rx == RX_FREE || e->role->repeater))
        clip_begin(e);

    /* ---- the roger beep, once the over has been played out ---- */
    if (e->beep_pending) {
        if (e->local_over || e->net_active || e->tx == TX_OFF)
            e->beep_pending = false;            /* somebody talks again, or the carrier is gone */
        else if (e->beep_net && e->nq_len > 0)  /* still being played: the pause comes after it */
            e->beep_at = e->t_ms + (uint64_t)e->cfg->repeater.roger_delay_ms;
        else if (!e->clip && e->rq_len == 0 && e->nq_len == 0 && e->t_ms >= e->beep_at) {
            e->beep_pending = false;
            bool said = false;
            if (e->beep_net && e->beep_call &&
                !(e->cfg->repeater.net_call_once && e->beep_who[0] && !strcasecmp(e->beep_who, e->call_last))) {
                call_send(e, e->beep_who);
                if (e->clip) {
                    said = true;
                    e->clip_solo = false;       /* nothing waits for it */
                    e->clip_after = true;
                    snprintf(e->call_last, sizeof e->call_last, "%s", e->beep_who);
                }
            }
            if (!said && (!e->beep_net || (e->cfg->repeater.roger_beep && e->cfg->repeater.roger_net_beep)))
                beep_begin(e, e->beep_net);
        }
    }

    switch (e->tx) {
    case TX_OFF:
        return;
    case TX_DELAY:
        if (--e->delay_left > 0)
            return;
        e->tx = TX_ON;
        break;
    default:
        break;
    }

    feed(e);

    /* Nothing left to send and nothing more coming: wait until the sound card
     * has played the last frame (plus the hang time), then drop the carrier. */
    bool idle = e->nq_len == 0 && !e->clip && !e->net_active && audio_tx_queued(e->audio) == 0 &&
                e->rq_len == 0 && !e->beep_pending && !(e->role->repeater && e->local_over && !e->rpt_lockout);
    if (!idle) {
        if (e->tx == TX_TAIL) {
            e->busy_since = e->t_ms;            /* a pause ended: a new over */
            e->busy_wall = mono_ms();
        }
        e->tx = TX_ON;
        return;
    }
    if (e->tx == TX_ON) {
        e->tx = TX_TAIL;
        e->tail_left = e->tail_frames;
    }
    if (e->tail_left-- <= 0)
        unkey(e);
}

/* ------------------------------------------------------------------ */
/* talkgroup -> radio                                                  */

static void net_over_begin(engine_t *e)
{
    e->net_active = true;
    lvl_reset(&e->net_lvl);
    agc_begin_over(&e->net_agc);
    e->look_head = e->look_n = 0;
    memset(&e->over, 0, sizeof e->over);
    e->over.tg = e->sel_tg;
    snprintf(e->over.call, sizeof e->over.call, "%s", e->talker);
    e->n_net_overs++;
    /* which repeater it is, in Morse, before it is heard — when it is another
     * one than was announced last */
    e->call_due = e->role->repeater && e->cfg->repeater.net_call && !e->cfg->repeater.net_call_after;
    /* the Morse of the over before this one, still playing: the speech does not wait for it */
    if (e->clip && e->clip_after)
        clip_drop(e);
}

static void net_over_end(engine_t *e)
{
    if (!e->net_active)
        return;
    while (e->look_n > 0) {                     /* what the normaliser still held */
        if (!e->lockout)
            net_out(e, e->net_look[e->look_head]);
        e->look_head = (e->look_head + 1) % NET_LOOK;
        e->look_n--;
    }
    e->net_active = false;
    e->lockout = false;
    e->acc_n = 0;

    net_over_t *o = &e->over;
    if (o->lost > o->frames) o->lost = o->frames;
    const char *outcome = o->timed_out          ? "timed out"
                        : o->lost == 0          ? "transmitted"
                        : o->lost == o->frames  ? "channel busy"
                                                : "partly";
    if (o->lost > 0 && !o->timed_out)
        e->n_net_blocked++;
    /* what was transmitted of a talkgroup over ends with "bleep blop" */
    /* ...or, after it, with which repeater that was, in Morse ([repeater]
     * net_call_when = after): then no beep — unless it has no letters to send */
    bool call_after = e->cfg->repeater.net_call && e->cfg->repeater.net_call_after;
    if (e->role->repeater && (call_after || (e->cfg->repeater.roger_beep && e->cfg->repeater.roger_net_beep)) &&
        !o->timed_out && o->frames > o->lost && e->tx != TX_OFF && !e->local_over) {
        e->beep_pending = true;
        e->beep_net = true;
        e->beep_call = call_after;
        snprintf(e->beep_who, sizeof e->beep_who, "%s", o->call[0] ? o->call : e->talker);
        e->beep_at = e->t_ms + (uint64_t)e->cfg->repeater.roger_delay_ms;
    }
    int ms = o->frames * e->frame_ms;
    char lev[48] = "";
    float nin, ngain;
    if (e->cfg->tx.normalize && agc_over_stats(&e->net_agc, &nin, &ngain))
        snprintf(lev, sizeof lev, ", levelled %+.0f dB", (double)ngain);
    log_info("net: %s on TG %u, %.1f s [level %.0f dBFS, peak %.0f%s] — %s", o->call[0] ? o->call : "?",
             (unsigned)o->tg, ms / 1000.0, (double)lvl_db(&e->net_lvl), (double)lvl_peak_db(&e->net_lvl), lev,
             o->timed_out ? "cut by the transmit time-out"
             : o->lost == 0 ? "transmitted"
             : o->lost == o->frames ? "NOT transmitted: a station was on the frequency"
             : "partly transmitted: a station was on the frequency");

    status_netover_t n;
    memset(&n, 0, sizeof n);
    n.when = util_wall_time();
    n.duration_ms = ms;
    n.tg = o->tg;
    snprintf(n.call, sizeof n.call, "%s", o->call);
    snprintf(n.outcome, sizeof n.outcome, "%s", outcome);
    status_snapshot_t *d = status_lock(e->status);
    status_add_netover(d, &n);
    status_unlock(e->status);
}

/* One decoded 20 ms frame from the talkgroup. */
static void net_frame(engine_t *e, int16_t *pcm)
{
    e->net_last = e->t_ms;
    if (!e->net_active)
        net_over_begin(e);
    e->over.frames++;
    tg_touch(e);

    if (e->lockout) {
        e->over.lost++;
        return;
    }
    if (e->role->repeater && e->local_over) {   /* a station is on the input */
        e->over.lost++;
        return;
    }
    if (e->tx == TX_OFF) {
        rx_class_t rx = rx_class(e);
        if (rx == RX_BUSY) {            /* a station is talking on the frequency */
            e->over.lost++;
            return;
        }
        if (rx == RX_FREE)
            key(e, "talkgroup audio");
        /* RX_UNSURE: the squelch is open but nothing says it is a voice. Keep
         * the audio; tx_tick() keys or drops it once the gate has decided. */
    }
    /* One level for everything that is transmitted, wherever it comes from.
     * The level is measured on this frame, the gain goes on the one from
     * 100 ms ago: a programme that turns loud after a quiet passage does not
     * come out with a spike. */
    if (e->cfg->tx.normalize) {
        agc_measure(&e->net_agc, pcm, e->frame);
        size_t bytes = sizeof(int16_t) * (size_t)e->frame;
        if (e->look_n < NET_LOOK) {
            memcpy(e->net_look[(e->look_head + e->look_n++) % NET_LOOK], pcm, bytes);
            return;
        }
        int16_t old[AUDIO_MAX_FRAME];
        memcpy(old, e->net_look[e->look_head], bytes);
        memcpy(e->net_look[e->look_head], pcm, bytes);
        e->look_head = (e->look_head + 1) % NET_LOOK;
        net_out(e, old);
        return;
    }
    net_out(e, pcm);
}

/* A frame of talkgroup audio on its way to the transmitter. */
static void net_out(engine_t *e, int16_t *pcm)
{
    agc_apply(&e->net_agc, pcm, e->frame);
    if (e->tx_gain != 1.0f)
        for (int i = 0; i < e->frame; i++)
            pcm[i] = sat16((float)pcm[i] * e->tx_gain);

    /* On a transmitter that is already up, gather a few frames first so
     * network jitter does not chop the audio. (After keying, the TX delay
     * has gathered far more than that.) */
    if (e->nq_len == 0 && (e->tx == TX_ON || e->tx == TX_TAIL) &&
        e->t_ms - e->net_fed_at > 2u * (unsigned)e->frame_ms)
        e->net_hold = JITTER_FRAMES;

    if (!netq_push(e, pcm))
        e->over.lost++;
}

void engine_on_net_audio(engine_t *e, const uint8_t *opus, int len)
{
    if (len <= 0) {                     /* the talker stopped */
        net_over_end(e);
        return;
    }
    int16_t pcm[MAX_DECODE];
    int n = opus_chan_decode(e->codec, opus, len, pcm, MAX_DECODE);
    if (n <= 0)
        return;
    /* Nodes send 20 ms packets, but nothing in the protocol says they must. */
    for (int off = 0; off < n;) {
        int take = e->frame - e->acc_n;
        if (take > n - off) take = n - off;
        memcpy(e->acc + e->acc_n, pcm + off, sizeof(int16_t) * (size_t)take);
        e->acc_n += take;
        off += take;
        if (e->acc_n == e->frame) {
            e->acc_n = 0;
            lvl_add(&e->net_lvl, e->acc, e->frame);
            net_frame(e, e->acc);
        }
    }
}

/* ------------------------------------------------------------------ */
/* talkgroups                                                          */

static void select_tg(engine_t *e, uint32_t tg, const char *why)
{
    if (tg == e->sel_tg)
        return;
    bool in_flight = e->net_active;
    if (e->net_active) {                /* leaving in the middle of an over */
        net_over_end(e);
        netq_clear(e);
    }
    e->prev_tg = e->sel_tg;
    e->sel_tg = tg;
    e->talker[0] = '\0';
    if (e->local_over)
        e->local_tg = tg;
    tg_touch(e);
    if (e->ref)
        reflector_select_tg(e->ref, tg, in_flight);
    if (tg)
        log_info("tg: %u selected — %s", (unsigned)tg, why);
    else
        log_info("tg: none selected — %s", why);
}

static bool tg_listed(const engine_t *e, uint32_t tg)
{
    return tg != 0 && (tg == e->home_tg || config_tg_monitored(e->cfg, tg));
}

/* Somebody started talking on a talkgroup that is monitored, not selected. */
static void tg_activity_elsewhere(engine_t *e, uint32_t tg, const char *call)
{
    if (!tg_listed(e, tg) || e->local_over)
        return;
    bool silent = e->sel_tg == 0 || (!e->net_active && e->t_ms >= e->tg_idle_at);
    if (!silent && config_tg_prio(e->cfg, tg) <= config_tg_prio(e->cfg, e->sel_tg)) {
        log_debug("tg: %s on TG %u — staying on %u", call, (unsigned)tg, (unsigned)e->sel_tg);
        return;
    }
    char why[64];
    snprintf(why, sizeof why, "%s is talking there", call[0] ? call : "somebody");
    select_tg(e, tg, why);
    snprintf(e->talker, sizeof e->talker, "%s", call);
}

void engine_on_event(engine_t *e, const reflector_event_t *ev)
{
    switch (ev->type) {
    case RE_CONNECTED:
        e->talker[0] = '\0';
        break;
    case RE_DISCONNECTED:
        log_warn("reflector: connection lost");
        net_over_end(e);
        e->talker[0] = '\0';
        break;
    case RE_TALKER_START:
        if (!strcasecmp(ev->callsign, e->cfg->link.callsign))
            break;                      /* that is this node */
        if (ev->tg == e->sel_tg) {
            snprintf(e->talker, sizeof e->talker, "%s", ev->callsign);
            if (e->net_active && !e->over.call[0])
                snprintf(e->over.call, sizeof e->over.call, "%s", ev->callsign);
        } else {
            tg_activity_elsewhere(e, ev->tg, ev->callsign);
        }
        break;
    case RE_TALKER_STOP:
        if (ev->tg == e->sel_tg && !strcasecmp(ev->callsign, e->talker))
            e->talker[0] = '\0';
        break;
    }
}

/* <prefix><TG>#  select a talkgroup      <prefix>0#  back to the home talkgroup
 * <prefix>#      the talkgroup used before this one */
static void dtmf_command(engine_t *e, const char *cmd)
{
    const char *pfx = e->cfg->dtmf.tg_prefix;
    size_t pl = strlen(pfx);
    if (strncmp(cmd, pfx, pl) != 0) {
        log_info("dtmf: %s# is not a command of this node", cmd);
        return;
    }
    const char *arg = cmd + pl;
    uint32_t tg;
    if (*arg == '\0') {
        tg = e->prev_tg;
    } else {
        if (strlen(arg) > 9 || strspn(arg, "0123456789") != strlen(arg)) {
            log_info("dtmf: %s# — not a talkgroup number", cmd);
            return;
        }
        tg = (uint32_t)strtoul(arg, NULL, 10);
    }
    if (tg == 0)
        tg = e->home_tg;
    e->n_dtmf_commands++;
    log_info("dtmf: command %s# — talkgroup %u", cmd, (unsigned)tg);
    select_tg(e, tg, "DTMF command");
    tg_touch(e);
    if (e->cfg->dtmf.ack)
        snprintf(e->ack, sizeof e->ack, "%u", (unsigned)tg);
}

/* ------------------------------------------------------------------ */
/* the receiver: an over begins, its audio, it ends                    */
/*                                                                     */
/* Called by the gate (a local receiver) or by the link to the remote  */
/* receiver (a transmitter). Where the over goes depends on the role:  */
/* the talkgroup, the repeater this receiver serves, the transmitter.  */

static void rx_over_begin(engine_t *e)
{
    e->local_over = true;
    e->local_tg = e->sel_tg;
    e->local_sent = e->local_lost = 0;
    e->over_since = e->t_ms;
    e->rpt_frames = 0;
    e->rpt_lockout = false;
    e->beep_pending = false;
    agc_begin_over(&e->agc);
    lvl_reset(&e->rx_lvl);
    if (e->uplink)
        nettrx_squelch(e->uplink, true, siglev_now(e));
    if (e->role->repeater) {
        e->rpt_hold = JITTER_FRAMES;
        if (e->nq_len > 0) {                    /* the station on the input wins */
            e->over.lost += e->nq_len;
            netq_clear(e);
        }
    }
    if (e->role->reflector)
        log_debug("rx: over begins — to TG %u", (unsigned)e->local_tg);
    tg_touch(e);
}

static void rx_over_end(engine_t *e)
{
    if (!e->local_over)
        return;
    if (e->ref && e->local_sent > 0)
        reflector_tx_stop(e->ref);
    if (e->uplink)
        nettrx_squelch(e->uplink, false, siglev_now(e));
    e->local_over = false;
    if (e->role->repeater && e->cfg->gate.tail_silence) {
        /* The station has unkeyed and listens to the output. The transmitter
         * runs behind its receiver (the carrier's delay, the queue, the sound
         * card): what still waits is the end of that station's own over, and
         * it would hear it come back. Not sent. */
        rptq_clear(e);
        audio_tx_flush(e->audio);
    }
    if (e->role->repeater && e->rpt_frames > 0 && !e->rpt_lockout && e->cfg->repeater.roger_beep) {
        e->beep_pending = true;
        e->beep_net = false;
        e->beep_at = e->t_ms + (uint64_t)e->cfg->repeater.roger_delay_ms;
    }
    e->rpt_lockout = false;
    float in_db, gain_db;
    if (agc_over_stats(&e->agc, &in_db, &gain_db))
        log_info("rx: normaliser — %.0f dBFS in, %+.0f dB, %.0f dBFS out (peak %.0f)", (double)in_db,
                 (double)gain_db, (double)lvl_db(&e->rx_lvl), (double)lvl_peak_db(&e->rx_lvl));
    tg_touch(e);
}

/* One frame of an over. tail = the station has already unkeyed and this is
 * audio the gate still had in its delay line. */
static void rx_over_audio(engine_t *e, const int16_t *pcm, int n, bool tail)
{
    if (!e->local_over)
        return;
    tg_touch(e);
    if (n != e->frame) {
        e->local_lost++;
        return;
    }
    int16_t buf[AUDIO_MAX_FRAME];
    if (e->preamp != 1.0f) {
        for (int i = 0; i < n; i++)
            buf[i] = sat16((float)pcm[i] * e->preamp);
    } else {
        memcpy(buf, pcm, sizeof(int16_t) * (size_t)n);
    }
    agc_process(&e->agc, buf, n);
    lvl_add(&e->rx_lvl, buf, n);

    /* ---- to the repeater this receiver serves ---- */
    if (e->uplink) {
        nettrx_status_t us;
        nettrx_get_status(e->uplink, &us);
        if (us.connected) e->local_sent++;
        else              e->local_lost++;
        nettrx_audio(e->uplink, buf, n);
    }

    /* ---- to the talkgroup ---- */
    if (e->ref) {
        if (e->local_tg == 0) {
            e->local_lost++;
        } else {
            uint8_t pkt[REFLECTOR_MAX_OPUS];
            int len = opus_chan_encode(e->codec, buf, n, pkt, (int)sizeof pkt);
            if (len > 0 && reflector_send_audio(e->ref, pkt, len) == 0)
                e->local_sent++;
            else
                e->local_lost++;
        }
    }

    /* ---- to the transmitter ---- */
    if (e->role->repeater && !e->rpt_lockout) {
        /* All of it, the gate's delay line included: the stations listening
         * to the repeater must hear the over to its last word. */
        (void)tail;
        rptq_push(e, buf);
        e->rpt_frames++;
    }
}

/* <prefix><TG>#, digit by digit. */
static void rx_digit(engine_t *e, char digit, int duration_ms)
{
    e->n_dtmf_digits++;
    log_info("rx: DTMF %c (%d ms)", digit, duration_ms);
    if (e->uplink)
        nettrx_dtmf(e->uplink, digit, duration_ms);
    if (!e->role->reflector || !e->cfg->dtmf.commands)
        return;
    if (e->dtmf_n > 0 && e->t_ms - e->dtmf_last > DTMF_IDLE_MS)
        e->dtmf_n = 0;
    e->dtmf_last = e->t_ms;
    if (digit == '#') {
        e->dtmf_buf[e->dtmf_n] = '\0';
        e->dtmf_n = 0;
        if (e->dtmf_buf[0])
            dtmf_command(e, e->dtmf_buf);
        return;
    }
    if (e->dtmf_n + 1 >= (int)sizeof e->dtmf_buf) {
        e->dtmf_n = 0;
        return;
    }
    e->dtmf_buf[e->dtmf_n++] = digit;
}

/* ---- a local receiver: the gate's sink ---- */

static void vote_open(engine_t *e, int s);
static void vote_close(engine_t *e, int s);

static void sink_squelch(void *ctx, bool open)
{
    engine_t *e = ctx;
    e->own_open = open;
    if (open) {
        e->own_at = e->t_ms;
        vote_open(e, 0);
    } else {
        vote_close(e, 0);
    }
}

static void sink_audio(void *ctx, const int16_t *pcm, int n)
{
    engine_t *e = ctx;
    if (e->active == 0)
        rx_over_audio(e, pcm, n, !e->cos_open);
}

static void sink_dtmf(void *ctx, char digit, int duration_ms)
{
    rx_digit(ctx, digit, duration_ms);
}

/* ---- a remote receiver: the link's sink (netrx.c) ---- */

static void remote_over_done(engine_t *e, int i)
{
    int ms = (int)(e->t_ms - e->over_since);
    char where[96] = "";
    if (e->local_sent > 0)
        snprintf(where, sizeof where, " -> TG %u", (unsigned)e->local_tg);
    else if (e->local_tg == 0)
        snprintf(where, sizeof where, " — NOT SENT: no talkgroup selected");
    else
        snprintf(where, sizeof where, " — NOT SENT: the reflector is not connected");
    float in_db = -120.0f, gain_db = 0.0f;
    bool lvl = agc_over_stats(&e->agc, &in_db, &gain_db);
    log_info("rx: %.2f s from %s [signal %.0f, level %.0f dBFS]%s%s", ms / 1000.0, e->rxl[i].name,
             (double)e->rxl[i].siglev, (double)in_db,
             e->rpt_lockout ? ", cut by the time-out" : ", repeated", where);

    status_opening_t o;
    memset(&o, 0, sizeof o);
    o.when         = util_wall_time();
    o.duration_ms  = ms;
    o.speech_at_ms = -1;
    o.level_avg_db = lvl ? in_db : -120.0f;
    o.rssi         = (int)lrintf(e->rxl[i].siglev);
    o.tg           = e->local_sent > 0 ? e->local_tg : 0;
    snprintf(o.pass, sizeof o.pass, "%s", e->n_rxl > 1 || e->role->rx ? e->rxl[i].name : "receiver");
    snprintf(o.would, sizeof o.would, "receiver");
    snprintf(o.looks_like, sizeof o.looks_like, "–");
    status_snapshot_t *d = status_lock(e->status);
    d->openings++;
    d->passed++;
    status_add_opening(d, &o);
    status_unlock(e->status);
}

/* ---- several receivers: the voter ----
 *
 * One receiver has the over; what the others hear of it is not used. Who:
 * the first to open. A receiver on the internet is late — a web SDR by a
 * second or more — so:
 *   - when the one that had the over closes and another is still open, that
 *     is the same over arriving late there: it is not listened to for as long
 *     as it was late, and a little more. Only if it is still open after that
 *     is it a station the first receiver lost, and it gets the over;
 *   - during an over another receiver takes it when it hears the station
 *     clearly better for a while (the listener then hears a moment twice, or
 *     misses one: the price of a readable signal).
 * A simplex node's other receivers hear its own transmitter, late: they are
 * not listened to during a transmission and for a while after it. */

static const char *src_name(const engine_t *e, int s) { return s == 0 ? "the receiver here" : e->rxl[s - 1].name; }

static bool src_open(const engine_t *e, int s)
{
    return s == 0 ? e->own_open : e->rxl[s - 1].open && !e->rxl[s - 1].ignore;
}

static uint64_t src_opened_at(const engine_t *e, int s) { return s == 0 ? e->own_at : e->rxl[s - 1].opened_at; }

static float src_siglev(const engine_t *e, int s)
{
    return s == 0 ? siglev_now(e) : e->rxl[s - 1].siglev + e->rxl[s - 1].offset;
}

static int n_src(const engine_t *e) { return 1 + e->n_rxl; }

static void vote_begin(engine_t *e, int s)
{
    e->active = s;
    e->switched_at = e->t_ms;
    e->lag_said = false;
    if (s > 0) {
        e->rxl[s - 1].won++;
        e->rxl[s - 1].acc_n = 0;
    }
    rx_over_begin(e);
    if (e->n_rxl > 1 || (e->role->rx && e->n_rxl > 0))
        log_debug("voter: %s has the over (signal %.0f)", src_name(e, s), (double)src_siglev(e, s));
}

/* A simplex node: how long after its own transmission a receiver elsewhere
 * still brings that transmission. Known exactly when its lag was measured. */
static uint64_t tx_blank(const engine_t *e, const struct rxl *l)
{
    return l->lag_set ? (uint64_t)l->lag_ms + (uint64_t)e->cfg->voter.late_ms : (uint64_t)e->cfg->voter.tx_blank_ms;
}

#define LAG_UNKNOWN_MS 2500     /* a receiver whose delay has not been seen yet: assume a web SDR */

static void lag_seen(struct rxl *l, uint64_t ms)
{
    if (l->lag_set)
        return;                                 /* measured: not second-guessed */
    if (ms > 5000) ms = 5000;
    l->lag_ms = l->lag_ms ? (3 * l->lag_ms + (int)ms) / 4 : (int)ms;
}

static void vote_open(engine_t *e, int s)
{
    if (s > 0) {
        struct rxl *l = &e->rxl[s - 1];
        l->wait_until = 0;
        if (e->active >= 0 && e->active != s) {
            if (l->opened_at > src_opened_at(e, e->active))
                lag_seen(l, l->opened_at - src_opened_at(e, e->active));
        } else if (e->active < 0 && e->last_src >= 0 && e->last_src != s &&
                   e->t_ms - e->last_end < (uint64_t)(l->lag_ms ? l->lag_ms : LAG_UNKNOWN_MS) + (uint64_t)e->cfg->voter.late_ms) {
            /* An over has just ended on another receiver, and now this one
             * opens: the same over, arriving late. It lasts as long as that
             * one did; what is still there after that is somebody else. */
            l->wait_until = e->t_ms + (e->last_end - e->last_began) + (uint64_t)e->cfg->voter.late_ms;
            lag_seen(l, e->t_ms - e->last_began);
        }
    }
    if (e->active < 0 && (s == 0 || e->rxl[s - 1].wait_until <= e->t_ms))
        vote_begin(e, s);
}

/* The receiver that had the over closed. */
static void vote_end(engine_t *e)
{
    int was = e->active;
    uint64_t began = src_opened_at(e, was);
    if (was > 0) {
        struct rxl *l = &e->rxl[was - 1];
        if (l->acc_n > 0) {                     /* a partial frame is padded out, not dropped */
            memset(l->acc + l->acc_n, 0, sizeof(int16_t) * (size_t)(e->frame - l->acc_n));
            l->acc_n = 0;
            rx_over_audio(e, l->acc, e->frame, false);
        }
    }
    /* Another receiver that is in step with this one still hears the station:
     * the same over — this receiver lost the station, or its squelch simply
     * closed a moment before the other's. It goes on there; ended and begun
     * again it would be two overs, the second often a few milliseconds long. */
    int lag_was = was > 0 ? e->rxl[was - 1].lag_ms : 0, next = -1;
    for (int s = e->role->rx ? 0 : 1; s < n_src(e); s++) {
        if (s == was || !src_open(e, s) || (s > 0 && e->rxl[s - 1].wait_until > e->t_ms))
            continue;
        if (abs((s > 0 ? e->rxl[s - 1].lag_ms : 0) - lag_was) > e->cfg->voter.switch_lag_ms)
            continue;
        if (next < 0 || src_siglev(e, s) > src_siglev(e, next))
            next = s;
    }
    if (next >= 0) {
        log_debug("voter: %s has closed, %s is still open — the over goes on there", src_name(e, was), src_name(e, next));
        e->active = next;
        e->switched_at = e->t_ms;
        if (next > 0) {
            e->rxl[next - 1].won++;
            e->rxl[next - 1].acc_n = 0;
        }
        return;
    }
    if (was > 0)
        remote_over_done(e, was - 1);
    rx_over_end(e);
    e->active = -1;
    e->last_src = was;
    e->last_began = began;
    e->last_end = e->t_ms;
    for (int i = 0; i < e->n_rxl; i++) {
        struct rxl *l = &e->rxl[i];
        if (i + 1 == was || !l->open || l->ignore)
            continue;
        uint64_t late = l->opened_at > began ? l->opened_at - began : 0;
        if (late > 5000) late = 5000;
        if (l->lag_set)
            late = (uint64_t)l->lag_ms;         /* how far behind it is, exactly */
        /* opened before the one that had the over: it is not behind — go on there */
        l->wait_until = l->opened_at > began ? e->t_ms + late + (uint64_t)e->cfg->voter.late_ms : e->t_ms;
    }
}

static void vote_close(engine_t *e, int s)
{
    if (e->active == s)
        vote_end(e);
}

static void vote_tick(engine_t *e)
{
    const voter_config_t *v = &e->cfg->voter;
    if (e->n_rxl == 0)
        return;
    /* one radio on one frequency: what the others hear now, or soon, is us */
    if (e->role->tx && !e->role->repeater) {
        if (e->tx != TX_OFF) {
            e->tx_seen = true;
            e->tx_seen_at = e->t_ms;
        }
        for (int i = 0; e->tx_seen && i < e->n_rxl; i++) {
            if (e->t_ms - e->tx_seen_at < tx_blank(e, &e->rxl[i])) {
                if (e->rxl[i].open && !e->rxl[i].ignore) {
                    e->rxl[i].ignore = true;
                    if (e->active == i + 1)
                        vote_end(e);
                }
            }
        }
    }
    if (e->active < 0) {
        int best = -1;
        for (int s = e->role->rx ? 0 : 1; s < n_src(e); s++)
            if (src_open(e, s) && (s == 0 || e->rxl[s - 1].wait_until <= e->t_ms) &&
                (best < 0 || src_siglev(e, s) > src_siglev(e, best)))
                best = s;
        if (best >= 0) {
            if (best > 0)
                log_info("voter: %s still hears a station — it gets the over", src_name(e, best));
            vote_begin(e, best);
        }
        return;
    }
    if (!v->switch_over)
        return;
    int best = -1;
    for (int s = e->role->rx ? 0 : 1; s < n_src(e); s++)
        if (s != e->active && src_open(e, s) && src_siglev(e, s) >= src_siglev(e, e->active) + v->margin &&
            (best < 0 || src_siglev(e, s) > src_siglev(e, best)))
            best = s;
    for (int i = 0; i < e->n_rxl; i++)
        if (i + 1 != best)
            e->rxl[i].better_since = 0;
    if (best < 0) {
        e->own_better = 0;
        return;
    }
    uint64_t *since = best == 0 ? &e->own_better : &e->rxl[best - 1].better_since;
    if (best != 0) e->own_better = 0;
    if (*since == 0)
        *since = e->t_ms ? e->t_ms : 1;
    if (e->t_ms - *since < (uint64_t)v->hold_ms || e->t_ms - e->switched_at < 1000)
        return;
    /* A receiver that is behind (a web SDR: a second or more) would say the
     * last words again; one that is ahead would skip as many. Neither takes
     * a running over: the late one is for what the others do not hear. */
    int apart = (best > 0 ? e->rxl[best - 1].lag_ms : 0) - (e->active > 0 ? e->rxl[e->active - 1].lag_ms : 0);
    if (abs(apart) > v->switch_lag_ms) {
        if (!e->lag_said) {
            e->lag_said = true;
            log_info("voter: %s hears it better (signal %.0f, %s %.0f), but is %.1f s %s — the over stays where it is",
                     src_name(e, best), (double)src_siglev(e, best), src_name(e, e->active),
                     (double)src_siglev(e, e->active), abs(apart) / 1000.0, apart > 0 ? "behind" : "ahead");
        }
        return;
    }
    log_info("voter: %s hears it better (signal %.0f, %s %.0f) — it takes the over", src_name(e, best),
             (double)src_siglev(e, best), src_name(e, e->active), (double)src_siglev(e, e->active));
    *since = 0;
    e->active = best;
    e->switched_at = e->t_ms;
    if (best > 0) {
        e->rxl[best - 1].won++;
        e->rxl[best - 1].acc_n = 0;
    }
}

void engine_remote_squelch(engine_t *e, int i, bool open, float siglev)
{
    struct rxl *l = &e->rxl[i];
    if (e->block_rx)
        open = false;
    l->siglev = siglev;
    if (open == l->open)
        return;
    l->open = open;
    if (open) {
        l->opened_at = e->t_ms;
        l->acc_n = 0;
        l->ignore = e->role->tx && !e->role->repeater &&
                    (e->tx != TX_OFF || (e->tx_seen && e->t_ms - e->tx_seen_at < tx_blank(e, l)));
        if (!l->ignore)
            vote_open(e, i + 1);
    } else {
        vote_close(e, i + 1);
        l->ignore = false;
    }
    if (!e->role->rx) {                         /* no receiver here: the squelch shown is theirs */
        e->cos_open = false;
        for (int k = 0; k < e->n_rxl; k++)
            e->cos_open |= e->rxl[k].open;
    }
}

void engine_remote_audio(engine_t *e, int i, const int16_t *pcm, int n)
{
    struct rxl *l = &e->rxl[i];
    if (e->block_rx || e->active != i + 1)
        return;
    if (!e->role->rx)                           /* no receiver here: the one that has the over */
        monitor_feed(e->monitor, MONITOR_RX, pcm, n);
    for (int off = 0; off < n;) {
        int take = e->frame - l->acc_n;
        if (take > n - off) take = n - off;
        memcpy(l->acc + l->acc_n, pcm + off, sizeof(int16_t) * (size_t)take);
        l->acc_n += take;
        off += take;
        if (l->acc_n == e->frame) {
            l->acc_n = 0;
            rx_over_audio(e, l->acc, e->frame, false);
        }
    }
}

void engine_remote_dtmf(engine_t *e, int i, char digit, int duration_ms)
{
    if (e->active == i + 1)
        rx_digit(e, digit, duration_ms);
}

void engine_remote_siglev(engine_t *e, int i, float siglev) { e->rxl[i].siglev = siglev; }

static void sink_report(void *ctx, const gate_report_t *r)
{
    engine_t *e = ctx;
    bool blocked = r->pass == GATE_PASS_NONE;
    bool monitor = r->pass == GATE_PASS_UNGATED && r->analysed;     /* r->would = what "on" does */
    bool would_block = monitor && r->would == GATE_PASS_NONE;
    bool sent = !blocked && e->local_sent > 0;
    bool up = e->role->uplink;              /* a receiver: the destination is its repeater */

    /* An opening that is kept off the talkgroup, or would be, gets the numbers
     * that say why. */
    bool no_voice = r->analysed && (blocked || would_block);
    char detail[200] = "";
    if (no_voice)
        snprintf(detail, sizeof detail, " [%s, voiced %d ms of %d ms loud, level %.0f dBFS, peak %.0f dBFS%s%.0d]",
                 r->looks_like, r->voiced_ms, r->loud_ms, r->level_avg_db, r->level_peak_db,
                 e->rssi_peak >= 0 ? ", rssi " : "", e->rssi_peak >= 0 ? e->rssi_peak : 0);
    else if (r->analysed)
        snprintf(detail, sizeof detail, " [%s, voiced %d ms, level %.0f dBFS%s%.0d]",
                 r->looks_like, r->voiced_ms, r->level_avg_db,
                 e->rssi_peak >= 0 ? ", rssi " : "", e->rssi_peak >= 0 ? e->rssi_peak : 0);
    char where[96] = "";
    if (!blocked && up) {
        if (!sent)
            snprintf(where, sizeof where, " — NOT SENT: no repeater is connected");
    } else if (!blocked) {
        if (sent)
            snprintf(where, sizeof where, " -> TG %u", (unsigned)e->local_tg);
        else if (e->local_tg == 0)
            snprintf(where, sizeof where, " — NOT SENT: no talkgroup selected");
        else
            snprintf(where, sizeof where, " — NOT SENT: the reflector is not connected");
    }
    double secs = r->duration_ms / 1000.0;
    if (blocked && r->dtmf_digits > 0)
        log_info("rx: %.2f s — DTMF only, kept off the %s%s", secs, up ? "repeater" : "talkgroup", detail);
    else if (blocked && r->too_late)
        log_info("rx: %.2f s BLOCKED — no voice within the deadline%s", secs, detail);
    else if (blocked)
        log_info("rx: %.2f s BLOCKED — no voice%s", secs, detail);
    else if (r->pass == GATE_PASS_SPEECH)
        log_info("rx: %.2f s passed — voice after %d ms, stream delay %d ms%s%s",
                 secs, r->speech_at_ms, r->delay_ms, detail, where);
    /* monitor mode: everything passed; say what "on" would have done with it */
    else if (would_block && r->dtmf_digits > 0)
        log_info("rx: %.2f s passed (monitor) — DTMF only: WOULD BE KEPT OFF the %s%s%s", secs,
                 up ? "repeater" : "talkgroup", detail, where);
    else if (would_block)
        log_info("rx: %.2f s passed (monitor) — WOULD BE BLOCKED: no voice%s%s%s", secs,
                 r->too_late ? " within the deadline" : "", detail, where);
    else if (monitor && r->would == GATE_PASS_SPEECH)
        log_info("rx: %.2f s passed (monitor) — voice after %d ms%s%s", secs, r->speech_at_ms, detail, where);
    else if (monitor)
        log_info("rx: %.2f s passed (monitor) — %s%s%s", secs, gate_pass_name(r->would), detail, where);
    else
        log_info("rx: %.2f s passed — %s%s%s", secs, gate_pass_name(r->pass), detail, where);
    /* Somebody keyed up for a second or more and no voice was found: show what
     * the detector made of it, frame by frame (README, "Watching it"). */
    if (no_voice && r->dtmf_digits == 0 && r->trace[0]) {
        if (r->duration_ms >= 1000)
            log_info("rx:   heard, 20 ms per character: %s", r->trace);
        else
            log_debug("rx:   heard, 20 ms per character: %s", r->trace);
    }

    status_opening_t o;
    memset(&o, 0, sizeof o);
    o.when         = util_wall_time();
    o.duration_ms  = r->duration_ms;
    o.analysed     = r->analysed;
    o.speech_at_ms = r->speech_at_ms;
    o.voiced_ms    = r->voiced_ms;
    o.delay_ms     = r->delay_ms;
    o.level_avg_db = r->analysed ? r->level_avg_db : -120.0f;
    o.rssi         = e->rssi_peak;
    o.dtmf_digits  = r->dtmf_digits;
    o.tg           = sent ? e->local_tg : 0;
    o.sent         = sent;
    snprintf(o.pass, sizeof o.pass, "%s", gate_pass_name(r->pass));
    snprintf(o.would, sizeof o.would, "%s", gate_pass_name(r->would));
    snprintf(o.looks_like, sizeof o.looks_like, "%s", r->looks_like);

    status_snapshot_t *d = status_lock(e->status);
    d->openings++;
    if (blocked) {
        d->blocked++;
        if (!strcmp(r->looks_like, "noise"))     d->blocked_noise++;
        else if (!strcmp(r->looks_like, "tone")) d->blocked_tone++;
        else                                     d->blocked_silence++;
    } else {
        d->passed++;
        if (would_block) d->would_block++;
    }
    status_add_opening(d, &o);
    status_unlock(e->status);

    e->rssi_peak = -1;
    /* (Not while another receiver has the over: these count what went to the
     * talkgroup in it, and its end marker depends on them.) */
    if (e->active <= 0)
        e->local_sent = e->local_lost = 0;
}

/* ------------------------------------------------------------------ */
/* the clock                                                           */

/* The next moment the local clock shows a whole multiple of `interval_min`
 * minutes past midnight: 60 = every hour on the hour. */
static time_t next_slot(time_t now, int interval_min)
{
    struct tm tm;
    localtime_r(&now, &tm);
    int mod = tm.tm_hour * 60 + tm.tm_min;
    int next = (mod / interval_min + 1) * interval_min;
    if (next > 1440) next = 1440;               /* midnight starts a new count */
    return now + (time_t)(next - mod) * 60 - tm.tm_sec;
}

static void clock_tick(engine_t *e)
{
    int interval = e->role->tx ? e->cfg->ident.interval_min : 0;
    if (interval > 0) {
        time_t now = util_wall_time();
        if (e->ident_next == 0) {
            e->ident_next = next_slot(now, interval);
        } else if (now >= e->ident_next) {
            /* Minutes late means the clock was set (no RTC: it jumps once NTP
             * answers after a boot) — that is not an identification time. */
            if (now - e->ident_next < 90)
                e->ident_due = true;
            e->ident_next = next_slot(now, interval);
        } else if (e->ident_next - now > (time_t)interval * 60 + 90) {
            e->ident_next = next_slot(now, interval);       /* the clock was set back */
        }
    }

    /* the spoken announcement: every interval, but not when the Morse is due */
    const announce_config_t *an = &e->cfg->announce;
    if (e->role->tx && an->enabled) {
        time_t now = util_wall_time();
        if (e->ann_next == 0) {
            e->ann_next = next_slot(now, an->interval_min);
        } else if (now >= e->ann_next) {
            struct tm tm;
            time_t slot = e->ann_next;
            localtime_r(&slot, &tm);
            int mod = tm.tm_hour * 60 + tm.tm_min;
            if (now - e->ann_next < 90 && (an->with_ident || !(interval > 0 && mod % interval == 0))) {
                e->ann_due = true;
                e->ann_hhmm = tm.tm_hour * 100 + tm.tm_min;
                e->ann_until = e->t_ms + (uint64_t)an->wait_s * 1000;
            }
            e->ann_next = next_slot(now, an->interval_min);
        } else if (e->ann_next - now > (time_t)an->interval_min * 60 + 90) {
            e->ann_next = next_slot(now, an->interval_min);
        }
    }

    /* a talkgroup that was selected for a while falls back to the home one */
    if (e->ref && e->sel_tg != e->home_tg && !e->local_over && !e->net_active && e->t_ms >= e->tg_idle_at)
        select_tg(e, e->home_tg, "no activity, back to the home talkgroup");

    /* an over whose end marker got lost */
    if (e->net_active && e->t_ms - e->net_last >= NET_GAP_MS)
        net_over_end(e);
}

/* A web SDR gives audio and nothing else. Without a carrier an FM receiver
 * hisses, loudly, up to the top of its audio; a carrier quiets it. So: the
 * squelch is open while the hiss above the speech band is low. */
static bool sdr_squelch(engine_t *e, bool audio)
{
    const sdr_config_t *c = &e->cfg->sdr;
    if (!audio) {                               /* the SDR sends nothing */
        e->sdr_run = 0;
        e->sdr_settle = 25;                     /* ...and when it does again, the meter needs half a second */
        e->noise.have = false;
        return e->sdr_open = false;
    }
    if (e->sdr_settle > 0) {
        e->sdr_settle--;
        return false;
    }
    bool want = e->sdr_open ? e->noise.db < c->squelch_close_db : e->noise.db < c->squelch_open_db;
    if (want == e->sdr_open) {
        e->sdr_run = 0;
    } else if (++e->sdr_run * e->frame_ms >= (want ? c->squelch_open_ms : c->squelch_close_ms)) {
        e->sdr_open = want;
        e->sdr_run = 0;
    }
    return e->sdr_open;
}

/* ------------------------------------------------------------------ */
/* The reflector is told what the receivers hear (MsgSignalStrengthValues, as
 * an SvxLink node does) and whether the transmitter is on (MsgTxStatus): it
 * shows in the reflector's status, for whoever draws a dashboard from it.
 * Receivers by their letter in the node information (main.c): the one in this
 * box first, then those elsewhere. A change of squelch or of who is listened
 * to goes out at once, the signal itself twice a second while one is open. */
static void report_tick(engine_t *e)
{
    if (!e->ref || ++e->rep_tick < 5)           /* looked at every 100 ms */
        return;
    e->rep_tick = 0;
    if (!reflector_connected(e->ref)) {
        e->rep_n = e->rep_tx = -1;              /* it forgets us: all of it again after the login */
        return;
    }

    refmsg_rx_t rx[1 + CFG_MAX_RECEIVERS];
    int n = 0;
    bool any = false, flags = false, level = false;
    if (e->role->rx) {
        float v = e->cos_open ? siglev_now(e) : 0.0f;
        rx[n++] = (refmsg_rx_t){ .id = 'A', .siglev = (uint8_t)lrintf(v < 0 ? 0 : v > 100 ? 100 : v),
                                 .enabled = !e->block_rx, .sql_open = e->cos_open,
                                 .active = e->cos_open && e->own_open && e->active == 0 };
    }
    for (int i = 0; i < e->n_rxl; i++) {
        float v = e->rxl[i].open ? e->rxl[i].siglev + e->rxl[i].offset : 0.0f;
        netrx_status_t st;
        netrx_get_status(e->rxl[i].link, &st);
        rx[n] = (refmsg_rx_t){ .id = (char)('A' + n), .siglev = (uint8_t)lrintf(v < 0 ? 0 : v > 100 ? 100 : v),
                               .enabled = st.connected && !e->block_rx, .sql_open = e->rxl[i].open,
                               .active = e->rxl[i].open && !e->rxl[i].ignore && e->active == i + 1 };
        n++;
    }
    for (int i = 0; i < n; i++) {
        any |= rx[i].sql_open;
        if (e->rep_n != n) { flags = true; continue; }
        flags |= rx[i].enabled != e->rep_rx[i].enabled || rx[i].sql_open != e->rep_rx[i].sql_open ||
                 rx[i].active != e->rep_rx[i].active;
        level |= rx[i].siglev != e->rep_rx[i].siglev;
    }
    if (n > 0 && (flags || (level && any && e->t_ms - e->rep_at >= 500)) &&
        reflector_send_rx_status(e->ref, rx, n)) {
        memcpy(e->rep_rx, rx, sizeof(rx[0]) * (size_t)n);
        e->rep_n = n;
        e->rep_at = e->t_ms;
    }

    if (e->role->tx) {
        int on = e->tx != TX_OFF;
        if (on != e->rep_tx && reflector_send_tx_status(e->ref, 'A', on))
            e->rep_tx = on;
    }
}

void engine_on_frame(engine_t *e, const audio_frame_t *f, bool hw_cos)
{
    e->t_ms += (uint64_t)e->frame_ms;

    if (e->role->rx) {
        bool cos = f->sim_cos >= 0 ? f->sim_cos == 1 : hw_cos;
        int16_t pcm[AUDIO_MAX_FRAME];
        bool whole = f->n == e->frame;
        if (whole) {
            memcpy(pcm, f->pcm, sizeof(int16_t) * (size_t)f->n);
            if (e->noise_on)        /* the hiss, as the receiver gives it */
                noisemeter_process(&e->noise, pcm, f->n);
            if (e->role->sdr && f->sim_cos != 1)
                cos = sdr_squelch(e, f->sim_cos < 0);
            /* no decoder in the radio: the tone is looked for in the audio */
            if (e->ctcss_on)
                cos = ctcss_process(&e->ctcss, pcm, e->line_on ? cos : true);
            if (e->deemph_on)
                deemph_process(&e->deemph, pcm, f->n);
            monitor_feed(e->monitor, MONITOR_RX, pcm, f->n);    /* what the receiver gives, squelch or not */
        }
        /* One radio (simplex): while the transmitter is keyed the receiver
         * hears nothing, and it needs a moment to settle after the carrier
         * drops. A repeater's receiver is another radio and listens on. */
        if (e->role->tx && !e->role->repeater) {
            if (f->tx || e->tx != TX_OFF) {
                e->blank_left = e->recover_frames;
                cos = false;
            } else if (e->blank_left > 0) {
                e->blank_left--;
                cos = false;
            }
        }
        if (e->block_rx)
            cos = false;
        if (cos != e->cos_open) {
            e->cos_open = cos;
            if (!cos) e->rssi = -1;
            if (cos && e->ctcss_on)
                log_debug("radio: squelch open — tone at %.0f dBFS, %.0f dB above the noise",
                          (double)e->ctcss.open_level_db, (double)e->ctcss.open_snr_db);
            else
                log_debug("radio: squelch %s", cos ? "open" : "closed");
        }
        /* no RSSI from this receiver: how quiet its noise is says how strong */
        if (e->noise_on && cos && e->noise.have) {
            int v = (int)lrintf(noisemeter_siglev(e->noise.db, e->cfg->siglev.noise_open_db,
                                                  e->cfg->siglev.noise_full_db));
            e->rssi = v;
            if (v > e->rssi_peak) e->rssi_peak = v;
            if (e->uplink && ++e->noise_tick >= 15) {       /* every 300 ms */
                e->noise_tick = 0;
                nettrx_siglev(e->uplink, siglev_now(e));
            }
        }
        if (whole)
            gate_process(&e->gate, pcm, f->n, cos);
    }

    vote_tick(e);
    clock_tick(e);
    if (e->role->tx)
        tx_tick(e);
    report_tick(e);
}

/* ------------------------------------------------------------------ */

bool engine_wants_rssi(const engine_t *e)
{
    return e->role->rx && e->cos_open && (e->tx == TX_OFF || e->role->repeater);
}

void engine_set_monitor(engine_t *e, struct monitor *m) { e->monitor = m; }

int engine_add_rxlink(engine_t *e, netrx_t *link, const char *name, float siglev_offset, int lag_ms)
{
    if (e->n_rxl == CFG_MAX_RECEIVERS)
        return -1;
    struct rxl *l = &e->rxl[e->n_rxl];
    l->link = link;
    l->offset = siglev_offset;
    l->lag_ms = lag_ms > 0 ? lag_ms : 0;
    l->lag_set = lag_ms > 0;
    if (name && name[0])
        snprintf(l->name, sizeof l->name, "%s", name);
    else if (e->n_rxl == 0 && !e->role->rx)
        snprintf(l->name, sizeof l->name, "the receiver");
    else
        snprintf(l->name, sizeof l->name, "receiver %d", e->n_rxl + 1);
    return e->n_rxl++;
}

void engine_set_unkey_hook(engine_t *e, void (*fn)(void *), void *ctx)
{
    e->unkey_hook = fn;
    e->unkey_ctx = ctx;
}
bool engine_transmitting(const engine_t *e) { return e->tx != TX_OFF; }
bool engine_ptt_failed(const engine_t *e) { return e->ptt_failed; }

void engine_on_rssi(engine_t *e, int rssi)
{
    if (!e->cos_open)
        return;
    e->rssi = rssi;
    if (rssi > e->rssi_peak) e->rssi_peak = rssi;
    if (e->uplink)
        nettrx_siglev(e->uplink, siglev_now(e));
}

void engine_push_status(engine_t *e)
{
    reflector_status_t link;
    memset(&link, 0, sizeof link);
    if (e->ref)
        reflector_get_status(e->ref, &link);
    const vad_frame_t *f = gate_last_frame(&e->gate);
    nettrx_status_t up;
    netrx_status_t rxl;
    memset(&up, 0, sizeof up);
    memset(&rxl, 0, sizeof rxl);
    if (e->uplink) nettrx_get_status(e->uplink, &up);
    if (e->n_rxl) netrx_get_status(e->rxl[0].link, &rxl);

    status_snapshot_t *d = status_lock(e->status);
    d->noise_meter  = e->noise_on;
    d->noise_db     = e->noise_on && e->noise.have ? e->noise.db : -120.0f;
    d->tone_detect  = e->ctcss_on;
    if (e->ctcss_on) {
        d->tone_open     = e->ctcss.open;
        d->tone_level_db = e->ctcss.level_db;
        d->tone_snr_db   = e->ctcss.snr_db;
    }
    d->link         = link;
    d->uplink       = up;
    d->rxlink       = rxl;
    d->siglev       = e->role->remote_rx ? e->rxl[e->active > 0 ? e->active - 1 : 0].siglev : siglev_now(e);
    d->n_receivers  = e->n_rxl;
    d->voter_active = e->active;
    for (int i = 0; i < e->n_rxl; i++) {
        netrx_get_status(e->rxl[i].link, &d->receivers[i].link);
        snprintf(d->receivers[i].name, sizeof d->receivers[i].name, "%s", e->rxl[i].name);
        d->receivers[i].siglev  = e->rxl[i].siglev + e->rxl[i].offset;
        d->receivers[i].open    = e->rxl[i].open;
        d->receivers[i].ignored = e->rxl[i].ignore;
        d->receivers[i].won     = e->rxl[i].won;
        d->receivers[i].lag_ms  = e->rxl[i].lag_ms;
        d->receivers[i].lag_set = e->rxl[i].lag_set;
    }
    d->over_open    = e->local_over;
    d->norm_gain_db = e->agc.gain_db;
    d->block_rx = e->block_rx;
    d->block_tx = e->block_tx;
    d->tg           = e->sel_tg;
    snprintf(d->talker, sizeof d->talker, "%s", e->net_active || e->talker[0] ? e->talker : "");
    d->cos_open     = e->cos_open;
    d->gate_open    = e->role->rx ? gate_is_open(&e->gate) : e->local_over;
    d->hold_left_ms = gate_hold_left_ms(&e->gate);
    d->level_db     = e->role->rx && e->cos_open ? f->level_db : -120.0f;
    d->periodicity  = e->role->rx && e->cos_open ? f->periodicity : 0.0f;
    d->voiced       = e->role->rx && e->cos_open && f->voiced;
    d->rssi         = e->cos_open ? e->rssi : -1;
    if (!e->role->rx) {
        /* no receiver here: the signal of the one that has the over (or, before
         * the vote, of the first that hears something), as it reports it */
        int who = e->active > 0 ? e->active - 1 : -1;
        for (int i = 0; who < 0 && i < e->n_rxl; i++)
            if (e->rxl[i].open)
                who = i;
        d->rssi = who >= 0 && e->rxl[who].open ? (int)lrintf(e->rxl[who].siglev + e->rxl[who].offset) : -1;
    }
    snprintf(d->gate_state, sizeof d->gate_state, "%s",
             e->role->rx ? gate_state_name(gate_state(&e->gate)) : e->local_over ? "open" : "idle");
    d->tx           = e->tx != TX_OFF;
    snprintf(d->tx_why, sizeof d->tx_why, "%s", e->tx != TX_OFF && e->tx_why ? e->tx_why : "");
    d->tx_ms        = e->tx != TX_OFF ? (int)(e->t_ms - e->tx_since) : 0;
    d->ident_last   = e->ident_last;
    d->ident_next   = e->ident_next;
    d->overruns     = audio_overruns(e->audio);
    d->underruns    = audio_underruns(e->audio);
    d->dtmf_digits  = e->n_dtmf_digits;
    d->dtmf_commands = e->n_dtmf_commands;
    d->net_overs    = e->n_net_overs;
    d->net_blocked  = e->n_net_blocked;
    d->tx_count     = e->n_tx;
    d->tx_total_ms  = e->tx_total_ms + (e->tx != TX_OFF ? (unsigned long)(e->t_ms - e->tx_since) : 0);
    d->tx_timeouts  = e->n_timeouts;
    d->idents       = e->n_idents;
    status_unlock(e->status);
}

engine_t *engine_new(const config_t *cfg, const role_t *role, audio_t *audio, reflector_t *ref,
                     nettrx_t *uplink, gpio_t *gpio, status_t *status)
{
    engine_t *e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->cfg = cfg;
    e->role = role;
    e->audio = audio;
    e->ref = ref;
    e->uplink = uplink;
    e->gpio = gpio;
    e->status = status;
    e->frame = cfg->audio.period_frames;
    e->frame_ms = e->frame * 1000 / cfg->audio.sample_rate;
    e->rssi = e->rssi_peak = -1;
    e->active = -1;
    e->rep_n = e->rep_tx = -1;
    e->last_src = -1;
    e->preamp  = powf(10.0f, cfg->audio.preamp_db / 20.0f);
    e->line_on = cfg->radio.cos_line >= 0 || role->sdr;   /* a web SDR: the squelch is made here */
    if (role->rx && cfg->ctcss.detect) {
        ctcss_params_t cp;
        ctcss_default_params(&cp);
        cp.sample_rate  = cfg->audio.sample_rate;
        cp.frame        = e->frame;
        cp.tone_hz      = cfg->ctcss.tone_hz;
        cp.window_ms    = cfg->ctcss.window_ms;
        cp.delay_ms     = cfg->ctcss.delay_ms;
        cp.open_snr_db  = cfg->ctcss.open_snr_db;
        cp.close_snr_db = cfg->ctcss.close_snr_db;
        cp.min_level_db = cfg->ctcss.min_level_db;
        cp.open_ms      = cfg->ctcss.open_ms;
        cp.close_ms     = cfg->ctcss.close_ms;
        cp.highpass     = cfg->ctcss.highpass;
        ctcss_init(&e->ctcss, &cp);
        e->ctcss_on = true;
    }
    if (role->rx && cfg->siglev.mode == SIGLEV_NOISE) {
        noisemeter_init(&e->noise, cfg->audio.sample_rate, cfg->siglev.noise_hz);
        e->noise_on = true;
    }
    if (role->rx && cfg->audio.deemphasis) {
        deemph_init(&e->deemph, cfg->audio.sample_rate);
        e->deemph_on = true;
    }
    e->tx_gain = powf(10.0f, cfg->tx.gain_db / 20.0f);
    e->tx_level = powf(10.0f, cfg->tx.level_db / 20.0f);
    if (role->tx && cfg->tag.enabled) {
        /* added after the transmit level and the limiter: the same step down */
        e->tag = tag_new(32767.0f * powf(10.0f, (cfg->tag.level_db + cfg->tx.level_db) / 20.0f));
        if (e->tag) {
            snprintf(e->tag_own, sizeof e->tag_own, "%s",
                     cfg->link.callsign[0] ? cfg->link.callsign : cfg->ident.callsign);
            e->tag_own[strcspn(e->tag_own, "-")] = '\0';
            audio_set_tag(audio, e->tag);           /* the audio keeps and frees it */
            log_info("tag: who is talking goes out under the audio at %.0f dBFS (docs/TAG.md)", (double)cfg->tag.level_db);
        }
    }
    e->duck = 1.0f;
    e->duck_to = powf(10.0f, -cfg->tx.duck_db / 20.0f);
    e->recover_frames = ms_to_frames(e, cfg->tx.rx_recover_ms);
    e->tail_frames = ms_to_frames(e, audio_tx_latency_ms(audio) + cfg->tx.hang_ms);
    e->home_tg = e->sel_tg = e->prev_tg = cfg->link.default_tg;
    /* In Morse a callsign goes without what follows a dash: ON0ABC-S is ON0ABC
     * (the dash names the set, not the station) — as for the stations that
     * come from the talkgroup (call_send). */
    char own[sizeof cfg->ident.callsign];
    snprintf(own, sizeof own, "%s", cfg->ident.callsign);
    own[strcspn(own, "-")] = '\0';
    cw_sanitize(own, e->ident_text, (int)sizeof e->ident_text);

    agc_params_t ap;
    agc_default_params(&ap);
    ap.enabled     = cfg->audio.normalize;
    ap.sample_rate = cfg->audio.sample_rate;
    ap.target_db   = cfg->audio.normalize_target_db;
    ap.max_gain_db = cfg->audio.normalize_max_gain_db;
    agc_init(&e->agc, &ap);
    ap.enabled     = cfg->tx.normalize;
    ap.max_gain_db = cfg->tx.normalize_max_gain_db;
    agc_init(&e->net_agc, &ap);
    txshape_init(&e->shape, cfg->audio.sample_rate, cfg->tx.preemphasis, cfg->tx.limit_db);

    e->netq  = malloc(sizeof(int16_t) * (size_t)NETQ_FRAMES * (size_t)e->frame);
    e->rptq  = malloc(sizeof(int16_t) * (size_t)RPTQ_FRAMES * (size_t)e->frame);
    e->codec = opus_chan_new(cfg->audio.sample_rate, OPUS_BITRATE, 5, 1);
    if (!e->netq || !e->rptq || !e->codec) {
        engine_free(e);
        return NULL;
    }

    gate_params_t gp = cfg->gate;
    gp.sample_rate = cfg->audio.sample_rate;
    gp.frame_ms = e->frame_ms;
    gate_sink_t sink = { e, sink_squelch, sink_audio, sink_dtmf, sink_report };
    gate_init(&e->gate, &gp, &sink);
    return e;
}

void engine_free(engine_t *e)
{
    if (!e)
        return;
    if (e->role->tx) {
        if (e->tx != TX_OFF)
            unkey(e);
        else
            ptt(e, false);      /* belt and braces: never leave with the line asserted */
    }
    if (e->codec) opus_chan_free(e->codec);
    free(e->clip);
    free(e->netq);
    free(e->rptq);
    free(e);
}
