/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "config.h"
#include "cw.h"
#include "log.h"
#include "sa818.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void config_defaults(config_t *cfg, const role_t *role)
{
    memset(cfg, 0, sizeof *cfg);

    cfg->link.port                = 5300;
    cfg->link.tg_select_timeout_s = 30;
    cfg->link.udp_heartbeat_s     = 10;
    snprintf(cfg->link.pki_dir, sizeof cfg->link.pki_dir, "/var/lib/tailchaser/pki");
    cfg->link.verify_host = true;
    snprintf(cfg->link.qth_name, sizeof cfg->link.qth_name, "TailChaser");

    snprintf(cfg->audio.alsa_device, sizeof cfg->audio.alsa_device, "plughw:0");
    cfg->audio.channel        = AUDIO_CH_LEFT;
    cfg->audio.tx_channel     = AUDIO_CH_LEFT;
    cfg->audio.sample_rate    = 16000;
    cfg->audio.period_frames  = 320;
    cfg->audio.buffer_periods = 4;
    cfg->audio.capture_pct    = 50;
    cfg->audio.playback_pct   = 77;
    cfg->audio.preamp_db      = 0.0f;
    cfg->audio.normalize             = true;
    cfg->audio.normalize_target_db   = -20.0f;
    cfg->audio.normalize_max_gain_db = 30.0f;
    cfg->tx.normalize                = true;
    cfg->tx.normalize_max_gain_db    = 15.0f;
    cfg->audio.capture  = true;
    cfg->audio.playback = true;

    snprintf(cfg->gpio.chip, sizeof cfg->gpio.chip, "auto");

    snprintf(cfg->radio.serial_port, sizeof cfg->radio.serial_port, "/dev/serial0");
    cfg->radio.cos_line       = 12;
    cfg->radio.cos_active_low = true;
    cfg->radio.ptt_line       = 16;
    cfg->radio.ptt_active_low = true;
    cfg->radio.bandwidth      = 0;
    cfg->radio.squelch        = 1;
    cfg->radio.volume         = 1;
    cfg->radio.pa_line        = -1;
    cfg->radio.pa_active_low  = true;

    cfg->tx.delay_ms      = 900;
    cfg->tx.hang_ms       = 0;
    cfg->tx.timeout_s     = 360;
    cfg->tx.rx_recover_ms = 300;
    cfg->tx.gain_db       = 0.0f;
    cfg->tx.limit_db      = 0.0f;        /* off */
    cfg->tx.duck_db = 6.0f;
    snprintf(cfg->announce.dir, sizeof cfg->announce.dir, "/var/lib/tailchaser/announce");
    cfg->announce.interval_min = 15;
    cfg->announce.level_db     = -16.0f;
    cfg->announce.wait_s       = 120;
    cfg->announce.max_s        = 30;
    cfg->location.antenna_dir = -1;
    cfg->location.tone_to_tg  = true;
    cfg->ctcss.detect       = false;
    cfg->ctcss.tone_hz      = 0.0f;
    cfg->ctcss.window_ms    = 300;
    cfg->ctcss.delay_ms     = 300;
    cfg->ctcss.open_snr_db  = 15.0f;
    cfg->ctcss.close_snr_db = 8.0f;
    cfg->ctcss.min_level_db = -50.0f;
    cfg->ctcss.open_ms      = 60;
    cfg->ctcss.close_ms     = 160;
    cfg->ctcss.highpass     = true;
    cfg->tx.test_s        = 10;
    cfg->tx.test_level_db = -20.0f;

    cfg->ident.interval_min = 60;
    cfg->ident.wpm          = 24;
    cfg->ident.pitch_hz     = 600;
    cfg->ident.level_db     = -12.0f;

    gate_default_params(&cfg->gate);

    cfg->dtmf.commands = true;
    cfg->dtmf.ack      = true;
    snprintf(cfg->dtmf.tg_prefix, sizeof cfg->dtmf.tg_prefix, "91");

    cfg->status.http_port = 8080;
    snprintf(cfg->status.bind, sizeof cfg->status.bind, "0.0.0.0");
    cfg->status.rssi_lo   = 79;
    cfg->status.rssi_hi   = 100;

    cfg->uplink.listen_port = 5210;
    cfg->uplink.proto_major = 2;
    cfg->uplink.proto_minor = 0;        /* auto */
    cfg->uplink.rx_id       = '?';

    cfg->receiver.port = 5210;
    snprintf(cfg->receiver.codec, sizeof cfg->receiver.codec, "S16");

    cfg->siglev.mode    = SIGLEV_RSSI;
    cfg->siglev.fixed   = 100;
    cfg->siglev.rssi_lo = 79;
    cfg->siglev.rssi_hi = 100;
    cfg->siglev.noise_open_db = -45.0f;
    cfg->siglev.noise_full_db = -60.0f;

    cfg->repeater.roger_beep     = true;
    cfg->repeater.roger_delay_ms = 200;
    cfg->repeater.roger_tones[0] = 800;         /* blop */
    cfg->repeater.roger_n        = 1;
    cfg->repeater.roger_net_beep = true;
    cfg->repeater.net_call          = true;
    cfg->repeater.net_call_wpm      = 27;
    cfg->repeater.net_call_pitch_hz = 800;
    cfg->repeater.net_call_lead_ms  = 600;
    cfg->repeater.net_call_level_db = -14.0f;
    snprintf(cfg->repeater.net_call_prefix, sizeof cfg->repeater.net_call_prefix, "ON0");
    cfg->repeater.net_call_other[0] = '\0';     /* the roger beep already says "network" */
    cfg->repeater.net_tones[0]   = 1200;        /* bleep ... */
    cfg->repeater.net_tones[1]   = 800;         /* ... blop  */
    cfg->repeater.net_n          = 2;
    cfg->repeater.roger_ms       = 70;
    cfg->repeater.roger_gap_ms   = 20;
    cfg->repeater.roger_level_db = -18.0f;

    /* ---- what differs by role ---- */
    if (role->repeater) {
        /* The audio waits in a queue while the carrier comes up, so nothing is
         * lost to the delay; and a repeater keeps its carrier between overs. */
        cfg->tx.delay_ms = 300;
        cfg->tx.hang_ms  = 3000;
    }
    if (role->uplink) {
        /* A repeater receiver (gate.h): the talker hears the repeater, so the
         * delayed end of an over is blanked; DTMF is a command for the repeater
         * and has to get there; and keying a repeater on a short noise is the
         * worse mistake. */
        cfg->gate.tail_silence      = true;
        cfg->gate.dtmf_opens        = true;
        cfg->gate.vad.min_voiced_ms = 140;
    }

    snprintf(cfg->log_level, sizeof cfg->log_level, "info");
}

