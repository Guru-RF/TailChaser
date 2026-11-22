/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — the program behind tailchaser-simplex, -duplex, -receiver and
 * -transmitter (role.h). One daemon instead of SvxLink's logic cores, remotetrx
 * and their scripts:
 *
 *   radio ──AF──► sound card ──► gate ──► normaliser ──┬─► Opus ──► reflector (TLS + UDP)
 *     │  └─SQ──► GPIO ───────────┘                     ├─► RemoteTrx ──► the repeater   (receiver)
 *     │                                                └─► the transmitter              (repeater)
 *     └◄─AF──── sound card ◄── queue ◄── Opus ◄── reflector
 *      ◄─PTT─── GPIO ◄── engine (TX delay, hang time, time-out, Morse identification)
 *
 * The radio's squelch says "carrier"; the gate (gate.c / vad.c) only lets an
 * over through once there is a voice on it. Which of the boxes above exist is
 * the role.
 */
#include "audio.h"
#include "config.h"
#include "detect.h"
#include "engine.h"
#include "gate.h"
#include "gpio.h"
#include "log.h"
#include "mixer.h"
#include "netrx.h"
#include "sdr.h"
#include "monitor.h"
#include "nettrx.h"
#include "reflector.h"
#include "role.h"
#include "sa818.h"
#include "serial_worker.h"
#include "status.h"
#include "admin.h"
#include "util.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef TAILCHASER_VERSION
#define TAILCHASER_VERSION "dev"
#endif

#define CONFIG_DIR     "/etc/tailchaser"
#define EXIT_CONFIG    78       /* sysexits EX_CONFIG: the unit does not restart on it */
#define RSSI_POLL_MS   300      /* while the radio squelch is open */
#define STATUS_PUSH_MS 200
#define WATCHDOG_MS    5000
#define AUDIO_STALL_MS 5000     /* this long without a frame: the sound card hangs */

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }
static volatile sig_atomic_t g_test;
static void on_test(int sig) { (void)sig; g_test = 1; }
static volatile sig_atomic_t g_say, g_call;
static void on_call(int sig) { (void)sig; g_call = 1; }
static void on_say(int sig) { (void)sig; g_say = 1; }

/* systemd's notify protocol, without libsystemd: "READY=1" once the node is
 * up, "WATCHDOG=1" from the main loop. If the loop ever hangs, systemd kills
 * the daemon — and with it the PTT line is released. */
static void sd_notify_msg(const char *msg)
{
    const char *path = getenv("NOTIFY_SOCKET");
    if (!path || !*path)
        return;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    size_t len = strlen(path);
    if (len >= sizeof sa.sun_path)
        return;
    memcpy(sa.sun_path, path, len);
    if (sa.sun_path[0] == '@')
        sa.sun_path[0] = '\0';          /* abstract socket */
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    if (sendto(fd, msg, strlen(msg), MSG_NOSIGNAL, (struct sockaddr *)&sa,
               (socklen_t)(offsetof(struct sockaddr_un, sun_path) + len)) < 0) { /* not fatal */ }
    close(fd);
}

