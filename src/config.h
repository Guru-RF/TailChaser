/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — configuration (INI file, /etc/tailchaser/<role>.conf).
 *
 * One format for the four programs. Every program reads every section; what a
 * role does not have (a receiver has no [tx], a simplex node no [uplink]) is
 * simply not used, so a file can be carried from one role to another.
 */
#ifndef SVXSR_CONFIG_H
#define SVXSR_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "gate.h"
#include "role.h"

#define CFG_MAX_ROGER  6
#define CFG_MAX_RADIOS  2       /* sizes the shared gpio/serial code: output line 0 is
                                 * the PTT, line 1 the amplifier (PA) line */
#define CFG_MAX_MONITOR 32

typedef enum { AUDIO_CH_LEFT = 0, AUDIO_CH_RIGHT = 1 } audio_channel_t;
typedef enum { SIGLEV_RSSI = 0, SIGLEV_FIXED = 1, SIGLEV_NOISE = 2 } siglev_mode_t;
typedef enum { RADIO_SA818 = 0, RADIO_NONE = 1 } radio_type_t;

/* [uplink] — receiver: the repeater connects here (SvxLink RemoteTrx protocol) */
typedef struct {
    int  listen_port;       /* TCP port the repeater's Net receiver connects to   */
    char auth_key[128];     /* shared secret; AUTH_KEY on an SvxLink repeater     */
    int  proto_major;       /* RemoteTrx protocol version announced               */
    int  proto_minor;       /*   0 = auto; 8 = SvxLink 25.05, 7 = older           */
    char rx_id;             /* receiver id reported with the squelch ('?' = none) */
} uplink_config_t;

/* [receiver] — transmitter: where its receiver is (a tailchaser-receiver) */
typedef struct {
    char host[128];
    int  port;
    char auth_key[128];     /* the receiver's [uplink] auth_key                   */
    char codec[8];          /* S16 | RAW | OPUS: the audio from the receiver      */
} receiver_config_t;

/* [siglev] — receiver: the signal level reported to the repeater (0..100) */
typedef struct {
    siglev_mode_t mode;
    int   fixed;            /* value reported in fixed mode / before a reading    */
    int   rssi_lo;          /* raw SA818 RSSI mapped to siglev 0                  */
    int   rssi_hi;          /* raw SA818 RSSI mapped to siglev 100                */
    float noise_open_db;    /* noise: the level above the speech band without a signal (0) */
    float noise_full_db;    /* ...and with a full-quieting one (100)              */
} siglev_config_t;

/* [repeater] — duplex, transmitter */
typedef struct {
    bool  roger_beep;       /* a short tone when a station on the input unkeys    */
    int   roger_delay_ms;
    int   roger_tones[CFG_MAX_ROGER];   /* after a station on the input: "blop"          */
    int   roger_n;
    bool  net_call;                     /* Morse before an over from the talkgroup: who  */
    int   net_call_wpm, net_call_pitch_hz, net_call_lead_ms;
    float net_call_level_db;
    char  net_call_prefix[12];          /* "ON0": ON0OST is sent as OST                  */
    char  net_call_other[12];           /* everybody else: "INET"                        */
    bool  roger_net_beep;               /* ...and one after an over from the talkgroup   */
    int   net_tones[CFG_MAX_ROGER];     /* "bleep blop": the pitches, one after the other */
    int   net_n;
    int   roger_ms;         /* each tone */
    int   roger_gap_ms;     /* silence between two tones */
    float roger_level_db;
} repeater_config_t;

/* [reflector] — the SvxReflector this node is a member of */
typedef struct {
    char     domain[128];       /* DNS SRV lookup: _svxreflector._tcp.<domain>      */
    char     host[256];         /* or "host[:port][,host2...]" when there is no SRV */
    int      port;
    char     callsign[32];      /* the node's login on the reflector (ON0ORA-S)     */
    char     auth_key[128];     /* password reflectors only; empty = certificate    */
    char     email[96];         /* contact address in the certificate request      */
    bool     verify_host;       /* the reflector certificate must name the reflector */
    char     pki_dir[256];      /* <CALLSIGN>.key/.csr/.crt + ca-bundle.crt         */
    char     location[64], sysop[32], latitude[24], longitude[24], locator[16];
    char     qth_name[48];
    uint32_t default_tg;        /* the talkgroup the node lives on (0 = none)       */
    uint32_t monitor_tgs[CFG_MAX_MONITOR];
    int8_t   monitor_prio[CFG_MAX_MONITOR];     /* number of '+' after the TG      */
    int      n_monitor;
    int      tg_select_timeout_s;   /* back to default_tg after this much silence   */
    int      udp_heartbeat_s;
} link_config_t;