int config_tg_prio(const config_t *cfg, uint32_t tg)
{
    for (int i = 0; i < cfg->link.n_monitor; i++)
        if (cfg->link.monitor_tgs[i] == tg)
            return cfg->link.monitor_prio[i];
    return 0;
}

bool config_tg_monitored(const config_t *cfg, uint32_t tg)
{
    for (int i = 0; i < cfg->link.n_monitor; i++)
        if (cfg->link.monitor_tgs[i] == tg)
            return true;
    return false;
}

/* ---- parser helpers ---- */

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

/* A '#' or ';' starts a comment at the start of the value or after a space —
 * but not inside a quoted value: location = "Hill #2" keeps its '#'. */
static void strip_inline_comment(char *v)
{
    char *p = v;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '"' || *p == '\'') {
        char *close = strchr(p + 1, *p);
        if (close)
            v = close + 1;
    }
    for (p = v; *p; p++) {
        if ((*p == '#' || *p == ';') && (p == v || isspace((unsigned char)p[-1]))) {
            *p = '\0';
            break;
        }
    }
}

static void unquote(char *v)
{
    size_t n = strlen(v);
    if (n >= 2 && ((v[0] == '"' && v[n - 1] == '"') || (v[0] == '\'' && v[n - 1] == '\''))) {
        memmove(v, v + 1, n - 2);
        v[n - 2] = '\0';
    }
}

static bool parse_bool(const char *v, bool *out)
{
    if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1")) {
        *out = true;
        return true;
    }
    if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcmp(v, "0")) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_int(const char *v, int *out)
{
    char *end;
    errno = 0;
    long l = strtol(v, &end, 10);
    if (errno || end == v || *trim(end) != '\0')
        return false;
    *out = (int)l;
    return true;
}

static bool parse_float(const char *v, float *out)
{
    char *end;
    errno = 0;
    double d = strtod(v, &end);
    if (errno || end == v || *trim(end) != '\0')
        return false;
    *out = (float)d;
    return true;
}

static bool parse_tg(const char *v, uint32_t *out)
{
    char *end;
    errno = 0;
    unsigned long l = strtoul(v, &end, 10);
    if (errno || end == v || *trim(end) != '\0' || !isdigit((unsigned char)v[0]))
        return false;
    *out = (uint32_t)l;
    return true;
}

static bool parse_mhz(const char *v, uint32_t *out)
{
    float f;
    if (!parse_float(v, &f) || f <= 0)
        return false;
    *out = (uint32_t)llround(strtod(v, NULL) * 1e6);
    return true;
}

static bool parse_ctcss(const char *v, int *out)
{
    if (!strcasecmp(v, "none") || !strcasecmp(v, "off") || v[0] == '\0') {
        *out = 0;
        return true;
    }
    int idx = sa818_ctcss_parse(v);
    if (idx < 0)
        return false;
    *out = idx;
    return true;
}

static bool parse_channel(const char *v, audio_channel_t *out)
{
    if (!strcasecmp(v, "left") || !strcasecmp(v, "mono") || !strcmp(v, "0")) *out = AUDIO_CH_LEFT;
    else if (!strcasecmp(v, "right") || !strcmp(v, "1"))                     *out = AUDIO_CH_RIGHT;
    else return false;
    return true;
}

/* "1745++, 8+, 23": SvxLink notation, the '+' are the priority. */
static bool parse_monitor(link_config_t *l, const char *s)
{
    l->n_monitor = 0;
    while (*s) {
        while (*s && (isspace((unsigned char)*s) || *s == ',')) s++;
        if (!*s) break;
        if (!isdigit((unsigned char)*s))
            return false;
        char *end;
        unsigned long tg = strtoul(s, &end, 10);
        s = end;
        int prio = 0;
        while (*s == '+') { prio++; s++; }
        if (tg == 0 || l->n_monitor >= CFG_MAX_MONITOR)
            return false;
        l->monitor_tgs[l->n_monitor]  = (uint32_t)tg;
        l->monitor_prio[l->n_monitor] = (int8_t)(prio > 100 ? 100 : prio);
        l->n_monitor++;
    }
    return true;
}

#define SET_STR(dst, v) snprintf((dst), sizeof(dst), "%s", (v))