static void scopy(char *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *tone_name(int idx) { return idx ? sa818_ctcss_str(idx) : "none"; }

/* Program the SA818's one channel. Returns the open device, or NULL. */
static sa818_t *radio_open(const radio_config_t *r, const char *port, char *firmware, size_t cap)
{
    sa818_t *dev = sa818_open(port, SA818_DEFAULT_BAUD);
    if (!dev) {
        log_error("radio: cannot open the SA818 on %s", port);
        return NULL;
    }
    if (sa818_set_group(dev, r->bandwidth, r->freq_tx_hz, r->freq_rx_hz,
                        r->ctcss_tx, r->ctcss_rx, r->squelch) != 0) {
        log_error("radio: SA818 programming failed (%.4f MHz)", r->freq_rx_hz / 1e6);
        sa818_close(dev);
        return NULL;
    }
    sa818_set_filter(dev, r->filter_emphasis, r->filter_highpass, r->filter_lowpass);
    sa818_set_volume(dev, r->volume);
    scopy(firmware, cap, sa818_firmware(dev));
    return dev;
}

static gpio_t *open_gpio(const config_t *cfg)
{
    const char *chip = cfg->gpio.chip;
    char chipbuf[64];
    if (chip[0] == '\0' || !strcmp(chip, "auto")) {
        if (gpio_detect_header_chip(chipbuf, sizeof chipbuf) == 0) {
            chip = chipbuf;
            log_info("gpio: auto-detected header chip %s", chip);
        } else {
            chip = "/dev/gpiochip0";
            log_warn("gpio: header auto-detect failed; falling back to %s", chip);
        }
    }
    return gpio_open(chip);
}

/* -u: the daemon is gone — crashed, killed by the watchdog, or stopped. Whether
 * the kernel leaves an output line as it was when its owner dies depends on
 * the kernel; a transmitter must not depend on that. Take the line, drive it
 * inactive, let go. */
static int release_ptt(const config_t *cfg)
{
    if (!strncmp(cfg->audio.alsa_device, "file:", 5)) {
        log_info("gpio: audio is a file — there is no PTT line to release");
        return 0;
    }
    if (cfg->radio.ptt_line < 0) {
        log_error("gpio: [radio] ptt_line is not set — no line to release");
        return 1;
    }
    gpio_t *gpio = open_gpio(cfg);
    if (!gpio)
        return 1;
    int rc = gpio_ptt_init(gpio, 0, (unsigned)cfg->radio.ptt_line, cfg->radio.ptt_active_low);
    if (rc == 0)
        log_info("gpio: PTT line %d released — the transmitter is off", cfg->radio.ptt_line);
    else
        log_error("gpio: cannot take the PTT line %d", cfg->radio.ptt_line);
    if (cfg->radio.pa_line >= 0) {
        int rp = gpio_ptt_init(gpio, 1, (unsigned)cfg->radio.pa_line, cfg->radio.pa_active_low);
        if (rp == 0)
            log_info("gpio: PA line %d released — the amplifier is off", cfg->radio.pa_line);
        else
            log_error("gpio: cannot take the PA line %d", cfg->radio.pa_line);
        if (rp != 0) rc = rp;
    }
    gpio_close(gpio);
    return rc == 0 ? 0 : 1;
}

static void usage(const role_t *role, const char *argv0)
{
    fprintf(stderr,
            "%s %s — %s\n"
            "usage: %s [-c config] [-v] [-t%s]\n"
            "  -c FILE  configuration file (default %s/%s.conf)\n"
            "  -v       verbose: per-event debug logging\n"
            "  -t       check the configuration and exit\n",
            role->binary, TAILCHASER_VERSION, role->summary, argv0, role->tx ? " | -u" : "",
            CONFIG_DIR, role->name);
    if (role->tx)
        fprintf(stderr,
                "  -u       release the PTT line and exit (run by the service after the\n"
                "           daemon has stopped, however it stopped)\n");
}

/* The receiver and the transmitter as the reflector's dashboards show them:
 * frequency, tone, power, antenna. From [radio] and [ctcss] where the program
 * knows, from [location] where it was told. */
static void node_info_radio(reflector_cfg_t *rc, const config_t *cfg, const role_t *role)
{
    const location_config_t *lo = &cfg->location;
    const radio_config_t *r = &cfg->radio;
    double f_rx = lo->freq_rx_mhz > 0 ? lo->freq_rx_mhz : role->rx ? r->freq_rx_hz / 1e6 : 0.0;
    double f_tx = lo->freq_tx_mhz > 0 ? lo->freq_tx_mhz : role->tx ? r->freq_tx_hz / 1e6 : 0.0;
    float t_rx = lo->ctcss_rx_hz != 0.0f ? lo->ctcss_rx_hz
               : role->rx && cfg->ctcss.detect ? cfg->ctcss.tone_hz
               : role->rx ? sa818_ctcss_hz(r->ctcss_rx) : 0.0f;
    float t_tx = lo->ctcss_tx_hz != 0.0f ? lo->ctcss_tx_hz : role->tx ? sa818_ctcss_hz(r->ctcss_tx) : 0.0f;

    char ant[200] = "", esc[130];
    size_t o = 0;
    for (const char *p = lo->antenna; *p && o + 2 < sizeof esc; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '"' || ch == '\\') esc[o++] = '\\';
        if (ch >= 0x20) esc[o++] = (char)ch;
    }
    esc[o] = '\0';
    if (esc[0] || lo->antenna_height_m > 0 || lo->antenna_dir >= 0) {
        int n = snprintf(ant, sizeof ant, ",\"ant\":[{\"comment\":\"%s\"", esc);
        if (lo->antenna_height_m > 0)
            n += snprintf(ant + n, sizeof ant - (size_t)n, ",\"height\":%d", lo->antenna_height_m);
        if (lo->antenna_dir >= 0)
            n += snprintf(ant + n, sizeof ant - (size_t)n, ",\"dir\":%d", lo->antenna_dir);
        snprintf(ant + n, sizeof ant - (size_t)n, "}]");
    }

    /* The receivers, each under its letter: the one in this box first, then
     * those elsewhere ([receiver], [receiver2]..) — the letters the signal
     * reports to the reflector go by (engine.c, report_tick). */
    char rx[1800], tx[280], what[320];
    int n = 0;
    if (f_rx > 0)
        n += snprintf(what + n, sizeof what - (size_t)n, ",\"freq\":%.4f", f_rx);
    if (role->rx || f_rx > 0 || t_rx > 0)
        n += snprintf(what + n, sizeof what - (size_t)n, ",\"sqlType\":\"%s\"", t_rx > 0 ? "CTCSS" : "COS");
    if (t_rx > 0)
        n += snprintf(what + n, sizeof what - (size_t)n, ",\"ctcssFreq\":[%.1f]", (double)t_rx);
    snprintf(what + n, sizeof what - (size_t)n, "%s", ant);
    int n_far = role->reflector ? config_receivers(cfg, role) : 0, letter = 0;
    n = snprintf(rx, sizeof rx, "\"rx\":{");
    if (role->rx || n_far == 0)
        n += snprintf(rx + n, sizeof rx - (size_t)n, "\"%c\":{\"name\":\"Rx1\"%s}", 'A' + letter++, what);
    for (int k = 0; k < n_far && (size_t)n + sizeof what + 80 < sizeof rx; k++) {
        char name[60];
        size_t q = 0;
        for (const char *p = config_receiver(cfg, k)->name; *p && q + 1 < sizeof name; p++)
            if ((unsigned char)*p >= 0x20 && *p != '"' && *p != '\\') name[q++] = *p;
        name[q] = '\0';
        if (!name[0]) snprintf(name, sizeof name, "Rx%d", letter + 1);
        n += snprintf(rx + n, sizeof rx - (size_t)n, "%s\"%c\":{\"name\":\"%s\"%s}", letter ? "," : "", 'A' + letter, name, what);
        letter++;
    }
    snprintf(rx + n, sizeof rx - (size_t)n, "}");

    n = snprintf(tx, sizeof tx, "\"tx\":{\"A\":{\"name\":\"Tx1\"");
    if (f_tx > 0)
        n += snprintf(tx + n, sizeof tx - (size_t)n, ",\"freq\":%.4f", f_tx);
    if (lo->power_w > 0)
        n += snprintf(tx + n, sizeof tx - (size_t)n, ",\"pwr\":%g", (double)lo->power_w);
    if (t_tx > 0)
        n += snprintf(tx + n, sizeof tx - (size_t)n, ",\"ctcssFreq\":%.1f", (double)t_tx);
    snprintf(tx + n, sizeof tx - (size_t)n, "%s}}", ant);

    snprintf(rc->rxtx_json, sizeof rc->rxtx_json, "%s,%s", rx, tx);
    rc->hidden = lo->hidden;
    rc->extra_json[0] = '\0';
    if (lo->tone_to_tg && t_rx > 0 && cfg->link.default_tg)
        snprintf(rc->extra_json, sizeof rc->extra_json, "\"toneToTalkgroup\":{\"%.1f\":%u},",
                 (double)t_rx, (unsigned)cfg->link.default_tg);
}