typedef struct {
    char alsa_device[1024]; /* "plughw:0"; "file:a.wav,pause:2000,b.wav" = bench test */
    audio_channel_t channel;        /* codec input the SA818 audio is on            */
    audio_channel_t tx_channel;     /* codec output that feeds the SA818 microphone */
    int  sample_rate;       /* fixed 16000: the SvxLink network sample rate         */
    int  period_frames;     /* fixed 320 = 20 ms = one Opus frame                   */
    int  buffer_periods;
    int  capture_pct;       /* sound card capture gain, 0..100 (-1 = leave alone)   */
    int  playback_pct;      /* sound card output level, 0..100 (-1 = leave alone)   */
    float preamp_db;        /* software gain on received audio, before the normaliser */
    bool  normalize;        /* bring every received over to the same loudness       */
    float normalize_target_db;      /* speech level aimed for, dBFS RMS             */
    float normalize_max_gain_db;    /* never amplify more than this                 */
    bool  deemphasis;               /* 6 dB/octave down: for discriminator audio    */
    bool  capture, playback;        /* set from the role, not from the file         */
    char tx_record[256];    /* bench test only: write the transmit audio here       */
} audio_config_t;

typedef struct {
    char chip[64];          /* "auto" or /dev/gpiochipN                             */
} gpio_config_t;

typedef struct {
    radio_type_t type;      /* sa818 = programmed by this daemon; none = any radio
                             * on the sound card, squelch and PTT on GPIO lines     */
    char     serial_port[64];
    int      cos_line;      /* SA818 SQ pin; -1 = none: [ctcss] is the squelch      */
    bool     cos_active_low;
    int      ptt_line;      /* SA818 PTT pin                                        */
    bool     ptt_active_low;
    bool     freq_explicit;
    uint32_t freq_rx_hz, freq_tx_hz;    /* the same on a simplex node               */
    int      bandwidth;     /* 0 = narrow 12.5 kHz, 1 = wide 25 kHz                 */
    int      ctcss_rx;      /* SA818 table index, 0 = none: tone needed to open the squelch */
    int      ctcss_tx;      /* SA818 table index, 0 = none: tone sent with every transmission */
    int      squelch;       /* 0..8                                                 */
    int      volume;        /* 1..8                                                 */
    int      filter_emphasis, filter_highpass, filter_lowpass;  /* 0 = enabled      */
    int      pa_line;       /* a second output keyed with the PTT: the amplifier
                             * (-1 = none)                                          */
    bool     pa_active_low;
    int      pa_delay_ms;   /* 0 = the amplifier just before the PTT; more = this
                             * long after it: exciter first, then the amplifier     */
    bool     refresh_after_tx;      /* send the chip its filter settings again after
                                     * every transmission                           */
} radio_config_t;

/* [announce] — a spoken announcement on the clock. The daemon only plays
 * <dir>/HHMM.wav; tools/tailchaser-announce makes those files (the text, the
 * voice service and its token are its settings, in the same section). */
typedef struct {
    bool  enabled;
    char  dir[256];
    int   interval_min;     /* 15 = at :15, :30, :45 — not when the Morse is due...  */
    bool  with_ident;       /* ...unless this is set: then right after the Morse    */
    float level_db;         /* the speech is brought to this level (dBFS RMS)       */
    bool  duck;             /* false: wait for a quiet moment; true: start anyway,
                             * quieter, under whoever is talking                    */
    int   wait_s;           /* ...and give up after this long                       */
    int   max_s;            /* never longer than this                               */
} announce_config_t;

/* [location] — what the reflector's dashboards and maps show of the station.
 * Frequencies and tones are taken from [radio] and [ctcss] where they are
 * known; here is what the program cannot know, or what it knows wrong. */