/* Callsigns are upper case on the air and on the reflector. */
static void upcase(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

/* Returns 1 handled, 0 unknown key, -1 bad value. */
static int set_key(config_t *cfg, const char *sec, const char *key, char *v)
{
    int  i;
    bool b;
    float f;
#define INT(field)   do { if (!parse_int(v, &i)) return -1; (field) = i; return 1; } while (0)
#define BOOL(field)  do { if (!parse_bool(v, &b)) return -1; (field) = b; return 1; } while (0)
#define FLT(field)   do { if (!parse_float(v, &f)) return -1; (field) = f; return 1; } while (0)
#define STR(field)   do { SET_STR(field, v); return 1; } while (0)

    if (!strcmp(sec, "reflector")) {
        link_config_t *l = &cfg->link;
        if (!strcmp(key, "domain"))      STR(l->domain);
        if (!strcmp(key, "host") || !strcmp(key, "hosts")) STR(l->host);
        if (!strcmp(key, "port"))        INT(l->port);
        if (!strcmp(key, "callsign"))    { SET_STR(l->callsign, v); upcase(l->callsign); return 1; }
        if (!strcmp(key, "auth_key"))    STR(l->auth_key);
        if (!strcmp(key, "email"))       STR(l->email);
        if (!strcmp(key, "verify_host")) BOOL(l->verify_host);
        if (!strcmp(key, "pki_dir"))     STR(l->pki_dir);
        if (!strcmp(key, "location"))    STR(l->location);
        if (!strcmp(key, "sysop"))       STR(l->sysop);
        if (!strcmp(key, "latitude"))    STR(l->latitude);
        if (!strcmp(key, "longitude"))   STR(l->longitude);
        if (!strcmp(key, "locator"))     STR(l->locator);
        if (!strcmp(key, "qth_name"))    STR(l->qth_name);
        if (!strcmp(key, "default_tg"))  return parse_tg(v, &l->default_tg) ? 1 : -1;
        if (!strcmp(key, "monitor_tgs")) return parse_monitor(l, v) ? 1 : -1;
        if (!strcmp(key, "tg_select_timeout_s")) INT(l->tg_select_timeout_s);
        if (!strcmp(key, "udp_heartbeat_s"))     INT(l->udp_heartbeat_s);
    } else if (!strcmp(sec, "uplink")) {
        uplink_config_t *u = &cfg->uplink;
        if (!strcmp(key, "listen_port")) INT(u->listen_port);
        if (!strcmp(key, "auth_key"))    STR(u->auth_key);
        if (!strcmp(key, "proto_major")) INT(u->proto_major);
        if (!strcmp(key, "proto_minor")) {
            if (!strcasecmp(v, "auto")) { u->proto_minor = 0; return 1; }
            INT(u->proto_minor);
        }
        if (!strcmp(key, "rx_id")) { u->rx_id = v[0] ? v[0] : '?'; return 1; }
    } else if (!strcmp(sec, "receiver")) {
        receiver_config_t *rc = &cfg->receiver;
        if (!strcmp(key, "host")) {
            /* host or host:port (not an IPv6 literal with colons) */
            char *colon = strrchr(v, ':');
            if (colon && strchr(v, ':') == colon) {
                if (!parse_int(colon + 1, &i)) return -1;
                rc->port = i;
                *colon = '\0';
            }
            STR(rc->host);
        }
        if (!strcmp(key, "port"))     INT(rc->port);
        if (!strcmp(key, "auth_key")) STR(rc->auth_key);
        if (!strcmp(key, "codec"))    { SET_STR(rc->codec, v); upcase(rc->codec); return 1; }
    } else if (!strcmp(sec, "siglev")) {
        siglev_config_t *sl = &cfg->siglev;
        if (!strcmp(key, "mode")) {
            if (!strcasecmp(v, "rssi"))       sl->mode = SIGLEV_RSSI;
            else if (!strcasecmp(v, "fixed")) sl->mode = SIGLEV_FIXED;
            else if (!strcasecmp(v, "noise")) sl->mode = SIGLEV_NOISE;
            else return -1;
            return 1;
        }
        if (!strcmp(key, "fixed"))   INT(sl->fixed);
        if (!strcmp(key, "rssi_lo")) INT(sl->rssi_lo);
        if (!strcmp(key, "rssi_hi")) INT(sl->rssi_hi);
        if (!strcmp(key, "noise_open_db")) FLT(sl->noise_open_db);
        if (!strcmp(key, "noise_full_db")) FLT(sl->noise_full_db);
    } else if (!strcmp(sec, "repeater")) {
        repeater_config_t *rp = &cfg->repeater;
        if (!strcmp(key, "roger_beep"))     BOOL(rp->roger_beep);
        if (!strcmp(key, "roger_delay_ms")) INT(rp->roger_delay_ms);
        bool net = !strcmp(key, "roger_net_tones");
        if (net || !strcmp(key, "roger_tones") || !strcmp(key, "roger_pitch_hz")) {
            /* one pitch or several: "1200, 800" */
            char tmp[128];
            snprintf(tmp, sizeof tmp, "%s", v);
            int n = 0, *dst = net ? rp->net_tones : rp->roger_tones;
            char *save = NULL;
            for (char *t = strtok_r(tmp, ", \t", &save); t; t = strtok_r(NULL, ", \t", &save)) {
                if (n >= CFG_MAX_ROGER || !parse_int(t, &i)) return -1;
                dst[n++] = i;
            }
            if (n == 0) return -1;
            if (net) rp->net_n = n; else rp->roger_n = n;
            return 1;
        }
        if (!strcmp(key, "roger_net_beep")) BOOL(rp->roger_net_beep);
        if (!strcmp(key, "net_call"))          BOOL(rp->net_call);
        if (!strcmp(key, "net_call_wpm"))      INT(rp->net_call_wpm);
        if (!strcmp(key, "net_call_pitch_hz")) INT(rp->net_call_pitch_hz);
        if (!strcmp(key, "net_call_lead_ms"))  INT(rp->net_call_lead_ms);
        if (!strcmp(key, "net_call_level_db")) FLT(rp->net_call_level_db);
        if (!strcmp(key, "net_call_prefix"))   STR(rp->net_call_prefix);
        if (!strcmp(key, "net_call_other"))    STR(rp->net_call_other);
        if (!strcmp(key, "roger_ms"))       INT(rp->roger_ms);
        if (!strcmp(key, "roger_gap_ms"))   INT(rp->roger_gap_ms);
        if (!strcmp(key, "roger_level_db")) FLT(rp->roger_level_db);
    } else if (!strcmp(sec, "audio")) {
        audio_config_t *a = &cfg->audio;
        if (!strcmp(key, "normalize"))             BOOL(a->normalize);
        if (!strcmp(key, "normalize_target_db"))   FLT(a->normalize_target_db);
        if (!strcmp(key, "normalize_max_gain_db")) FLT(a->normalize_max_gain_db);
        if (!strcmp(key, "deemphasis"))   BOOL(a->deemphasis);
        if (!strcmp(key, "alsa_device"))   STR(a->alsa_device);
        if (!strcmp(key, "audio_channel")) {
            if (!parse_channel(v, &a->channel)) return -1;
            a->tx_channel = a->channel;         /* tx_channel after it overrides */
            return 1;
        }
        if (!strcmp(key, "tx_channel"))    return parse_channel(v, &a->tx_channel) ? 1 : -1;
        if (!strcmp(key, "buffer_periods")) INT(a->buffer_periods);
        if (!strcmp(key, "capture_pct")) {
            if (!strcasecmp(v, "keep")) { a->capture_pct = -1; return 1; }
            INT(a->capture_pct);
        }
        if (!strcmp(key, "playback_pct")) {
            if (!strcasecmp(v, "keep")) { a->playback_pct = -1; return 1; }
            INT(a->playback_pct);
        }
        if (!strcmp(key, "preamp_db")) FLT(a->preamp_db);
        if (!strcmp(key, "tx_record")) STR(a->tx_record);
    } else if (!strcmp(sec, "gpio")) {
        if (!strcmp(key, "chip")) STR(cfg->gpio.chip);
    } else if (!strcmp(sec, "radio")) {
        radio_config_t *r = &cfg->radio;
        if (!strcmp(key, "type")) {
            if (!strcasecmp(v, "sa818"))     r->type = RADIO_SA818;
            else if (!strcasecmp(v, "none")) r->type = RADIO_NONE;
            else return -1;
            return 1;
        }
        if (!strcmp(key, "pa_line")) {
            if (!strcasecmp(v, "none")) { r->pa_line = -1; return 1; }
            INT(r->pa_line);
        }
        if (!strcmp(key, "pa_active_low"))    BOOL(r->pa_active_low);
        if (!strcmp(key, "pa_delay_ms"))      INT(r->pa_delay_ms);
        if (!strcmp(key, "refresh_after_tx")) BOOL(r->refresh_after_tx);
        if (!strcmp(key, "serial_port"))    STR(r->serial_port);
        if (!strcmp(key, "cos_line")) {
            if (!strcasecmp(v, "none")) { r->cos_line = -1; return 1; }
            INT(r->cos_line);
        }
        if (!strcmp(key, "cos_active_low")) BOOL(r->cos_active_low);
        if (!strcmp(key, "ptt_line")) {
            if (!strcasecmp(v, "none")) { r->ptt_line = -1; return 1; }
            INT(r->ptt_line);
        }
        if (!strcmp(key, "ptt_active_low")) BOOL(r->ptt_active_low);
        if (!strcmp(key, "freq") || !strcmp(key, "frequency")) {
            if (!parse_mhz(v, &r->freq_rx_hz)) return -1;
            r->freq_tx_hz = r->freq_rx_hz;
            r->freq_explicit = true;
            return 1;
        }
        if (!strcmp(key, "freq_rx")) { r->freq_explicit = true; return parse_mhz(v, &r->freq_rx_hz) ? 1 : -1; }
        if (!strcmp(key, "freq_tx")) { r->freq_explicit = true; return parse_mhz(v, &r->freq_tx_hz) ? 1 : -1; }
        if (!strcmp(key, "bandwidth")) {
            if (!strcasecmp(v, "narrow"))    r->bandwidth = 0;
            else if (!strcasecmp(v, "wide")) r->bandwidth = 1;
            else if (parse_int(v, &i))       r->bandwidth = i;
            else return -1;
            return 1;
        }
        if (!strcmp(key, "ctcss")) {
            if (!parse_ctcss(v, &r->ctcss_rx)) return -1;
            r->ctcss_tx = r->ctcss_rx;
            return 1;
        }
        if (!strcmp(key, "ctcss_rx"))        return parse_ctcss(v, &r->ctcss_rx) ? 1 : -1;
        if (!strcmp(key, "ctcss_tx"))        return parse_ctcss(v, &r->ctcss_tx) ? 1 : -1;
        if (!strcmp(key, "squelch"))         INT(r->squelch);
        if (!strcmp(key, "volume"))          INT(r->volume);
        if (!strcmp(key, "filter_emphasis")) INT(r->filter_emphasis);
        if (!strcmp(key, "filter_highpass")) INT(r->filter_highpass);
        if (!strcmp(key, "filter_lowpass"))  INT(r->filter_lowpass);
    } else if (!strcmp(sec, "announce")) {
        announce_config_t *an = &cfg->announce;
        if (!strcmp(key, "enabled"))      BOOL(an->enabled);
        if (!strcmp(key, "dir"))          STR(an->dir);
        if (!strcmp(key, "interval_min")) INT(an->interval_min);
        if (!strcmp(key, "with_ident"))   BOOL(an->with_ident);
        if (!strcmp(key, "level_db"))     FLT(an->level_db);
        if (!strcmp(key, "wait_s"))       INT(an->wait_s);
        if (!strcmp(key, "max_s"))        INT(an->max_s);
        if (!strcmp(key, "busy")) {
            if (!strcasecmp(v, "wait"))      an->duck = false;
            else if (!strcasecmp(v, "duck")) an->duck = true;
            else return -1;
            return 1;
        }
        return 1;       /* everything else here is for tailchaser-announce */
    } else if (!strcmp(sec, "location")) {
        location_config_t *lo = &cfg->location;
        link_config_t *l = &cfg->link;
        /* the place itself: the same keys as in [reflector] */
        if (!strcmp(key, "name") || !strcmp(key, "location")) STR(l->location);
        if (!strcmp(key, "sysop"))       STR(l->sysop);
        if (!strcmp(key, "latitude"))    STR(l->latitude);
        if (!strcmp(key, "longitude"))   STR(l->longitude);
        if (!strcmp(key, "locator"))     STR(l->locator);
        if (!strcmp(key, "qth_name"))    STR(l->qth_name);
        if (!strcmp(key, "freq_rx")) { if (!parse_float(v, &f)) return -1; lo->freq_rx_mhz = (double)f; return 1; }
        if (!strcmp(key, "freq_tx")) { if (!parse_float(v, &f)) return -1; lo->freq_tx_mhz = (double)f; return 1; }
        if (!strcmp(key, "ctcss_rx") || !strcmp(key, "ctcss_tx")) {
            float *dst = key[6] == 'r' ? &lo->ctcss_rx_hz : &lo->ctcss_tx_hz;
            if (!strcasecmp(v, "none")) { *dst = -1.0f; return 1; }
            if (!parse_float(v, &f) || f < 60.0f || f > 260.0f) return -1;
            *dst = f;
            return 1;
        }
        if (!strcmp(key, "power_w"))          FLT(lo->power_w);
        if (!strcmp(key, "antenna"))          STR(lo->antenna);
        if (!strcmp(key, "antenna_height_m")) INT(lo->antenna_height_m);
        if (!strcmp(key, "antenna_dir")) {
            if (!strcasecmp(v, "omni")) { lo->antenna_dir = -1; return 1; }
            INT(lo->antenna_dir);
        }
        if (!strcmp(key, "hidden"))           BOOL(lo->hidden);
        if (!strcmp(key, "tone_to_talkgroup")) BOOL(lo->tone_to_tg);
    } else if (!strcmp(sec, "ctcss")) {
        ctcss_config_t *c = &cfg->ctcss;
        if (!strcmp(key, "detect"))       BOOL(c->detect);
        if (!strcmp(key, "tone"))         FLT(c->tone_hz);
        if (!strcmp(key, "window_ms"))    INT(c->window_ms);
        if (!strcmp(key, "delay_ms"))     INT(c->delay_ms);
        if (!strcmp(key, "open_snr_db"))  FLT(c->open_snr_db);
        if (!strcmp(key, "close_snr_db")) FLT(c->close_snr_db);
        if (!strcmp(key, "min_level_db")) FLT(c->min_level_db);
        if (!strcmp(key, "open_ms"))      INT(c->open_ms);
        if (!strcmp(key, "close_ms"))     INT(c->close_ms);
        if (!strcmp(key, "highpass"))     BOOL(c->highpass);
    } else if (!strcmp(sec, "tx")) {
        if (!strcmp(key, "delay_ms"))      INT(cfg->tx.delay_ms);
        if (!strcmp(key, "hang_ms"))       INT(cfg->tx.hang_ms);
        if (!strcmp(key, "timeout_s"))     INT(cfg->tx.timeout_s);
        if (!strcmp(key, "rx_recover_ms")) INT(cfg->tx.rx_recover_ms);
        if (!strcmp(key, "gain_db"))       FLT(cfg->tx.gain_db);
        if (!strcmp(key, "normalize"))     BOOL(cfg->tx.normalize);
        if (!strcmp(key, "normalize_max_gain_db")) FLT(cfg->tx.normalize_max_gain_db);
        if (!strcmp(key, "level_db"))      FLT(cfg->tx.level_db);
        if (!strcmp(key, "duck_db"))       FLT(cfg->tx.duck_db);
        if (!strcmp(key, "preemphasis"))   BOOL(cfg->tx.preemphasis);
        if (!strcmp(key, "limit_db"))      FLT(cfg->tx.limit_db);
        if (!strcmp(key, "test_s"))        INT(cfg->tx.test_s);
        if (!strcmp(key, "test_level_db")) FLT(cfg->tx.test_level_db);
    } else if (!strcmp(sec, "ident")) {
        if (!strcmp(key, "callsign"))     { SET_STR(cfg->ident.callsign, v); upcase(cfg->ident.callsign); return 1; }
        if (!strcmp(key, "interval_min")) INT(cfg->ident.interval_min);
        if (!strcmp(key, "wpm"))          INT(cfg->ident.wpm);
        if (!strcmp(key, "pitch_hz"))     INT(cfg->ident.pitch_hz);
        if (!strcmp(key, "level_db"))     FLT(cfg->ident.level_db);
    } else if (!strcmp(sec, "gate")) {
        gate_params_t *g = &cfg->gate;
        if (!strcmp(key, "mode")) {
            if (!strcasecmp(v, "on") || !strcasecmp(v, "gate")) g->mode = GATE_MODE_ON;
            else if (!strcasecmp(v, "monitor"))                 g->mode = GATE_MODE_MONITOR;
            else if (!strcasecmp(v, "off"))                     g->mode = GATE_MODE_OFF;
            else return -1;
            return 1;
        }
        if (!strcmp(key, "lookback_ms"))        INT(g->lookback_ms);
        if (!strcmp(key, "hold_s"))             INT(g->hold_s);
        if (!strcmp(key, "min_open_ms"))        INT(g->min_open_ms);
        if (!strcmp(key, "voice_deadline_ms"))  INT(g->voice_deadline_ms);
        if (!strcmp(key, "carrier_timeout_ms")) INT(g->carrier_timeout_ms);
        if (!strcmp(key, "bridge_ms"))          INT(g->bridge_ms);
        if (!strcmp(key, "tail_trim_ms"))       INT(g->tail_trim_ms);
        if (!strcmp(key, "tail_silence"))       BOOL(g->tail_silence);
        if (!strcmp(key, "min_level_db"))       FLT(g->vad.min_level_db);
        if (!strcmp(key, "voiced_threshold"))   FLT(g->vad.voiced_thr);
        if (!strcmp(key, "tone_purity"))        FLT(g->vad.tone_purity);
        if (!strcmp(key, "min_voiced_ms"))      INT(g->vad.min_voiced_ms);
        if (!strcmp(key, "window_ms"))          INT(g->vad.window_ms);
    } else if (!strcmp(sec, "dtmf")) {
        if (!strcmp(key, "enabled"))    BOOL(cfg->gate.dtmf_enabled);
        if (!strcmp(key, "opens_gate")) BOOL(cfg->gate.dtmf_opens);
        if (!strcmp(key, "muting"))     BOOL(cfg->gate.dtmf_muting);
        if (!strcmp(key, "commands"))   BOOL(cfg->dtmf.commands);
        if (!strcmp(key, "tg_prefix"))  STR(cfg->dtmf.tg_prefix);
        if (!strcmp(key, "ack"))        BOOL(cfg->dtmf.ack);
    } else if (!strcmp(sec, "status")) {
        if (!strcmp(key, "http_port")) INT(cfg->status.http_port);
        if (!strcmp(key, "bind"))      STR(cfg->status.bind);
        if (!strcmp(key, "rssi_lo"))   { cfg->status_rssi_set = true; INT(cfg->status.rssi_lo); }
        if (!strcmp(key, "rssi_hi"))   { cfg->status_rssi_set = true; INT(cfg->status.rssi_hi); }
    } else if (!strcmp(sec, "log")) {
        if (!strcmp(key, "level")) STR(cfg->log_level);
    }
    return 0;
#undef INT
#undef BOOL
#undef FLT
#undef STR
}

int config_load(config_t *cfg, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        log_error("config: cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    snprintf(cfg->path, sizeof cfg->path, "%s", path);
    char line[2048], sec[32] = "";
    int lineno = 0, rc = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *s = trim(line);
        if (*s == '\0' || *s == '#' || *s == ';')
            continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (!e) {
                log_error("config: %s:%d: unterminated section header", path, lineno);
                rc = -1;
                continue;
            }
            *e = '\0';
            snprintf(sec, sizeof sec, "%s", trim(s + 1));
            for (char *p = sec; *p; p++) *p = (char)tolower((unsigned char)*p);
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) {
            log_error("config: %s:%d: expected key = value", path, lineno);
            rc = -1;
            continue;
        }
        *eq = '\0';
        char *key = trim(s);
        char *val = eq + 1;
        strip_inline_comment(val);
        val = trim(val);
        unquote(val);
        for (char *p = key; *p; p++) *p = (char)tolower((unsigned char)*p);

        int r = set_key(cfg, sec, key, val);
        if (r == 0)
            log_warn("config: %s:%d: unknown setting [%s] %s (ignored)", path, lineno, sec, key);
        else if (r < 0) {
            /* never echo a secret into the journal */
            bool secret = !strcmp(key, "auth_key");
            log_error("config: %s:%d: bad value for [%s] %s: '%s'", path, lineno, sec, key,
                      secret ? "..." : val);
            rc = -1;
        }
    }
    fclose(f);
    return rc;
}