static void fill_reflector_cfg(reflector_cfg_t *rc, const config_t *cfg, const role_t *role)
{
    const link_config_t *l = &cfg->link;
    memset(rc, 0, sizeof *rc);
    snprintf(rc->domain, sizeof rc->domain, "%s", l->domain);
    snprintf(rc->host, sizeof rc->host, "%s", l->host);
    rc->port = l->port;
    snprintf(rc->callsign, sizeof rc->callsign, "%s", l->callsign);
    snprintf(rc->auth_key, sizeof rc->auth_key, "%s", l->auth_key);
    rc->use_authkey = l->auth_key[0] != '\0';
    rc->verify_host = l->verify_host;
    snprintf(rc->email, sizeof rc->email, "%s", l->email);
    snprintf(rc->state_dir, sizeof rc->state_dir, "%s", l->pki_dir);
    snprintf(rc->location, sizeof rc->location, "%s", l->location);
    snprintf(rc->sysop, sizeof rc->sysop, "%s", l->sysop);
    snprintf(rc->latitude, sizeof rc->latitude, "%s", l->latitude);
    snprintf(rc->longitude, sizeof rc->longitude, "%s", l->longitude);
    snprintf(rc->locator, sizeof rc->locator, "%s", l->locator);
    snprintf(rc->qth_name, sizeof rc->qth_name, "%s", l->qth_name);
    snprintf(rc->sw_version, sizeof rc->sw_version, "%s", TAILCHASER_VERSION);
    node_info_radio(rc, cfg, role);
    rc->default_tg = l->default_tg;
    /* Monitored: what the config lists, plus the home talkgroup — while
     * another one is selected, the node must still hear about its own. */
    for (int i = 0; i < l->n_monitor && rc->n_monitor < REFLECTOR_MAX_MONITOR; i++)
        rc->monitor_tgs[rc->n_monitor++] = l->monitor_tgs[i];
    if (l->default_tg && !config_tg_monitored(cfg, l->default_tg) &&
        rc->n_monitor < REFLECTOR_MAX_MONITOR)
        rc->monitor_tgs[rc->n_monitor++] = l->default_tg;
    rc->tcp_heartbeat_ms = 5000;
    rc->udp_heartbeat_ms = l->udp_heartbeat_s * 1000;
    rc->reconnect_max_ms = 20000;
}

