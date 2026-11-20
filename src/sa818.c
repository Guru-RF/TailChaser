/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "sa818.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

/* CTCSS tones, index 0 == "0.0" (no tone). Standard 38-tone table. */
static const char *const CTCSS[SA818_CTCSS_COUNT] = {
    "0.0", "67.0", "71.9", "74.4", "77.0", "79.7", "82.5", "85.4", "88.5",
    "91.5", "94.8", "97.4", "100.0", "103.5", "107.2", "110.9", "114.8", "118.8",
    "123.0", "127.3", "131.8", "136.5", "141.3", "146.2", "151.4", "156.7",
    "162.2", "167.9", "173.8", "179.9", "186.2", "192.8", "203.5", "210.7",
    "218.1", "225.7", "233.6", "241.8", "250.3"
};

struct sa818 {
    int  fd;
    bool is_pro;
    char firmware[48];
};

/* ---- CTCSS helpers ---- */

int sa818_ctcss_parse(const char *s)
{
    if (!s) return -1;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return -1;
    if (!strcasecmp(s, "none") || !strcasecmp(s, "off"))
        return 0;

    /* Bare integer index 0..38 */
    char *end = NULL;
    long idx = strtol(s, &end, 10);
    if (end && *end == '\0' && idx >= 0 && idx < SA818_CTCSS_COUNT)
        return (int)idx;

    /* Tone string match, e.g. "88.5" (also accept "88.50"/"88"). */
    double want = strtod(s, &end);
    if (end && *end == '\0') {
        for (int i = 0; i < SA818_CTCSS_COUNT; i++) {
            double t = strtod(CTCSS[i], NULL);
            if (fabs(t - want) < 0.05)
                return i;
        }
    }
    return -1;
}

const char *sa818_ctcss_str(int idx)
{
    if (idx < 0 || idx >= SA818_CTCSS_COUNT) return NULL;
    return CTCSS[idx];
}

float sa818_ctcss_hz(int idx)
{
    if (idx <= 0 || idx >= SA818_CTCSS_COUNT) return 0.0f;
    return strtof(CTCSS[idx], NULL);
}

/* ---- low-level serial ---- */

static int set_baud_const(int baud)
{
    switch (baud) {
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    default:     return B9600;
    }
}

static int serial_open(const char *port, int baud)
{
    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        log_error("sa818: open %s: %s", port, strerror(errno));
        return -1;
    }

    struct termios t;
    if (tcgetattr(fd, &t) != 0) {
        log_error("sa818: tcgetattr %s: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    cfmakeraw(&t);
    speed_t sp = (speed_t)set_baud_const(baud);
    cfsetispeed(&t, sp);
    cfsetospeed(&t, sp);
    t.c_cflag |= (CLOCAL | CREAD);
    t.c_cflag &= (tcflag_t)~CSTOPB;          /* 1 stop bit  */
    t.c_cflag &= (tcflag_t)~PARENB;          /* no parity   */
    t.c_cflag = (t.c_cflag & (tcflag_t)~CSIZE) | CS8; /* 8 data bits */
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;                       /* we manage timeout via select */
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        log_error("sa818: tcsetattr %s: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static void serial_flush(int fd) { tcflush(fd, TCIOFLUSH); }

static int serial_send(int fd, const char *cmd)
{
    char buf[128];
    int n = snprintf(buf, sizeof buf, "%s\r\n", cmd);
    if (n < 0 || n >= (int)sizeof buf) return -1;
    log_debug("sa818 -> %s", cmd);
    ssize_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, (size_t)(n - off));
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += w;
    }
    return 0;
}

/* Read a single CRLF-terminated line with a deadline. Returns line length
 * (>=0) into out, or -1 on timeout/error. */
static int serial_readline(int fd, char *out, size_t cap, int timeout_ms)
{
    size_t len = 0;
    uint64_t deadline_ms;
    {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        deadline_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000 + (uint64_t)timeout_ms;
    }

    for (;;) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
        if (now >= deadline_ms)
            return -1;
        int remaining = (int)(deadline_ms - now);

        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv = { remaining / 1000, (remaining % 1000) * 1000 };
        int s = select(fd + 1, &rf, NULL, NULL, &tv);
        if (s < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (s == 0)
            return -1;

        char c;
        ssize_t r = read(fd, &c, 1);
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return -1;
        }
        if (r == 0)
            continue;
        if (c == '\n') {
            while (len > 0 && (out[len - 1] == '\r' || out[len - 1] == ' '))
                len--;
            out[len] = '\0';
            if (len == 0)
                continue;            /* skip empty lines */
            return (int)len;
        }
        if (c == '\r')
            continue;
        if (len < cap - 1)
            out[len++] = c;
    }
}

/* Send a command and wait for a reply line starting with one of the expected
 * prefixes (NULL-terminated array). Returns 0 on match (reply copied to out),
 * -1 otherwise. Command echo is ignored. */