typedef struct {
    double freq_rx_mhz, freq_tx_mhz;    /* 0 = from [radio]                         */
    float  ctcss_rx_hz, ctcss_tx_hz;    /* 0 = from [radio] / [ctcss]; -1 = none    */
    float  power_w;                     /* transmitter output; 0 = not said         */
    char   antenna[64];                 /* "X300 7dBi"                              */
    int    antenna_height_m;            /* above ground; 0 = not said               */
    int    antenna_dir;                 /* degrees; -1 = omnidirectional            */
    bool   hidden;                      /* ask the dashboards not to list the node  */
    bool   tone_to_tg;                  /* tell which tone opens the home talkgroup */
} location_config_t;

/* [ctcss] — a tone squelch in software, for a receiver without one */
typedef struct {
    bool  detect;           /* off: the squelch line alone decides                  */
    float tone_hz;
    int   window_ms, delay_ms;
    float open_snr_db, close_snr_db, min_level_db;
    int   open_ms, close_ms;
    bool  highpass;         /* filter the tone out of the audio                     */
} ctcss_config_t;

/* [tx] — keying the transmitter */
typedef struct {
    int   delay_ms;         /* carrier this long before the audio starts            */
    int   hang_ms;          /* carrier this long after the audio ends               */
    int   timeout_s;        /* longest single transmission (0 = no limit)           */
    int   rx_recover_ms;    /* squelch ignored this long after unkeying             */
    float gain_db;          /* software gain, reflector -> radio                    */
    bool  normalize;        /* talkgroup audio is brought to [audio]
                             * normalize_target_db like a station on the input:
                             * everything that is transmitted has one level         */
    float normalize_max_gain_db;
    float level_db;         /* software gain on everything that is transmitted      */
    float duck_db;          /* Morse and announcements are this much quieter while
                             * somebody is talking                                  */
    bool  preemphasis;      /* 6 dB/octave in software: for a radio whose own
                             * pre-emphasis is bypassed                             */
    float limit_db;         /* nothing louder than this reaches the radio (dBFS)   */
    int   test_s;           /* SIGUSR1: a test transmission this long             */
    float test_level_db;    /* ...its tones at this peak level (dBFS)             */
} tx_config_t;

/* [ident] — the callsign in Morse, on the clock */
typedef struct {
    char  callsign[32];
    int   interval_min;     /* 60 = every hour on the hour; 0 = off                 */
    int   wpm;
    int   pitch_hz;
    float level_db;         /* tone peak, dBFS                                      */
} ident_config_t;

/* [dtmf] — what is not part of the gate */
typedef struct {
    bool commands;          /* <tg_prefix><TG># selects a talkgroup                 */
    char tg_prefix[8];      /* "91", as on an SvxLink node                          */
    bool ack;               /* answer a talkgroup command in Morse                  */
} dtmf_config_t;

typedef struct {
    int  http_port;         /* status page; 0 = off                                 */
    char bind[48];
    int  rssi_lo;           /* S-meter on the page: raw SA818 RSSI shown as S1      */
    int  rssi_hi;           /*                      ... and as S9+40                */
} status_config_t;

typedef struct {
    link_config_t   link;
    uplink_config_t   uplink;
    receiver_config_t receiver;
    siglev_config_t   siglev;
    repeater_config_t repeater;
    audio_config_t  audio;
    gpio_config_t   gpio;
    radio_config_t  radio;
    announce_config_t announce;
    location_config_t location;
    ctcss_config_t ctcss;
    tx_config_t     tx;
    ident_config_t  ident;
    gate_params_t   gate;   /* [gate] + the gate's share of [dtmf] */
    dtmf_config_t   dtmf;
    status_config_t status;
    bool            status_rssi_set;    /* [status] rssi_lo/hi came from the file */
    char            log_level[16];
    char            path[256];
} config_t;

/* The defaults of a role: a talkgroup node and a repeater receiver want
 * different things from the same gate (gate.h). */
void config_defaults(config_t *cfg, const role_t *role);
int  config_load(config_t *cfg, const char *path);

/* What follows from the role and the file together (a receiver's transmit
 * frequency is its receive frequency, ...). Call after config_load(). */
void config_finish(config_t *cfg, const role_t *role);

/* Range/consistency checks for this role; logs every problem. Returns 0 if
 * usable. */
int  config_validate(const config_t *cfg, const role_t *role);

/* Priority of a talkgroup: the number of '+' it has in monitor_tgs (0 if it is
 * not monitored). */
int  config_tg_prio(const config_t *cfg, uint32_t tg);
bool config_tg_monitored(const config_t *cfg, uint32_t tg);

#endif /* SVXSR_CONFIG_H */