static void log_setup(const config_t *cfg, const role_t *role)
{
    const link_config_t *l = &cfg->link;
    const radio_config_t *r = &cfg->radio;
    if (r->type == RADIO_NONE)
        log_info("radio: not programmed by this daemon — squelch line GPIO %d, PTT GPIO %d (-1 = none)",
                 role->rx ? r->cos_line : -1, role->tx ? r->ptt_line : -1);
    else if (!role->tx)
        log_info("radio: RX %.4f MHz %s, CTCSS %s, squelch %d, volume %d",
                 r->freq_rx_hz / 1e6, r->bandwidth ? "wide" : "narrow",
                 tone_name(r->ctcss_rx), r->squelch, r->volume);
    else if (!role->rx)
        log_info("radio: TX %.4f MHz %s, CTCSS %s", r->freq_tx_hz / 1e6,
                 r->bandwidth ? "wide" : "narrow", tone_name(r->ctcss_tx));
    else if (r->freq_rx_hz == r->freq_tx_hz)
        log_info("radio: %.4f MHz %s, CTCSS RX %s / TX %s, squelch %d, volume %d",
                 r->freq_rx_hz / 1e6, r->bandwidth ? "wide" : "narrow",
                 tone_name(r->ctcss_rx), tone_name(r->ctcss_tx), r->squelch, r->volume);
    else
        log_info("radio: RX %.4f / TX %.4f MHz %s, CTCSS RX %s / TX %s, squelch %d, volume %d",
                 r->freq_rx_hz / 1e6, r->freq_tx_hz / 1e6, r->bandwidth ? "wide" : "narrow",
                 tone_name(r->ctcss_rx), tone_name(r->ctcss_tx), r->squelch, r->volume);
    if (role->rx && cfg->ctcss.detect) {
        const ctcss_config_t *ct = &cfg->ctcss;
        log_info("ctcss: tone squelch in software — %.1f Hz %s (%d ms window, audio %d ms late, "
                 "%.0f dB above the noise, at least %.0f dBFS)%s", (double)ct->tone_hz,
                 r->cos_line >= 0 ? "and the squelch line" : "alone, there is no squelch line",
                 ct->window_ms, ct->delay_ms, (double)ct->open_snr_db, (double)ct->min_level_db,
                 ct->highpass ? ", tone filtered out" : "");
    }
    if (role->tx && cfg->announce.enabled)
        log_info("announce: %s/HHMM.wav every %d min%s, at %.0f dBFS — %s", cfg->announce.dir,
                 cfg->announce.interval_min, cfg->announce.with_ident ? " (after the Morse too)" : "",
                 (double)cfg->announce.level_db,
                 cfg->announce.duck ? "under a conversation when there is one"
                                    : "in a quiet moment, or not at all");
    if (role->tx && cfg->tx.duck_db > 0.0f)
        log_info("tx: Morse and announcements %.0f dB down while somebody is talking", (double)cfg->tx.duck_db);
    if (role->rx && cfg->siglev.mode == SIGLEV_NOISE)
        log_info("siglev: from the receiver noise above %.1f kHz — %.0f dBFS without a signal = 0, "
                 "%.0f dBFS = 100", cfg->siglev.noise_hz / 1000.0, (double)cfg->siglev.noise_open_db,
                 (double)cfg->siglev.noise_full_db);
    if (role->rx && cfg->audio.deemphasis)
        log_info("audio: de-emphasis in software (discriminator audio)");
    if (role->tx && r->pa_line >= 0)
        log_info("radio: amplifier keyed on GPIO %d (active %s)", r->pa_line,
                 r->pa_active_low ? "low" : "high");

    if (role->reflector) {
        char tgs[400] = "";
        size_t o = 0;
        for (int i = 0; i < l->n_monitor && o + 24 < sizeof tgs; i++) {
            o += (size_t)snprintf(tgs + o, sizeof tgs - o, "%s%u", i ? ", " : "", (unsigned)l->monitor_tgs[i]);
            for (int k = 0; k < l->monitor_prio[i] && o + 2 < sizeof tgs; k++)
                tgs[o++] = '+';
            tgs[o] = '\0';
        }
        log_info("reflector: %s as %s, home talkgroup %u, monitoring %s",
                 l->domain[0] ? l->domain : l->host, l->callsign, (unsigned)l->default_tg,
                 tgs[0] ? tgs : "nothing else");
    }

    const char *dest = role->uplink ? "repeater" : "talkgroup";
    if (role->rx) {
        switch (cfg->gate.mode) {
        case GATE_MODE_ON:
            log_info("gate: ON — the %s only hears a voice%s; look-back %d ms, "
                     "in-conversation hold %d s", dest, cfg->gate.dtmf_opens ? " (or DTMF)" : "",
                     cfg->gate.lookback_ms, cfg->gate.hold_s);
            break;
        case GATE_MODE_MONITOR:
            log_info("gate: MONITOR — everything is relayed; the log shows what would be blocked");
            break;
        default:
            log_info("gate: OFF — every squelch opening is relayed");
        }
    }
    if (role->rx || role->remote_rx) {
        if (cfg->audio.normalize)
            log_info("audio: received overs are levelled to %.0f dBFS (at most +%.0f dB)",
                     cfg->audio.normalize_target_db, cfg->audio.normalize_max_gain_db);
        else
            log_info("audio: received audio is passed on at the level it comes in");
    }
    if (role->tx) {
        if (cfg->tx.timeout_s > 0)
            log_info("tx: delay %d ms, hang %d ms, time-out after %d s", cfg->tx.delay_ms,
                     cfg->tx.hang_ms, cfg->tx.timeout_s);
        else
            log_warn("tx: delay %d ms, hang %d ms, NO time-out — nothing limits a transmission",
                     cfg->tx.delay_ms, cfg->tx.hang_ms);
        if (cfg->tx.preemphasis)
            log_info("tx: pre-emphasis in software, limiter at %.0f dBFS", cfg->tx.limit_db);
        if (cfg->ident.interval_min > 0)
            log_info("ident: %s in Morse every %d min on the clock (%d wpm, %d Hz, %.0f dBFS)",
                     cfg->ident.callsign, cfg->ident.interval_min, cfg->ident.wpm,
                     cfg->ident.pitch_hz, cfg->ident.level_db);
        else
            log_warn("ident: OFF — this node never sends its callsign");
    }
}

/* ---- transmitter: what its receiver says, handed to the engine ---- */
typedef struct { engine_t *eng; int link; } rl_ctx_t;
static void rl_squelch(void *ctx, bool open, float siglev) { rl_ctx_t *c = ctx; engine_remote_squelch(c->eng, c->link, open, siglev); }
static void rl_audio(void *ctx, const int16_t *pcm, int n) { rl_ctx_t *c = ctx; engine_remote_audio(c->eng, c->link, pcm, n); }
static void rl_dtmf(void *ctx, char digit, int ms)         { rl_ctx_t *c = ctx; engine_remote_dtmf(c->eng, c->link, digit, ms); }
static void rl_siglev(void *ctx, float siglev)             { rl_ctx_t *c = ctx; engine_remote_siglev(c->eng, c->link, siglev); }

/* refresh_after_tx: the chip gets its filter settings again once the carrier
 * has dropped (some modules lose them next to a power amplifier). */
typedef struct { serial_worker_t *sw; const radio_config_t *r; } refresh_t;
static void refresh_chip(void *ctx)
{
    refresh_t *rf = ctx;
    if (!rf->sw)
        return;
    sw_cmd_t c = { .type = SW_SET_FILTER, .radio_idx = 0, .emphasis = rf->r->filter_emphasis,
                   .highpass = rf->r->filter_highpass, .lowpass = rf->r->filter_lowpass };
    serial_worker_submit(rf->sw, &c);
}