static bool in_band(uint32_t hz, bool *vhf)
{
    if (hz >= 134000000u && hz <= 174000000u) { *vhf = true; return true; }
    if (hz >= 400000000u && hz <= 480000000u) { *vhf = false; return true; }
    return false;
}

void config_finish(config_t *cfg, const role_t *role)
{
    radio_config_t *r = &cfg->radio;
    cfg->audio.capture  = role->rx;
    cfg->audio.playback = role->tx;
    if (!role->tx) {                    /* a receiver: the chip is given one frequency */
        r->freq_tx_hz = r->freq_rx_hz;
        r->ctcss_tx   = r->ctcss_rx;
    } else if (!role->rx) {             /* a transmitter */
        if (r->freq_tx_hz == 0) r->freq_tx_hz = r->freq_rx_hz;
        r->freq_rx_hz = r->freq_tx_hz;
    } else {
        if (r->freq_tx_hz == 0) r->freq_tx_hz = r->freq_rx_hz;
        if (r->freq_rx_hz == 0) r->freq_rx_hz = r->freq_tx_hz;
    }
    /* A receiver's S-meter uses the scale its signal level is reported on,
     * unless the page was given one of its own. */
    /* A signal level from the noise is 0..100 already. */
    if (role->rx && cfg->siglev.mode == SIGLEV_NOISE) {
        cfg->siglev.rssi_lo = 0;
        cfg->siglev.rssi_hi = 100;
        if (!cfg->status_rssi_set) {
            cfg->status.rssi_lo = 0;
            cfg->status.rssi_hi = 100;
        }
    }
    if (role->uplink && !cfg->status_rssi_set) {
        cfg->status.rssi_lo = cfg->siglev.rssi_lo;
        cfg->status.rssi_hi = cfg->siglev.rssi_hi;
    }
}

