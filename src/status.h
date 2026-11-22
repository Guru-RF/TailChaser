/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — read-only status page.
 *
 * A tiny HTTP server on its own thread: GET / is a self-refreshing page,
 * GET /status.json the same data for scripts. The main loop pushes state into
 * a snapshot under a mutex; the HTTP thread only ever reads the snapshot, so
 * a slow browser can never stall the audio path. Nothing can be changed from
 * here — configuration lives in the config file.
 */
#ifndef SVXSR_STATUS_H
#define SVXSR_STATUS_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "netrx.h"
#include "nettrx.h"
#include "reflector.h"

#define STATUS_HISTORY     60
#define STATUS_NET_HISTORY 30

/* one squelch opening on the radio */
typedef struct {
    time_t when;            /* wall clock at the end of the opening */
    int    duration_ms;
    char   pass[20];        /* gate_pass_name()                     */
    char   would[20];       /* ...of what mode on makes of it: "blocked" in
                             * monitor mode = it would have been      */
    char   looks_like[12];
    bool   analysed;
    int    speech_at_ms;
    int    voiced_ms;
    int    delay_ms;
    float  level_avg_db;
    int    rssi;            /* strongest raw RSSI seen, -1 unknown  */
    int    dtmf_digits;
    uint32_t tg;            /* talkgroup it was sent to, 0 = not sent */
    bool   sent;            /* it reached where it was going (talkgroup / repeater) */
} status_opening_t;

/* one over that came in from the reflector */
typedef struct {
    time_t   when;
    int      duration_ms;
    char     call[32];
    uint32_t tg;
    char     outcome[20];   /* "transmitted" | "channel busy" | "partly" | "timed out" */
} status_netover_t;

typedef struct {
    /* fixed at start */
    char   role[16];                /* simplex | duplex | receiver | transmitter */
    char   title[40];
    bool   has_rx, has_tx, has_reflector, has_uplink, has_rxlink, repeater;
    char   radio_type[8];
    int    listen_port;
    bool   normalize;
    char   version[32];
    bool   block_rx, block_tx;      /* set from the admin portal or the API */
    char   board[96];
    char   firmware[48];
    double freq_rx_mhz, freq_tx_mhz;
    char   bandwidth[8];
    char   ctcss_rx[12], ctcss_tx[12];
    int    squelch;
    bool   monitor;                     /* [status] monitor */
    bool   sdr_local;                   /* ...one on the USB port here */
    bool   has_sdr;                     /* the receiver is a web SDR */
    char   sdr_type[16], sdr_url[256], sdr_state[160], sdr_server[96];
    bool   sdr_connected, sdr_listening;
    float  sdr_smeter_db;
    int    sdr_buffer_ms, sdr_rate;
    unsigned long sdr_connects, sdr_underruns, sdr_dropped_ms;
    bool   noise_meter;                 /* [siglev] mode = noise */
    float  noise_db;
    bool   tone_detect, tone_open;      /* [ctcss]: the software tone squelch */
    float  tone_hz, tone_level_db, tone_snr_db;
    int    rssi_lo, rssi_hi;        /* S-meter scale: raw RSSI at S1 and at S9+60 */
    char   mode[10];
    int    hold_s;
    char   callsign[32];            /* the node's login on the reflector */
    char   ident_callsign[32];
    int    ident_interval_min;
    uint32_t home_tg;
    int    tx_timeout_s;
    time_t started;

    /* live */
    reflector_status_t link;
    nettrx_status_t uplink;         /* receiver: the repeater that is connected  */
    netrx_status_t  rxlink;         /* transmitter: the link to its receiver     */
    struct {                        /* every receiver elsewhere, and the vote     */
        netrx_status_t link;
        char  name[24];
        float siglev;
        bool  open, ignored, lag_set;
        int   lag_ms;
        unsigned long won;
    } receivers[4];
    int    n_receivers, voter_active;  /* -1 nobody, 0 the receiver here, 1+i    */
    float  siglev;                  /* 0..100                                    */
    bool   over_open;               /* an over is coming from the receiver       */
    float  norm_gain_db;            /* what the normaliser is doing              */
    uint32_t tg;                    /* selected talkgroup               */
    char   talker[32];              /* who is talking on it right now   */
    bool   cos_open;                /* radio squelch                    */
    bool   gate_open;               /* an over is being sent to the reflector */
    char   gate_state[12];
    int    hold_left_ms;
    float  level_db;
    float  periodicity;
    bool   voiced;
    int    rssi;
    bool   tx;                      /* PTT keyed                        */
    char   tx_why[24];              /* "talkgroup audio" | "identification" | "DTMF answer" */
    int    tx_ms;                   /* keyed for this long              */
    time_t ident_last, ident_next;
    unsigned long overruns, underruns;

    /* totals */
    unsigned long openings, passed, blocked;
    unsigned long blocked_noise, blocked_silence, blocked_tone;
    unsigned long would_block;      /* monitor mode */
    unsigned long dtmf_digits, dtmf_commands;
    unsigned long net_overs, net_blocked;   /* blocked = not transmitted: channel in use */
    unsigned long tx_count, tx_total_ms, tx_timeouts, idents;

    status_opening_t history[STATUS_HISTORY];
    int    hist_head, hist_len;     /* ring: newest at head-1 */
    status_netover_t net_history[STATUS_NET_HISTORY];
    int    net_head, net_len;
} status_snapshot_t;

typedef struct status status_t;

/* port 0 = no HTTP server (the snapshot still works). */
status_t *status_new(const char *bind_addr, int port);
/* The admin portal and the API are served by the same server. */
struct admin;
void status_set_admin(status_t *s, struct admin *a);
/* Listening in: /monitor/rx and /monitor/tx are served from this. */
struct monitor;
void status_set_monitor(status_t *s, struct monitor *m);
/* A transmit site: the status of its receive site (a tailchaser-receiver's
 * /status.json) is fetched and passed on as "remote_receiver", so whoever
 * asks only has to ask here. port 0 = not. */
void status_set_remote(status_t *s, const char *host, int port);
void      status_free(status_t *s);

/* Edit the snapshot: lock, change, unlock. */
status_snapshot_t *status_lock(status_t *s);
void               status_unlock(status_t *s);
void               status_add_opening(status_snapshot_t *snap, const status_opening_t *o);
void               status_add_netover(status_snapshot_t *snap, const status_netover_t *o);

#endif /* SVXSR_STATUS_H */