int tailchaser_main(role_id_t role_id, int argc, char **argv)
{
    const role_t *role = role_get(role_id);
    char default_cfg[128];
    snprintf(default_cfg, sizeof default_cfg, "%s/%s.conf", CONFIG_DIR, role->name);
    const char *cfgpath = default_cfg;
    int verbose = 0, opt;
    bool test_only = false, unkey_only = false;
    while ((opt = getopt(argc, argv, "c:vtuh")) != -1) {
        switch (opt) {
        case 'c': cfgpath = optarg; break;
        case 'v': verbose++; break;
        case 't': test_only = true; break;
        case 'u': unkey_only = true; break;
        case 'h': usage(role, argv[0]); return 0;
        default:  usage(role, argv[0]); return 2;
        }
    }
    log_set_level(LOG_INFO);
    umask(0077);                        /* what this daemon writes is key material */

    static config_t config;
    config_t *cfg = &config;
    config_defaults(cfg, role);
    int loaded = config_load(cfg, cfgpath);
    if (loaded == 0 && role->id == ROLE_RECEIVER && cfg->sdr_set) {
        /* a receive site with an [sdr] section: its receiver is that SDR — one on
         * the USB port, or a web SDR — and not a radio on the sound card. The
         * same program, with the defaults of a receiver that makes its own squelch. */
        static role_t with_sdr;
        with_sdr = *role;
        with_sdr.sdr = true;
        role = &with_sdr;
        config_defaults(cfg, role);
        loaded = config_load(cfg, cfgpath);
    }
    if (loaded != 0) {
        log_error("failed to load config %s", cfgpath);
        /* -u goes on with what could be read: a typo in the file must never
         * be the reason a transmitter stays keyed */
        if (!unkey_only)
            return EXIT_CONFIG;
    }
    config_finish(cfg, role);
    int lvl = log_level_from_name(cfg->log_level);
    log_set_level(lvl >= 0 ? (log_level_t)lvl : LOG_INFO);
    if (verbose)
        log_set_level(LOG_DEBUG);
    /* -u needs to know the PTT line, not a complete configuration: a file
     * somebody was halfway through editing must not keep a transmitter keyed. */
    if (unkey_only) {
        if (!role->tx && cfg->radio.ptt_line < 0) {
            log_info("gpio: a receiver without a PTT line — nothing to release");
            return 0;
        }
        return release_ptt(cfg);
    }
    if (config_validate(cfg, role) != 0)
        return EXIT_CONFIG;
    if (test_only) {
        log_setup(cfg, role);
        log_info("configuration OK");
        return 0;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGUSR1, on_test);           /* a test transmission */
    signal(SIGRTMIN + 1, on_call);      /* our own call in fast Morse, as others get it */
    signal(SIGUSR2, on_say);            /* the next spoken announcement, now */
    signal(SIGPIPE, SIG_IGN);

    gpio_t          *gpio = NULL;
    audio_t         *audio = NULL;
    sa818_t         *dev = NULL;
    serial_worker_t *sw = NULL;
    reflector_t     *ref = NULL;
    nettrx_t        *uplink = NULL;
    netrx_t         *rxlinks[CFG_MAX_RECEIVERS] = { NULL };
    static rl_ctx_t  rl_ctx[CFG_MAX_RECEIVERS];
    int              n_rxl = 0;
    sdr_t           *sdr = NULL;
    monitor_t       *monitor = NULL;
    status_t        *status = NULL;
    admin_t         *admin = NULL;
    engine_t        *eng = NULL;
    bool  dev_owned_by_sw = false, hw_cos = false;
    int   rc = 1;
    char  model[96] = "", firmware[48] = "";
    bool  file_input = !strncmp(cfg->audio.alsa_device, "file:", 5);
    const radio_config_t *r = &cfg->radio;
    static refresh_t refresh;

    log_info("%s %s starting", role->binary, TAILCHASER_VERSION);
    log_info("board: %s", detect_pi_model(model, sizeof model));

    if (role->sdr && !file_input) {
        if (config_sdr_local(cfg))
            log_info("receiver: an SDR on the USB port — %s, %.4f MHz", cfg->sdr.type, cfg->sdr.freq_hz / 1e6);
        else
            log_info("receiver: a web SDR — %s at %s, %.4f MHz", cfg->sdr.type, cfg->sdr.url, cfg->sdr.freq_hz / 1e6);
    } else if (file_input) {
        /* Bench mode: no radio, no GPIO, no sound card. */
        log_warn("audio is a FILE (%s) — radio hardware is not used", cfg->audio.alsa_device);
        /* ...unless the test suite hands us a stand-in SA818 on a pty, to see
         * what the chip would be told. */
        const char *bench_radio = getenv("TAILCHASER_BENCH_RADIO");
        if (bench_radio && *bench_radio && r->type == RADIO_SA818) {
            dev = radio_open(r, bench_radio, firmware, sizeof firmware);
            if (!dev)
                goto done;
        }
    } else {
        /* ---- GPIO: the PTT line first, pinned inactive. A receiver that has
         * one (the module's PTT pin) holds it inactive for good. ---- */
        gpio = open_gpio(cfg);
        if (!gpio)
            goto done;
        if (r->ptt_line >= 0 &&
            gpio_ptt_init(gpio, 0, (unsigned)r->ptt_line, r->ptt_active_low) != 0) {
            log_error("gpio: cannot take the PTT line %d — is SvxLink still running?", r->ptt_line);
            goto done;
        }
        if (role->tx && r->pa_line >= 0 &&
            gpio_ptt_init(gpio, 1, (unsigned)r->pa_line, r->pa_active_low) != 0) {
            log_error("gpio: cannot take the PA line %d — is another program holding it?", r->pa_line);
            goto done;
        }

        /* ---- sound card: flat path, the levels the node was set up with ---- */
        {
            char card[72], cardname[80];
            mixer_card_from_device(cfg->audio.alsa_device, card, sizeof card);
            bool wm = mixer_card_is_wm8960(card, cardname, sizeof cardname);
            int cap_pct  = role->rx ? cfg->audio.capture_pct : -1;
            /* a receiver never drives its transmit input */
            int play_pct = role->tx ? cfg->audio.playback_pct : cap_pct >= 0 ? 0 : -1;
            if (cap_pct < 0 && play_pct < 0)
                log_info("audio: sound card \"%s\" — mixer left as it is", cardname);
            else if (!wm)
                log_warn("audio: sound card \"%s\" is not a WM8960 — mixer left as it is; set "
                         "its levels with alsamixer (capture_pct / playback_pct = keep)", cardname);
            else
                mixer_apply_raw_audio(card, play_pct, cap_pct);
        }

        /* ---- SA818: one fixed channel ---- */
        if (r->type == RADIO_SA818) {
            dev = radio_open(r, r->serial_port, firmware, sizeof firmware);
            if (!dev)
                goto done;
        }
        if (role->rx && r->cos_line >= 0) {
            if (gpio_cos_init(gpio, 0, (unsigned)r->cos_line, r->cos_active_low) != 0)
                goto done;
            hw_cos = gpio_cos_read(gpio, 0);
        }
    }
    if (dev) {
        /* from here on the chip is only asked for its signal strength, off the main loop */
        sw = serial_worker_new();
        if (!sw)
            goto done;
        serial_worker_attach(sw, 0, dev);
        dev_owned_by_sw = true;
        if (serial_worker_run(sw) != 0)
            goto done;
    }
    log_setup(cfg, role);

    /* ---- status page ---- */
    status = status_new(cfg->status.bind, cfg->status.http_port);
    if (!status)
        goto done;
    if (cfg->status.admin && cfg->status.http_port > 0) {
        char file[256];
        if (cfg->status.admin_file[0])
            snprintf(file, sizeof file, "%.255s", cfg->status.admin_file);
        else
            snprintf(file, sizeof file, "/var/lib/tailchaser/admin-%s.conf", role->name);
        admin = admin_new(file, cfgpath, role->name, "/proc/self/exe");
        if (admin)
            status_set_admin(status, admin);
    }
    if (cfg->status.monitor && cfg->status.http_port > 0) {
        monitor = monitor_new();
        if (monitor) {
            status_set_monitor(status, monitor);
            log_warn("status: listening in is ON — whoever can open the status page hears the %s ([status] monitor)",
                     role->tx && (role->rx || role->remote_rx) ? "input and the output" : role->tx ? "output" : "input");
        }
    }
    if (role->remote_rx && cfg->receiver.status_port > 0)
        status_set_remote(status, cfg->receiver.host, cfg->receiver.status_port);
    {
        status_snapshot_t *d = status_lock(status);
        snprintf(d->role, sizeof d->role, "%s", role->name);
        snprintf(d->title, sizeof d->title, "%s", role->title);
        d->has_rx = role->rx;
        d->monitor = monitor != NULL;
        d->has_tx = role->tx;
        d->has_reflector = role->reflector;
        d->has_uplink = role->uplink;
        d->has_rxlink = role->remote_rx;
        d->repeater = role->repeater;
        snprintf(d->radio_type, sizeof d->radio_type, "%s", r->type == RADIO_NONE ? "none" : "sa818");
        d->listen_port = cfg->uplink.listen_port;
        d->normalize = cfg->audio.normalize;
        snprintf(d->version, sizeof d->version, "%s", TAILCHASER_VERSION);
        snprintf(d->board, sizeof d->board, "%s", model);
        snprintf(d->firmware, sizeof d->firmware, "%s", firmware);
        d->freq_rx_mhz = r->freq_rx_hz / 1e6;
        d->freq_tx_mhz = r->freq_tx_hz / 1e6;
        snprintf(d->bandwidth, sizeof d->bandwidth, "%s", r->bandwidth ? "wide" : "narrow");
        if (role->rx && cfg->ctcss.detect) {
            snprintf(d->ctcss_rx, sizeof d->ctcss_rx, "%.1f sw", (double)cfg->ctcss.tone_hz);
            d->tone_hz = cfg->ctcss.tone_hz;
        } else {
            snprintf(d->ctcss_rx, sizeof d->ctcss_rx, "%s", tone_name(r->ctcss_rx));
        }
        snprintf(d->ctcss_tx, sizeof d->ctcss_tx, "%s", tone_name(r->ctcss_tx));
        d->squelch = r->squelch;
        d->rssi_lo = cfg->status.rssi_lo;
        d->rssi_hi = cfg->status.rssi_hi;
        snprintf(d->mode, sizeof d->mode, "%s", gate_mode_name(cfg->gate.mode));
        d->hold_s = cfg->gate.hold_s;
        scopy(d->callsign, sizeof d->callsign, cfg->link.callsign);
        scopy(d->ident_callsign, sizeof d->ident_callsign, cfg->ident.callsign);
        d->ident_interval_min = role->tx ? cfg->ident.interval_min : 0;
        d->home_tg = cfg->link.default_tg;
        d->tx_timeout_s = cfg->tx.timeout_s;
        d->started = util_wall_time();
        status_unlock(status);
    }

    /* ---- the links, audio, engine ---- */
    if (role->reflector) {
        reflector_cfg_t rcfg;
        fill_reflector_cfg(&rcfg, cfg, role);
        ref = reflector_new(&rcfg);
        if (!ref)
            goto done;
    }
    if (role->uplink) {
        nettrx_cfg_t ncfg;
        memset(&ncfg, 0, sizeof ncfg);
        ncfg.listen_port = cfg->uplink.listen_port;
        scopy(ncfg.auth_key, sizeof ncfg.auth_key, cfg->uplink.auth_key);
        ncfg.proto_major = cfg->uplink.proto_major;
        ncfg.proto_minor = cfg->uplink.proto_minor;
        ncfg.rx_id       = cfg->uplink.rx_id;
        ncfg.sample_rate = cfg->audio.sample_rate;
        ncfg.preamp_db   = 0.0f;        /* the engine has applied [audio] preamp_db */
        uplink = nettrx_new(&ncfg);
        if (!uplink)
            goto done;
    }
    audio = audio_new(&cfg->audio);
    if (!audio)
        goto done;
    eng = engine_new(cfg, role, audio, ref, uplink, gpio, status);
    if (!eng)
        goto done;
    engine_set_monitor(eng, monitor);
    for (int k = 0; k < config_receivers(cfg, role); k++) {
        const receiver_config_t *rcv = config_receiver(cfg, k);
        netrx_cfg_t xcfg;
        memset(&xcfg, 0, sizeof xcfg);
        scopy(xcfg.host, sizeof xcfg.host, rcv->host);
        xcfg.port = rcv->port;
        scopy(xcfg.auth_key, sizeof xcfg.auth_key, rcv->auth_key);
        scopy(xcfg.codec, sizeof xcfg.codec, rcv->codec);
        xcfg.sample_rate = cfg->audio.sample_rate;
        rl_ctx[k] = (rl_ctx_t){ eng, k };
        netrx_sink_t xs = { &rl_ctx[k], rl_squelch, rl_audio, rl_dtmf, rl_siglev };
        rxlinks[k] = netrx_new(&xcfg, &xs);
        if (!rxlinks[k])
            goto done;
        n_rxl = k + 1;
        engine_add_rxlink(eng, rxlinks[k], rcv->name, rcv->siglev_offset, rcv->lag_ms);
        if (rcv->lag_ms > 0)
            log_info("voter: %s is %d ms behind (set: [receiver%.0d] lag_ms)", rcv->name[0] ? rcv->name : "a receiver", rcv->lag_ms, k ? k + 1 : 0);
    }
    if (n_rxl > 1 || (role->rx && n_rxl > 0))
        log_info("voter: %d receiver%s elsewhere%s — the one that hears a station first has the over, a better one takes it",
                 n_rxl, n_rxl == 1 ? "" : "s", role->rx ? " and the one in this box" : "");
    if (role->tx && r->refresh_after_tx && sw) {
        refresh.sw = sw;
        refresh.r = r;
        engine_set_unkey_hook(eng, refresh_chip, &refresh);
    }
    if (role->sdr && !file_input) {
        sdr = sdr_start(&cfg->sdr, cfg->audio.sample_rate);
        if (!sdr)
            goto done;
        audio_use_sdr(audio, sdr);
        status_snapshot_t *d = status_lock(status);
        d->has_sdr = true;
        snprintf(d->sdr_type, sizeof d->sdr_type, "%.15s", cfg->sdr.type);
        snprintf(d->sdr_url, sizeof d->sdr_url, "%.255s", config_sdr_local(cfg) ? "usb" : cfg->sdr.url);
        d->sdr_local = config_sdr_local(cfg);
        status_unlock(status);
    }
    if (audio_start(audio) != 0) {
        log_error("audio failed to start");
        goto done;
    }
    if (ref && reflector_start(ref) != 0)
        goto done;
    sd_notify_msg("READY=1");

    /* ---- main loop ---- */
    uint64_t last_rssi_ms = 0, last_status_ms = 0, last_wd_ms = 0, last_frame_ms = mono_ms();
    int cos_fd = gpio && role->rx && r->cos_line >= 0 ? gpio_cos_fd(gpio, 0) : -1;
    int sw_fd  = sw ? serial_worker_result_fd(sw) : -1;
    rc = 0;
    while (!g_stop) {
        struct pollfd p[9 + CFG_MAX_RECEIVERS];
        int n = 0, i_audio, i_net = -1, i_ev = -1, i_cos = -1, i_sw = -1;
        int i_listen = -1, i_client = -1, i_rxl[CFG_MAX_RECEIVERS];
        p[i_audio = n++] = (struct pollfd){ audio_fd(audio), POLLIN, 0 };
        if (ref) {
            p[i_net = n++] = (struct pollfd){ reflector_rx_audio_fd(ref), POLLIN, 0 };
            p[i_ev = n++]  = (struct pollfd){ reflector_event_fd(ref), POLLIN, 0 };
        }
        if (cos_fd >= 0) p[i_cos = n++] = (struct pollfd){ cos_fd, POLLIN, 0 };
        if (sw_fd >= 0)  p[i_sw = n++]  = (struct pollfd){ sw_fd, POLLIN, 0 };
        if (uplink) {
            p[i_listen = n++] = (struct pollfd){ nettrx_listen_fd(uplink), POLLIN, 0 };
            if (nettrx_client_fd(uplink) >= 0)
                p[i_client = n++] = (struct pollfd){ nettrx_client_fd(uplink),
                    (short)(POLLIN | (nettrx_want_write(uplink) ? POLLOUT : 0)), 0 };
        }
        for (int k = 0; k < n_rxl; k++) {
            i_rxl[k] = -1;
            if (netrx_fd(rxlinks[k]) >= 0)
                p[i_rxl[k] = n++] = (struct pollfd){ netrx_fd(rxlinks[k]),
                    (short)(POLLIN | (netrx_want_write(rxlinks[k]) ? POLLOUT : 0)), 0 };
        }

        if (poll(p, (nfds_t)n, 100) < 0) {
            if (errno == EINTR)
                continue;
            log_error("poll: %s", strerror(errno));
            rc = 1;
            break;
        }
        uint64_t now = mono_ms();
        if (admin) {
            bool brx, btx;
            admin_blocks(admin, &brx, &btx);
            engine_set_block(eng, brx, btx);
            if (admin_restart_wanted(admin) && engine_idle(eng)) {
                log_info("restart asked for in the admin portal — stopping; the service starts it again");
                break;
            }
        }
        if (g_test) {
            g_test = 0;
            engine_test_tx(eng);
        }
        if (g_call) {
            g_call = 0;
            engine_call_test(eng);
        }
        if (g_say) {
            g_say = 0;
            engine_announce_now(eng);
        }

        /* talkgroup: who is talking, then what they say */
        if (i_ev >= 0 && (p[i_ev].revents & POLLIN)) {
            reflector_event_t ev;
            while (reflector_get_event(ref, &ev) == 0)
                engine_on_event(eng, &ev);
        }
        if (i_net >= 0 && (p[i_net].revents & POLLIN)) {
            uint8_t pkt[REFLECTOR_MAX_OPUS];
            int len;
            while ((len = reflector_rx_audio_pop(ref, pkt, (int)sizeof pkt)) >= 0)
                engine_on_net_audio(eng, pkt, len);
        }

        /* receiver: the repeater that listens to us */
        if (uplink) {
            nettrx_tick(uplink, now);
            if (p[i_listen].revents & POLLIN)
                nettrx_on_accept(uplink);
            if (i_client >= 0 && nettrx_client_fd(uplink) == p[i_client].fd) {
                if (p[i_client].revents & (POLLIN | POLLHUP | POLLERR))
                    nettrx_on_readable(uplink);
                if (nettrx_client_fd(uplink) >= 0 && (p[i_client].revents & POLLOUT))
                    nettrx_on_writable(uplink);
            }
        }
        /* transmitter: the receiver we listen to */
        for (int k = 0; k < n_rxl; k++) {
            netrx_t *rxlink = rxlinks[k];
            if (i_rxl[k] >= 0 && netrx_fd(rxlink) == p[i_rxl[k]].fd) {
                if (p[i_rxl[k]].revents & (POLLOUT | POLLERR | POLLHUP))
                    netrx_on_writable(rxlink);
                if (netrx_fd(rxlink) >= 0 && (p[i_rxl[k]].revents & (POLLIN | POLLHUP | POLLERR)))
                    netrx_on_readable(rxlink);
            }
            netrx_tick(rxlink, now);
        }

        /* radio squelch line */
        if (i_cos >= 0 && (p[i_cos].revents & POLLIN)) {
            int v = gpio_cos_drain(gpio, 0);
            if (v >= 0) hw_cos = (v == 1);
        }

        /* RSSI readings back from the serial worker */
        if (i_sw >= 0 && (p[i_sw].revents & POLLIN)) {
            uint64_t junk;
            if (read(sw_fd, &junk, sizeof junk) < 0) { /* nothing pending */ }
            sw_result_t res;
            while (serial_worker_get_result(sw, &res) == 0)
                if (res.type == SW_RSSI && res.status == 0)
                    engine_on_rssi(eng, res.rssi);
        }

        /* the sound card: one call per 20 ms of audio — the engine's clock */
        if (p[i_audio].revents & POLLIN) {
            uint64_t junk;
            if (read(audio_fd(audio), &junk, sizeof junk) < 0) { /* spurious */ }
            audio_frame_t f;
            while (audio_pop(audio, &f) == 0) {
                engine_on_frame(eng, &f, hw_cos);
                last_frame_ms = now;
            }
            if (audio_failed(audio)) {
                log_error("the sound card stopped — exiting so the service restarts");
                rc = 1;
                break;
            }
        }
        /* The sound card is the engine's clock: without frames nothing moves
         * any more — not the time-out of a keyed transmitter either. A card
         * that stops without reporting an error must not go unnoticed. */
        if (now - last_frame_ms >= AUDIO_STALL_MS) {
            log_error("no audio from the sound card for %d s — exiting so the service restarts",
                      AUDIO_STALL_MS / 1000);
            rc = 1;
            break;
        }
        if (engine_ptt_failed(eng)) {
            log_error("the PTT line does not respond — exiting so the service releases it and restarts");
            rc = 1;
            break;
        }

        if (sw && engine_wants_rssi(eng) && now - last_rssi_ms >= RSSI_POLL_MS) {
            last_rssi_ms = now;
            sw_cmd_t c = { .type = SW_RSSI, .radio_idx = 0 };
            serial_worker_submit(sw, &c);
        }
        if (now - last_status_ms >= STATUS_PUSH_MS) {
            last_status_ms = now;
            engine_push_status(eng);
            if (sdr) {
                sdr_status_t ss;
                sdr_get_status(sdr, &ss);
                status_snapshot_t *d = status_lock(status);
                d->sdr_connected = ss.connected;
                d->sdr_listening = ss.listening;
                snprintf(d->sdr_state, sizeof d->sdr_state, "%s", ss.state);
                snprintf(d->sdr_server, sizeof d->sdr_server, "%s", ss.server);
                d->sdr_smeter_db = ss.smeter_db;
                d->sdr_buffer_ms = ss.buffered_ms;
                d->sdr_rate = ss.in_rate;
                d->sdr_connects = ss.connects;
                d->sdr_underruns = ss.underruns;
                d->sdr_dropped_ms = ss.dropped_ms;
                status_unlock(status);
            }
        }
        if (now - last_wd_ms >= WATCHDOG_MS) {
            last_wd_ms = now;
            sd_notify_msg("WATCHDOG=1");
        }
    }
    log_info("%s stopping", role->binary);
    sd_notify_msg("STOPPING=1");

done:
    if (eng) engine_free(eng);          /* drops the carrier */
    if (audio) audio_stop(audio);
    if (sdr) sdr_stop(sdr);
    if (audio) audio_free(audio);
    for (int k = 0; k < n_rxl; k++) netrx_free(rxlinks[k]);
    if (uplink) nettrx_free(uplink);
    if (ref) reflector_free(ref);
    if (status) status_free(status);
    if (admin) admin_free(admin);
    if (sw) serial_worker_free(sw);
    if (dev && !dev_owned_by_sw) sa818_close(dev);
    if (gpio) gpio_close(gpio);
    return rc;
}