int config_validate(const config_t *cfg, const role_t *role)
{
    int bad = 0;
#define FAIL(...) do { log_error(__VA_ARGS__); bad = 1; } while (0)

    if (role->reflector) {
        const link_config_t *l = &cfg->link;
        if (l->domain[0] == '\0' && l->host[0] == '\0')
            FAIL("config: [reflector] set domain (DNS SRV lookup) or host");
        if (l->callsign[0] == '\0')
            FAIL("config: [reflector] callsign is required — the node's login on the reflector");
        if (l->port < 1 || l->port > 65535)
            FAIL("config: [reflector] port %d out of range", l->port);
        if (l->pki_dir[0] == '\0')
            FAIL("config: [reflector] pki_dir must not be empty");
        if (l->tg_select_timeout_s < 1)
            FAIL("config: [reflector] tg_select_timeout_s must be at least 1");
        if (l->udp_heartbeat_s < 1 || l->udp_heartbeat_s > 60)
            FAIL("config: [reflector] udp_heartbeat_s must be 1..60");
        if (l->default_tg == 0 && l->n_monitor == 0)
            log_warn("config: [reflector] no default_tg and no monitor_tgs — the node hears and "
                     "reaches nobody until a talkgroup is selected with DTMF");
    }

    if (role->uplink) {
        const uplink_config_t *u = &cfg->uplink;
        if (u->listen_port < 1 || u->listen_port > 65535)
            FAIL("config: [uplink] listen_port %d out of range", u->listen_port);
        if (u->proto_minor != 0 && u->proto_minor != 7 && u->proto_minor != 8)
            FAIL("config: [uplink] proto_minor must be auto, 7 or 8");
        if (u->auth_key[0] == '\0')
            log_warn("config: [uplink] auth_key is empty — anybody who reaches port %d gets the "
                     "receiver", u->listen_port);
        const siglev_config_t *sl = &cfg->siglev;
        if (sl->rssi_lo < 0 || sl->rssi_hi > 255 || sl->rssi_hi <= sl->rssi_lo)
            FAIL("config: [siglev] rssi_lo and rssi_hi must be 0..255, rssi_hi the greater");
        if (sl->fixed < 0 || sl->fixed > 100)
            FAIL("config: [siglev] fixed must be 0..100");
    }

    if (role->remote_rx) {
        const receiver_config_t *rc = &cfg->receiver;
        if (rc->host[0] == '\0')
            FAIL("config: [receiver] host is required — the address of the tailchaser-receiver");
        if (rc->port < 1 || rc->port > 65535)
            FAIL("config: [receiver] port %d out of range", rc->port);
        if (strcmp(rc->codec, "S16") && strcmp(rc->codec, "RAW") && strcmp(rc->codec, "OPUS"))
            FAIL("config: [receiver] codec must be S16, RAW or OPUS");
    }

    const radio_config_t *r = &cfg->radio;
    if (r->type == RADIO_SA818) {
        bool vhf_rx = false, vhf_tx = false;
        if (!r->freq_explicit)
            FAIL("config: [radio] %s is required — there is no default frequency",
                 role->repeater && role->rx ? "freq_rx and freq_tx" : role->tx && role->rx ? "freq" : role->tx ? "freq_tx" : "freq_rx");
        else if (!in_band(r->freq_rx_hz, &vhf_rx) || !in_band(r->freq_tx_hz, &vhf_tx))
            FAIL("config: [radio] frequency %.4f / %.4f MHz is outside the SA818 bands "
                 "(134-174 / 400-480)", r->freq_rx_hz / 1e6, r->freq_tx_hz / 1e6);
        else if (vhf_rx != vhf_tx)
            FAIL("config: [radio] freq_rx and freq_tx must be in the same band");
        if (r->bandwidth != 0 && r->bandwidth != 1)
            FAIL("config: [radio] bandwidth must be 0/narrow or 1/wide");
        if (r->squelch < 0 || r->squelch > 8)
            FAIL("config: [radio] squelch must be 0..8");
        if (r->volume < 1 || r->volume > 8)
            FAIL("config: [radio] volume must be 1..8");
        if (r->ctcss_rx < 0 || r->ctcss_rx >= SA818_CTCSS_COUNT ||
            r->ctcss_tx < 0 || r->ctcss_tx >= SA818_CTCSS_COUNT)
            FAIL("config: [radio] ctcss_rx / ctcss_tx is not a valid tone");
    }
    if (role->rx && cfg->siglev.mode == SIGLEV_NOISE) {
        if (cfg->siglev.noise_full_db >= cfg->siglev.noise_open_db - 3.0f ||
            cfg->siglev.noise_open_db > 0.0f || cfg->siglev.noise_full_db < -100.0f)
            FAIL("config: [siglev] noise_full_db must be at least 3 dB below noise_open_db (dBFS)");
        if (cfg->audio.sample_rate < 16000)
            FAIL("config: [siglev] mode = noise needs a sample rate of 16000");
    }
    const ctcss_config_t *ct = &cfg->ctcss;
    if (role->rx && r->cos_line < 0 && !ct->detect)
        FAIL("config: [radio] cos_line is required (the squelch line of the receiver) — "
             "or [ctcss] detect = true, for a receiver that only gives audio");
    if (role->rx && ct->detect) {
        if (ct->tone_hz < 60.0f || ct->tone_hz > 260.0f)
            FAIL("config: [ctcss] tone is required: the tone in Hz, 60..260 (e.g. 79.7)");
        if (ct->window_ms < 100 || ct->window_ms > 1000)
            FAIL("config: [ctcss] window_ms must be 100..1000");
        if (ct->delay_ms < 0 || ct->delay_ms > 1000)
            FAIL("config: [ctcss] delay_ms must be 0..1000");
        if (ct->close_snr_db > ct->open_snr_db || ct->close_snr_db < 0.0f || ct->open_snr_db > 40.0f)
            FAIL("config: [ctcss] needs 0 <= close_snr_db <= open_snr_db <= 40");
        if (ct->min_level_db < -80.0f || ct->min_level_db > 0.0f)
            FAIL("config: [ctcss] min_level_db must be -80..0 (dBFS)");
        if (ct->open_ms < 0 || ct->open_ms > 2000 || ct->close_ms < 0 || ct->close_ms > 2000)
            FAIL("config: [ctcss] open_ms / close_ms must be 0..2000");
    }
    if (role->tx && r->ptt_line < 0)
        FAIL("config: [radio] ptt_line is required (the PTT line of the transmitter)");
    if (r->pa_delay_ms < 0 || r->pa_delay_ms > 5000)
        FAIL("config: [radio] pa_delay_ms must be 0..5000");
    if (r->pa_line >= 0 && r->pa_delay_ms > cfg->tx.delay_ms)
        log_warn("config: [radio] pa_delay_ms (%d) is longer than [tx] delay_ms (%d) — audio starts "
                 "before the amplifier is on", r->pa_delay_ms, cfg->tx.delay_ms);
    if (r->pa_line >= 0 && (r->pa_line == r->ptt_line || r->pa_line == r->cos_line))
        FAIL("config: [radio] pa_line must be a line of its own");

    if (role->tx) {
        const tx_config_t *t = &cfg->tx;
        if (t->delay_ms < 0 || t->delay_ms > 5000)
            FAIL("config: [tx] delay_ms must be 0..5000");
        if (t->hang_ms < 0 || t->hang_ms > 30000)
            FAIL("config: [tx] hang_ms must be 0..30000");
        if (t->timeout_s < 0)
            FAIL("config: [tx] timeout_s must not be negative (0 = no limit)");
        if (t->rx_recover_ms < 0 || t->rx_recover_ms > 5000)
            FAIL("config: [tx] rx_recover_ms must be 0..5000");
        if (t->gain_db < -40.0f || t->gain_db > 20.0f)
            FAIL("config: [tx] gain_db must be -40..20");
        if (t->duck_db < 0.0f || t->duck_db > 40.0f)
            FAIL("config: [tx] duck_db must be 0..40");
        const announce_config_t *an = &cfg->announce;
        if (an->enabled) {
            if (an->interval_min < 1 || an->interval_min > 720)
                FAIL("config: [announce] interval_min must be 1..720");
            if (an->level_db < -40.0f || an->level_db > -3.0f)
                FAIL("config: [announce] level_db must be -40..-3 (dBFS)");
            if (an->wait_s < 0 || an->wait_s > 600 || an->max_s < 1 || an->max_s > 120)
                FAIL("config: [announce] wait_s must be 0..600 and max_s 1..120");
        }
        if (t->level_db < -40.0f || t->level_db > 20.0f)
            FAIL("config: [tx] level_db must be -40..20");
        if (t->test_s < 2 || t->test_s > 60)
            FAIL("config: [tx] test_s must be 2..60");
        if (t->test_level_db > 0.0f || t->test_level_db < -40.0f)
            FAIL("config: [tx] test_level_db must be -40..0 (dBFS)");
        if (t->limit_db > 0.0f || t->limit_db < -30.0f)
            FAIL("config: [tx] limit_db must be -30..0 (dBFS)");

        const ident_config_t *id = &cfg->ident;
        if (id->interval_min < 0 || id->interval_min > 1440)
            FAIL("config: [ident] interval_min must be 0..1440 (0 = off)");
        if (id->interval_min > 0) {
            char cs[32];
            if (cw_sanitize(id->callsign, cs, sizeof cs) == 0)
                FAIL("config: [ident] callsign is required (or set interval_min = 0)");
            if (id->wpm < 5 || id->wpm > 60)
                FAIL("config: [ident] wpm must be 5..60");
            if (id->pitch_hz < 300 || id->pitch_hz > 3000)
                FAIL("config: [ident] pitch_hz must be 300..3000");
            if (id->level_db > 0.0f || id->level_db < -60.0f)
                FAIL("config: [ident] level_db must be -60..0 (dBFS)");
        }
    }
    if (role->repeater) {
        const repeater_config_t *rp = &cfg->repeater;
        if (rp->roger_delay_ms < 0 || rp->roger_delay_ms > 5000)
            FAIL("config: [repeater] roger_delay_ms must be 0..5000");
        if (rp->roger_ms < 20 || rp->roger_ms > 2000)
            FAIL("config: [repeater] roger_ms must be 20..2000");
        for (int k = 0; k < rp->roger_n; k++)
            if (rp->roger_tones[k] < 300 || rp->roger_tones[k] > 3000)
                FAIL("config: [repeater] roger_tones must be 300..3000 (Hz) each");
        for (int k = 0; k < rp->net_n; k++)
            if (rp->net_tones[k] < 300 || rp->net_tones[k] > 3000)
                FAIL("config: [repeater] roger_net_tones must be 300..3000 (Hz) each");
        if (rp->net_call_wpm < 10 || rp->net_call_wpm > 40 ||
            rp->net_call_pitch_hz < 300 || rp->net_call_pitch_hz > 3000)
            FAIL("config: [repeater] net_call_wpm must be 10..40 and net_call_pitch_hz 300..3000");
        if (rp->roger_gap_ms < 0 || rp->roger_gap_ms > 1000)
            FAIL("config: [repeater] roger_gap_ms must be 0..1000");
        if (rp->roger_level_db > 0.0f || rp->roger_level_db < -60.0f)
            FAIL("config: [repeater] roger_level_db must be -60..0 (dBFS)");
    }

    if (cfg->audio.buffer_periods < 2 || cfg->audio.buffer_periods > 16)
        FAIL("config: [audio] buffer_periods must be 2..16");
    if (cfg->audio.capture_pct > 100)
        FAIL("config: [audio] capture_pct must be 0..100 (or keep)");
    if (cfg->audio.playback_pct > 100)
        FAIL("config: [audio] playback_pct must be 0..100 (or keep)");
    if (cfg->audio.normalize_target_db > -3.0f || cfg->audio.normalize_target_db < -40.0f)
        FAIL("config: [audio] normalize_target_db must be -40..-3 (dBFS)");
    if (cfg->tx.normalize_max_gain_db < 0.0f || cfg->tx.normalize_max_gain_db > 40.0f)
        FAIL("config: [tx] normalize_max_gain_db must be 0..40");
    if (cfg->audio.normalize_max_gain_db < 0.0f || cfg->audio.normalize_max_gain_db > 40.0f)
        FAIL("config: [audio] normalize_max_gain_db must be 0..40");

    if (role->rx) {
        const gate_params_t *g = &cfg->gate;
        if (g->vad.voiced_thr <= 0.0f || g->vad.voiced_thr >= 1.0f)
            FAIL("config: [gate] voiced_threshold must be between 0 and 1");
        if (g->vad.min_voiced_ms < 20 || g->vad.min_voiced_ms > g->vad.window_ms)
            FAIL("config: [gate] min_voiced_ms must be 20..window_ms");
        if (g->vad.window_ms < 100 || g->vad.window_ms > 5000)
            FAIL("config: [gate] window_ms must be 100..5000");
        if (g->lookback_ms < 0 || g->hold_s < 0 || g->carrier_timeout_ms < 0 ||
            g->bridge_ms < 0 || g->tail_trim_ms < 0)
            FAIL("config: [gate] times must not be negative");
    }

    if (role->reflector && cfg->dtmf.commands) {
        const char *p = cfg->dtmf.tg_prefix;
        if (!*p)
            FAIL("config: [dtmf] tg_prefix must not be empty (or set commands = false)");
        for (; *p; p++)
            if (!strchr("0123456789ABCD*", *p))
                FAIL("config: [dtmf] tg_prefix may only hold DTMF digits");
    }
    if (cfg->status.http_port < 0 || cfg->status.http_port > 65535)
        FAIL("config: [status] http_port out of range");
    if (cfg->status.rssi_lo < 0 || cfg->status.rssi_hi > 255 ||
        cfg->status.rssi_hi <= cfg->status.rssi_lo)
        FAIL("config: [status] rssi_lo and rssi_hi must be 0..255, rssi_hi the greater");
#undef FAIL
    return bad ? -1 : 0;
}