static int sa818_command(sa818_t *dev, const char *cmd,
                         const char *const *expect, int timeout_ms,
                         char *out, size_t cap)
{
    serial_flush(dev->fd);
    if (serial_send(dev->fd, cmd) != 0)
        return -1;

    char line[256];
    uint64_t deadline_ms;
    {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        deadline_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000 + (uint64_t)timeout_ms;
    }

    for (;;) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
        if (now >= deadline_ms)
            return -1;
        int n = serial_readline(dev->fd, line, sizeof line, (int)(deadline_ms - now));
        if (n < 0)
            return -1;
        log_debug("sa818 <- %s", line);
        if (!strcmp(line, cmd))
            continue;   /* echo */
        if (!expect) {
            if (out) snprintf(out, cap, "%s", line);
            return 0;
        }
        for (int i = 0; expect[i]; i++) {
            if (!strncmp(line, expect[i], strlen(expect[i]))) {
                if (out) snprintf(out, cap, "%s", line);
                return 0;
            }
        }
        /* keep reading until deadline */
    }
}

/* ---- public API ---- */

static void detect_version(sa818_t *dev)
{
    static const char *const expect[] = { "+VERSION:", "++VERSION:", NULL };
    char reply[256];
    if (sa818_command(dev, "AT+VERSION", expect, 1500, reply, sizeof reply) != 0) {
        log_warn("sa818: no version response; assuming legacy SA818");
        return;
    }
    const char *colon = strchr(reply, ':');
    const char *val = colon ? colon + 1 : reply;
    snprintf(dev->firmware, sizeof dev->firmware, "%.*s",
             (int)(sizeof dev->firmware - 1), val);
    dev->is_pro = (strcasestr(val, "PRO") != NULL);
    log_info("sa818: firmware '%s' (%s)", dev->firmware, dev->is_pro ? "SA818Pro" : "SA818");
}

sa818_t *sa818_open(const char *port, int baud)
{
    sa818_t *dev = calloc(1, sizeof *dev);
    if (!dev)
        return NULL;
    dev->fd = serial_open(port, baud ? baud : SA818_DEFAULT_BAUD);
    if (dev->fd < 0) {
        free(dev);
        return NULL;
    }

    static const char *const expect[] = { "+DMOCONNECT:", NULL };
    char reply[64];
    bool ok = false;
    for (int i = 0; i < 3; i++) {
        if (sa818_command(dev, "AT+DMOCONNECT", expect, 1500, reply, sizeof reply) == 0 &&
            !strcmp(reply, "+DMOCONNECT:0")) {
            ok = true;
            break;
        }
        struct timespec ts = { 0, 200 * 1000000 };
        nanosleep(&ts, NULL);
    }
    if (!ok) {
        log_error("sa818: handshake failed on %s", port);
        close(dev->fd);
        free(dev);
        return NULL;
    }

    detect_version(dev);
    log_info("sa818: connected on %s", port);
    return dev;
}

void sa818_close(sa818_t *dev)
{
    if (!dev) return;
    if (dev->fd >= 0) close(dev->fd);
    free(dev);
}

bool        sa818_is_pro(const sa818_t *dev)   { return dev->is_pro; }
const char *sa818_firmware(const sa818_t *dev) { return dev->firmware; }

static bool freq_close(uint32_t hz, const char *mhz_str)
{
    double got = strtod(mhz_str, NULL) * 1e6;
    return fabs(got - (double)hz) <= 100.0;   /* within 100 Hz */
}

int sa818_read_group(sa818_t *dev, sa818_group_t *out)
{
    static const char *const expect[] = { "+DMOREADGROUP:", "+DMOREADGROUP=", NULL };
    char reply[256];
    if (sa818_command(dev, "AT+DMOREADGROUP", expect, 1500, reply, sizeof reply) != 0)
        return -1;

    const char *p = strpbrk(reply, ":=");
    if (!p) return -1;
    p++;

    char bw[8], txf[16], rxf[16], txc[8], sq[8], rxc[8];
    if (sscanf(p, "%7[^,],%15[^,],%15[^,],%7[^,],%7[^,],%7s",
               bw, txf, rxf, txc, sq, rxc) != 6)
        return -1;

    out->bw         = atoi(bw);
    out->tx_freq_hz = (uint32_t)(strtod(txf, NULL) * 1e6 + 0.5);
    out->rx_freq_hz = (uint32_t)(strtod(rxf, NULL) * 1e6 + 0.5);
    out->tx_ctcss   = atoi(txc);
    out->squelch    = atoi(sq);
    out->rx_ctcss   = atoi(rxc);
    return 0;
}

static int send_setgroup(sa818_t *dev, const char *args)
{
    static const char *const expect[] = { "+DMOSETGROUP:", "+DMOERROR", NULL };
    char cmd[200], reply[64];

    const char *seps = dev->is_pro ? "=" : ":=";
    for (const char *sp = seps; *sp; sp++) {
        snprintf(cmd, sizeof cmd, "AT+DMOSETGROUP%c%s", *sp, args);
        if (sa818_command(dev, cmd, expect, 2000, reply, sizeof reply) == 0) {
            if (!strcmp(reply, "+DMOSETGROUP:0")) return 0;   /* OK */
            if (!strcmp(reply, "+DMOSETGROUP:1")) return 1;   /* range error / Pro quirk */
        }
    }
    return -1;
}

int sa818_set_group(sa818_t *dev, int bw, uint32_t tx_hz, uint32_t rx_hz,
                    int ctcss_tx_idx, int ctcss_rx_idx, int squelch)
{
    char args[160];
    snprintf(args, sizeof args, "%d,%.4f,%.4f,%04d,%d,%04d",
             bw ? 1 : 0,
             (double)tx_hz / 1e6, (double)rx_hz / 1e6,
             ctcss_tx_idx, squelch, ctcss_rx_idx);

    int rc = send_setgroup(dev, args);

    /* SA818Pro may report :1 even on success — verify with a readback. */
    if (rc != 0) {
        struct timespec ts = { 0, 200 * 1000000 };
        nanosleep(&ts, NULL);
        sa818_group_t g;
        if (sa818_read_group(dev, &g) == 0) {
            char txf[16], rxf[16];
            snprintf(txf, sizeof txf, "%.4f", (double)g.tx_freq_hz / 1e6);
            snprintf(rxf, sizeof rxf, "%.4f", (double)g.rx_freq_hz / 1e6);
            if (g.bw == (bw ? 1 : 0) &&
                freq_close(tx_hz, txf) && freq_close(rx_hz, rxf) &&
                g.tx_ctcss == ctcss_tx_idx && g.rx_ctcss == ctcss_rx_idx &&
                g.squelch == squelch) {
                log_debug("sa818: setgroup not acked but readback confirms settings");
                rc = 0;
            }
        }
    }

    if (rc != 0) {
        log_error("sa818: setgroup failed (bw=%d tx=%.4f rx=%.4f ctcss=%d/%d sq=%d)",
                  bw, (double)tx_hz / 1e6, (double)rx_hz / 1e6,
                  ctcss_tx_idx, ctcss_rx_idx, squelch);
        return -1;
    }
    log_info("sa818: channel set rx=%.4f tx=%.4f bw=%s sq=%d ctcss=%s/%s",
             (double)rx_hz / 1e6, (double)tx_hz / 1e6, bw ? "wide" : "narrow",
             squelch, sa818_ctcss_str(ctcss_tx_idx), sa818_ctcss_str(ctcss_rx_idx));
    return 0;
}

int sa818_set_filter(sa818_t *dev, int emphasis, int highpass, int lowpass)
{
    static const char *const expect[] = { "+DMOSETFILTER:", "+DMOERROR", NULL };
    char cmd[64], reply[64], args[32];
    snprintf(args, sizeof args, "%d,%d,%d", emphasis, highpass, lowpass);

    /* Like DMOSETGROUP, the separator varies by module: SA818Pro uses '=',
     * other modules (e.g. SA868S) want ':'. Try both before giving up — the
     * old '='-only form was silently rejected on the SA868S, so the chip
     * filters/emphasis were never actually applied. */
    const char *seps = dev->is_pro ? "=" : ":=";
    for (const char *sp = seps; *sp; sp++) {
        snprintf(cmd, sizeof cmd, "AT+SETFILTER%c%s", *sp, args);
        if (sa818_command(dev, cmd, expect, 2000, reply, sizeof reply) == 0 &&
            strcmp(reply, "+DMOSETFILTER:0") == 0) {
            log_info("sa818: filters emphasis=%s highpass=%s lowpass=%s",
                     emphasis ? "off" : "on", highpass ? "off" : "on", lowpass ? "off" : "on");
            return 0;
        }
    }
    log_error("sa818: set filter failed");
    return -1;
}

int sa818_set_volume(sa818_t *dev, int level)
{
    static const char *const expect[] = { "+DMOSETVOLUME:", NULL };
    char cmd[48], reply[64];
    snprintf(cmd, sizeof cmd, "AT+DMOSETVOLUME=%d", level);
    if (sa818_command(dev, cmd, expect, 2000, reply, sizeof reply) != 0 ||
        strcmp(reply, "+DMOSETVOLUME:0") != 0) {
        log_error("sa818: set volume failed");
        return -1;
    }
    return 0;
}

int sa818_set_tail(sa818_t *dev, int open)
{
    static const char *const expect[] = { "+DMOSETTAIL:", NULL };
    char cmd[48], reply[64];
    snprintf(cmd, sizeof cmd, "AT+SETTAIL=%d", open ? 1 : 0);
    if (sa818_command(dev, cmd, expect, 2000, reply, sizeof reply) != 0 ||
        strcmp(reply, "+DMOSETTAIL:0") != 0) {
        log_error("sa818: set tail failed");
        return -1;
    }
    return 0;
}

int sa818_rssi(sa818_t *dev, int *out)
{
    static const char *const expect[] = { "RSSI:", "RSSI=", NULL };
    char reply[64];
    if (sa818_command(dev, "RSSI?", expect, 1500, reply, sizeof reply) != 0 &&
        sa818_command(dev, "AT+RSSI?", expect, 1500, reply, sizeof reply) != 0)
        return -1;
    const char *p = strpbrk(reply, ":=");
    if (!p) return -1;
    *out = atoi(p + 1);
    return 0;
}
