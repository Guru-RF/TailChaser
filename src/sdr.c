/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "sdr.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <opus/opus.h>

#include "log.h"
#include "util.h"

#define RING_MS        4000         /* the most audio kept in store                  */
#define SILENT_MS      15000        /* tuned, and no audio for this long: reconnect  */
#define MSG_MAX        (1 << 20)
#define RS_HALF        8            /* resampler: taps each side                     */
#define RS_PHASES      64
#define MAX_PROFILES   48

typedef struct sdr_backend {
    const char *name;
    /* the TCP (and TLS) connection is up: handshake, first messages. 0 = fine */
    int  (*open)(sdr_t *s);
    /* one websocket message. -1 = give this connection up */
    int  (*message)(sdr_t *s, int op, uint8_t *p, size_t n);
    /* called a few times a second: keep-alives, retries. -1 = give up */
    int  (*tick)(sdr_t *s, uint64_t now);
} sdr_backend_t;

typedef struct {
    int      index, prev;
    uint8_t  buf[8];
    int      have;          /* bytes of a SYNC header gathered */
    bool     synced, plain;
    int      left;
} adpcm_t;

struct sdr {
    sdr_config_t cfg;
    int          out_rate;
    const sdr_backend_t *be;
    pthread_t    thread;
    bool         running;
    atomic_bool  stop;

    /* where: parsed from the URL */
    bool  tls;
    char  host[128], path[160];
    int   port;

    /* the connection (thread only) */
    int      fd;
    SSL_CTX *ctx;
    SSL     *ssl;
    uint8_t *in;            /* bytes read and not yet used */
    size_t   in_len, in_cap;
    uint8_t *frag;          /* a message that comes in pieces */
    size_t   frag_len;
    int      frag_op;
    uint64_t last_audio, opened_at, next_ka;
    int      retry_s;

    /* the audio: thread -> sdr_take() */
    pthread_mutex_t mu;
    int16_t *ring;
    int      ring_cap, ring_head, ring_len;
    bool     primed;
    sdr_status_t st;

    /* resampler (thread only) */
    int      in_rate;
    float    rs_tab[RS_PHASES][2 * RS_HALF];
    float    rs_buf[4096 + 2 * RS_HALF];
    int      rs_len;
    double   rs_pos;

    /* OpenWebRX */
    adpcm_t  adpcm;
    bool     owrx_adpcm;
    double   center, span;
    char     sdr_id[64], profile_id[64];
    char     prof_id[MAX_PROFILES][96], prof_name[MAX_PROFILES][64];
    int      n_prof;
    char     tried[MAX_PROFILES][64];
    int      n_tried;
    uint64_t search_at;     /* the next step of the search for a device, or 0 */
    bool     tuned;
    double   tuned_center;

    /* KiwiSDR, Web-888, UberSDR: see sdr_kiwi.inc / sdr_uber.inc */
    int      kiwi_stage, kiwi_rate;
    double   kiwi_offset_khz;
    char     session[48];
    OpusDecoder *opus;
    int      opus_rate;
};

/* ------------------------------------------------------------------ */
/* state, in words                                                     */

static void say(sdr_t *s, bool connected, bool listening, const char *fmt, ...)
{
    char text[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&s->mu);
    bool changed = strcmp(text, s->st.state) != 0;
    snprintf(s->st.state, sizeof s->st.state, "%s", text);
    s->st.connected = connected;
    s->st.listening = listening;
    pthread_mutex_unlock(&s->mu);
    if (changed)
        log_info("sdr: %s", text);
}

/* ------------------------------------------------------------------ */
/* the audio on its way out                                            */

static void ring_put(sdr_t *s, const int16_t *pcm, int n)
{
    pthread_mutex_lock(&s->mu);
    for (int i = 0; i < n; i++) {
        if (s->ring_len == s->ring_cap) {           /* nobody takes it: the oldest goes */
            s->ring_head = (s->ring_head + 1) % s->ring_cap;
            s->ring_len--;
        }
        s->ring[(s->ring_head + s->ring_len++) % s->ring_cap] = pcm[i];
    }
    pthread_mutex_unlock(&s->mu);
}

static void rs_setup(sdr_t *s, int in_rate)
{
    s->in_rate = in_rate;
    s->rs_len = 0;
    s->rs_pos = RS_HALF;
    double fc = 0.92 * (s->out_rate < in_rate ? (double)s->out_rate / in_rate : 1.0);
    for (int p = 0; p < RS_PHASES; p++) {
        double frac = (double)p / RS_PHASES, sum = 0.0, w[2 * RS_HALF];
        for (int k = 0; k < 2 * RS_HALF; k++) {
            double x = (k - RS_HALF + 1) - frac;            /* distance to the output sample */
            double sinc = fabs(x) < 1e-9 ? 1.0 : sin(M_PI * fc * x) / (M_PI * fc * x);
            double win = 0.5 + 0.5 * cos(M_PI * x / RS_HALF);
            w[k] = fabs(x) >= RS_HALF ? 0.0 : sinc * win;
            sum += w[k];
        }
        for (int k = 0; k < 2 * RS_HALF; k++)
            s->rs_tab[p][k] = (float)(w[k] / sum);
    }
    pthread_mutex_lock(&s->mu);
    s->st.in_rate = in_rate;
    pthread_mutex_unlock(&s->mu);
}

/* Audio from the receiver, at its rate. */
static void emit(sdr_t *s, const int16_t *pcm, int n, int in_rate)
{
    if (n <= 0 || in_rate < 4000 || in_rate > 96000)
        return;
    s->last_audio = mono_ms();
    if (in_rate == s->out_rate) {
        ring_put(s, pcm, n);
        return;
    }
    if (in_rate != s->in_rate)
        rs_setup(s, in_rate);
    double step = (double)in_rate / s->out_rate;
    for (int off = 0; off < n;) {
        int room = (int)(sizeof s->rs_buf / sizeof s->rs_buf[0]) - s->rs_len;
        int take = n - off < room ? n - off : room;
        for (int i = 0; i < take; i++)
            s->rs_buf[s->rs_len++] = (float)pcm[off + i];
        off += take;

        int16_t out[2048];
        int no = 0;
        while ((int)s->rs_pos + RS_HALF < s->rs_len) {
            int i0 = (int)s->rs_pos;
            const float *w = s->rs_tab[(int)((s->rs_pos - i0) * RS_PHASES)];
            const float *x = s->rs_buf + i0 - RS_HALF + 1;
            float acc = 0.0f;
            for (int k = 0; k < 2 * RS_HALF; k++)
                acc += w[k] * x[k];
            out[no++] = (int16_t)(acc > 32767.0f ? 32767 : acc < -32768.0f ? -32768 : lrintf(acc));
            s->rs_pos += step;
            if (no == (int)(sizeof out / sizeof out[0])) {
                ring_put(s, out, no);
                no = 0;
            }
        }
        ring_put(s, out, no);
        int drop = (int)s->rs_pos - RS_HALF;        /* samples no output needs any more */
        if (drop > 0) {
            if (drop > s->rs_len) drop = s->rs_len;
            memmove(s->rs_buf, s->rs_buf + drop, sizeof(float) * (size_t)(s->rs_len - drop));
            s->rs_len -= drop;
            s->rs_pos -= drop;
        }
    }
}

int sdr_take(sdr_t *s, int16_t *out, int n)
{
    int got = 0;
    pthread_mutex_lock(&s->mu);
    int store = s->cfg.buffer_ms * s->out_rate / 1000;
    if (!s->primed && s->ring_len >= store + n)
        s->primed = true;
    /* the receiver's clock runs a little faster than ours, or the internet
     * held a second back and sent it at once: what is too old goes */
    if (s->ring_len > store + s->out_rate) {
        int drop = s->ring_len - store - n;
        s->ring_head = (s->ring_head + drop) % s->ring_cap;
        s->ring_len -= drop;
        s->st.dropped_ms += (unsigned long)(drop * 1000L / s->out_rate);
    }
    if (s->primed) {
        if (s->ring_len >= n) {
            for (int i = 0; i < n; i++)
                out[i] = s->ring[(s->ring_head + i) % s->ring_cap];
            s->ring_head = (s->ring_head + n) % s->ring_cap;
            s->ring_len -= n;
            got = n;
        } else {
            s->primed = false;                      /* ran empty: gather again */
            if (s->st.listening)
                s->st.underruns++;
        }
    }
    s->st.buffered_ms = s->ring_len * 1000 / s->out_rate;
    pthread_mutex_unlock(&s->mu);
    return got;
}

void sdr_get_status(sdr_t *s, sdr_status_t *out)
{
    pthread_mutex_lock(&s->mu);
    *out = s->st;
    pthread_mutex_unlock(&s->mu);
}

/* ------------------------------------------------------------------ */
/* bytes: TCP, TLS                                                     */

static void io_close(sdr_t *s)
{
    if (s->ssl) { SSL_free(s->ssl); s->ssl = NULL; }
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
    s->in_len = 0;
    s->frag_len = 0;
}

static int io_connect(sdr_t *s, const char *host, int port, bool tls)
{
    char p[16];
    snprintf(p, sizeof p, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, p, &hints, &res) != 0 || !res)
        return -1;
    int fd = -1;
    for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            continue;
        struct timeval tv = { 8, 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        if (connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
            log_debug("sdr: connect to %s (%s): %s", host, a->ai_family == AF_INET6 ? "IPv6" : "IPv4", strerror(errno));
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd < 0)
        return -1;
    s->fd = fd;
    if (!tls)
        return 0;
    if (!s->ctx) {
        s->ctx = SSL_CTX_new(TLS_client_method());
        if (!s->ctx)
            return -1;
        SSL_CTX_set_min_proto_version(s->ctx, TLS1_2_VERSION);
        SSL_CTX_set_default_verify_paths(s->ctx);
    }
    s->ssl = SSL_new(s->ctx);
    if (!s->ssl)
        return -1;
    SSL_set_fd(s->ssl, fd);
    SSL_set_tlsext_host_name(s->ssl, host);
    if (s->cfg.tls_verify) {
        SSL_set_verify(s->ssl, SSL_VERIFY_PEER, NULL);
        SSL_set1_host(s->ssl, host);
    }
    if (SSL_connect(s->ssl) != 1) {
        log_warn("sdr: TLS with %s failed%s", host,
                 s->cfg.tls_verify ? " (its certificate has to be valid; [sdr] tls_verify = false to accept any)" : "");
        ERR_clear_error();
        return -1;
    }
    return 0;
}

static int io_write(sdr_t *s, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n > 0) {
        ssize_t w = s->ssl ? SSL_write(s->ssl, p, (int)n) : send(s->fd, p, n, MSG_NOSIGNAL);
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* More bytes into s->in. 1 = got some, 0 = nothing within the time, -1 = closed. */
static int io_fill(sdr_t *s, int timeout_ms)
{
    if (s->in_len == s->in_cap) {
        size_t cap = s->in_cap ? s->in_cap * 2 : 16384;
        if (cap > MSG_MAX + 64)
            return -1;
        uint8_t *nb = realloc(s->in, cap);
        if (!nb)
            return -1;
        s->in = nb;
        s->in_cap = cap;
    }
    if (!s->ssl || SSL_pending(s->ssl) == 0) {
        struct pollfd p = { s->fd, POLLIN, 0 };
        int rc = poll(&p, 1, timeout_ms);
        if (rc == 0 || (rc < 0 && errno == EINTR))
            return 0;
        if (rc < 0)
            return -1;
    }
    ssize_t r;
    if (s->ssl) {
        r = SSL_read(s->ssl, s->in + s->in_len, (int)(s->in_cap - s->in_len));
        if (r <= 0) {
            int e = SSL_get_error(s->ssl, (int)r);
            ERR_clear_error();
            return e == SSL_ERROR_WANT_READ ? 0 : -1;
        }
    } else {
        r = recv(s->fd, s->in + s->in_len, s->in_cap - s->in_len, 0);
        if (r < 0 && (errno == EAGAIN || errno == EINTR))
            return 0;
        if (r <= 0)
            return -1;
    }
    s->in_len += (size_t)r;
    return 1;
}

static void io_used(sdr_t *s, size_t n)
{
    memmove(s->in, s->in + n, s->in_len - n);
    s->in_len -= n;
}

/* ------------------------------------------------------------------ */
/* HTTP and the websocket                                              */

/* One HTTP request on the connection; the answer's head and body end up in
 * s->in (NUL-terminated). Returns the status code, or -1. */
static int http_exchange(sdr_t *s, const char *request, size_t len, bool whole_body)
{
    if (io_write(s, request, len) != 0)
        return -1;
    uint64_t until = mono_ms() + 10000;
    for (;;) {
        if (s->in_len > 0) {
            s->in[s->in_len < s->in_cap ? s->in_len : s->in_cap - 1] = '\0';
            if (!whole_body && memmem(s->in, s->in_len, "\r\n\r\n", 4))
                break;
        }
        if (mono_ms() > until || atomic_load(&s->stop))
            return -1;
        int rc = io_fill(s, 250);
        if (rc < 0) {
            if (whole_body && s->in_len > 0)
                break;                              /* Connection: close — that was all */
            return -1;
        }
        if (whole_body && s->in_len > 0) {
            /* a body with a Content-Length that has arrived */
            s->in[s->in_len < s->in_cap ? s->in_len : s->in_cap - 1] = '\0';
            char *he = memmem(s->in, s->in_len, "\r\n\r\n", 4);
            char *cl = he ? strcasestr((char *)s->in, "content-length:") : NULL;
            if (he && cl && cl < he && s->in_len >= (size_t)(he + 4 - (char *)s->in) + (size_t)atol(cl + 15))
                break;
        }
    }
    if (s->in_len == s->in_cap && io_fill(s, 0) < 0)
        return -1;
    s->in[s->in_len < s->in_cap ? s->in_len : s->in_cap - 1] = '\0';
    if (s->in_len < 12 || strncmp((char *)s->in, "HTTP/1.", 7) != 0)
        return -1;
    return atoi((char *)s->in + 9);
}

static int ws_handshake(sdr_t *s, const char *path, const char *extra_headers)
{
    uint8_t rnd[16];
    char key[32], req[1024];
    RAND_bytes(rnd, sizeof rnd);
    EVP_EncodeBlock((unsigned char *)key, rnd, sizeof rnd);
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\nHost: %s%s%.0d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: %s://%s\r\n"
                     "User-Agent: TailChaser-SDRReceiver/" TAILCHASER_VERSION "\r\n%s\r\n",
                     path, s->host, (s->tls ? s->port != 443 : s->port != 80) ? ":" : "",
                     (s->tls ? s->port != 443 : s->port != 80) ? s->port : 0, key,
                     s->tls ? "https" : "http", s->host, extra_headers ? extra_headers : "");
    if (n <= 0 || n >= (int)sizeof req)
        return -1;
    int code = http_exchange(s, req, (size_t)n, false);
    log_debug("sdr: GET %s -> %d", path, code);
    if (code != 101) {
        if (code > 0)
            log_warn("sdr: %s%s is no websocket (HTTP %d)", s->host, path, code);
        return -1;
    }
    char *he = memmem(s->in, s->in_len, "\r\n\r\n", 4);
    io_used(s, (size_t)(he + 4 - (char *)s->in));
    return 0;
}

static int ws_send(sdr_t *s, int op, const void *payload, size_t n)
{
    uint8_t hdr[14], mask[4];
    size_t h = 0;
    RAND_bytes(mask, sizeof mask);
    hdr[h++] = (uint8_t)(0x80 | op);
    if (n < 126) {
        hdr[h++] = (uint8_t)(0x80 | n);
    } else if (n < 65536) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (uint8_t)(n >> 8);
        hdr[h++] = (uint8_t)n;
    } else {
        return -1;
    }
    memcpy(hdr + h, mask, 4);
    h += 4;
    uint8_t *out = malloc(h + n);
    if (!out)
        return -1;
    memcpy(out, hdr, h);
    for (size_t i = 0; i < n; i++)
        out[h + i] = ((const uint8_t *)payload)[i] ^ mask[i & 3];
    int rc = io_write(s, out, h + n);
    free(out);
    return rc;
}

static int ws_text(sdr_t *s, const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof text)
        return -1;
    return ws_send(s, 1, text, (size_t)n);
}

/* The next whole message into *p (valid until the next call). 1 = a message,
 * 0 = none within the time, -1 = the connection is gone. */
static int ws_recv(sdr_t *s, int timeout_ms, int *op, uint8_t **p, size_t *n)
{
    uint64_t until = mono_ms() + (uint64_t)timeout_ms;
    for (;;) {
        while (s->in_len >= 2) {
            bool fin = s->in[0] & 0x80;
            int  o = s->in[0] & 15;
            size_t len = s->in[1] & 127, off = 2;
            if (len == 126) {
                if (s->in_len < 4) break;
                len = ((size_t)s->in[2] << 8) | s->in[3];
                off = 4;
            } else if (len == 127) {
                if (s->in_len < 10) break;
                len = 0;
                for (int i = 2; i < 10; i++) len = (len << 8) | s->in[i];
                off = 10;
            }
            if (s->in[1] & 0x80) off += 4;          /* a server does not mask; skip if it does */
            if (len > MSG_MAX)
                return -1;
            if (s->in_len < off + len) break;
            uint8_t *body = s->in + off;
            if (o == 9) {
                ws_send(s, 10, body, len < 125 ? len : 125);
            } else if (o == 8) {
                return -1;
            } else if (o != 10) {
                if (o != 0) {
                    s->frag_len = 0;
                    s->frag_op = o;
                }
                if (s->frag_len + len > MSG_MAX)
                    return -1;
                uint8_t *nb = realloc(s->frag, s->frag_len + len + 1);
                if (!nb)
                    return -1;
                s->frag = nb;
                memcpy(s->frag + s->frag_len, body, len);
                s->frag_len += len;
                s->frag[s->frag_len] = '\0';
                if (fin) {
                    io_used(s, off + len);
                    *op = s->frag_op;
                    *p = s->frag;
                    *n = s->frag_len;
                    s->frag_len = 0;
                    return 1;
                }
            }
            io_used(s, off + len);
        }
        uint64_t now = mono_ms();
        if (now >= until)
            return 0;
        int rc = io_fill(s, (int)(until - now));
        if (rc < 0)
            return -1;
    }
}

/* ------------------------------------------------------------------ */
/* a little JSON: the value of a key, wherever it is in the text       */

static const char *jfind(const char *json, const char *key)
{
    char pat[80];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        p += strlen(pat);
        while (*p == ' ') p++;
        if (*p == ':') {
            p++;
            while (*p == ' ') p++;
            return p;
        }
    }
    return NULL;
}

static bool jnum(const char *json, const char *key, double *out)
{
    const char *p = jfind(json, key);
    if (!p || !(isdigit((unsigned char)*p) || *p == '-'))
        return false;
    *out = strtod(p, NULL);
    return true;
}

static bool jstr(const char *json, const char *key, char *out, size_t cap)
{
    const char *p = jfind(json, key);
    if (!p || *p != '"')
        return false;
    size_t n = 0;
    for (p++; *p && *p != '"' && n + 1 < cap; p++) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p;
    }
    out[n] = '\0';
    return true;
}

/* ------------------------------------------------------------------ */
/* IMA ADPCM as csdr sends it (OpenWebRX)                              */

static const int16_t ADPCM_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
    80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
    2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int8_t ADPCM_ADJ[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static int16_t adpcm_nibble(adpcm_t *a, int code)
{
    int step = ADPCM_STEP[a->index], diff = step >> 3;
    if (code & 1) diff += step >> 2;
    if (code & 2) diff += step >> 1;
    if (code & 4) diff += step;
    a->prev += code & 8 ? -diff : diff;
    if (a->prev > 32767) a->prev = 32767;
    if (a->prev < -32768) a->prev = -32768;
    a->index += ADPCM_ADJ[code];
    if (a->index < 0) a->index = 0;
    if (a->index > 88) a->index = 88;
    return (int16_t)a->prev;
}

/* "SYNC" + index + predictor (int16 LE each) before the first byte of codes
 * and after every 1001 of them; low nibble first. `plain`: no SYNC at all. */
static void owrx_adpcm(sdr_t *s, const uint8_t *p, size_t n)
{
    adpcm_t *a = &s->adpcm;
    int16_t out[1024];
    int no = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = p[i];
        if (!a->plain && (!a->synced || a->left == 0)) {
            static const char SYNC[4] = { 'S', 'Y', 'N', 'C' };
            if (a->have < 4) {
                if (b == (uint8_t)SYNC[a->have]) a->have++;
                else a->have = b == 'S' ? 1 : 0;
                if (a->have < 4) a->synced = false;
                continue;
            }
            a->buf[a->have++] = b;
            if (a->have < 8)
                continue;
            a->index = (int16_t)(a->buf[4] | (a->buf[5] << 8));
            a->prev  = (int16_t)(a->buf[6] | (a->buf[7] << 8));
            if (a->index < 0) a->index = 0;
            if (a->index > 88) a->index = 88;
            a->have = 0;
            a->synced = true;
            a->left = 1001;
            continue;
        }
        out[no++] = adpcm_nibble(a, b & 15);
        out[no++] = adpcm_nibble(a, b >> 4);
        if (!a->plain) a->left--;
        if (no == 1024) {
            emit(s, out, no, 12000);
            no = 0;
        }
    }
    emit(s, out, no, 12000);
}

/* ------------------------------------------------------------------ */
/* OpenWebRX                                                           */

static const char *owrx_profile_name(const sdr_t *s)
{
    char id[160];
    snprintf(id, sizeof id, "%s|%s", s->sdr_id, s->profile_id);
    for (int i = 0; i < s->n_prof; i++)
        if (!strcmp(s->prof_id[i], id))
            return s->prof_name[i];
    return s->profile_id[0] ? s->profile_id : "?";
}

static bool owrx_covers(const sdr_t *s)
{
    return s->span > 0 && fabs(s->cfg.freq_hz - s->center) <= 0.48 * s->span;
}

static int owrx_tune(sdr_t *s)
{
    int half = s->cfg.bandwidth_hz / 2;
    if (ws_text(s, "{\"type\":\"dspcontrol\",\"params\":{\"low_cut\":%d,\"high_cut\":%d,\"offset_freq\":%ld,"
                   "\"mod\":\"nfm\",\"squelch_level\":-150,\"secondary_mod\":false}}",
                -half, half, lrint(s->cfg.freq_hz - s->center)) != 0 ||
        ws_text(s, "{\"type\":\"dspcontrol\",\"action\":\"start\"}") != 0)
        return -1;
    s->tuned = true;
    s->tuned_center = s->center;
    s->search_at = 0;
    s->n_tried = 0;
    s->last_audio = mono_ms();
    say(s, true, true, "listening on %.4f MHz — %s, profile \"%s\"", s->cfg.freq_hz / 1e6, s->host,
        owrx_profile_name(s));
    return 0;
}

/* The device this connection is on does not have the frequency. Another
 * device of the receiver may: moving there moves this connection only. The
 * profile of a device is everybody's — it is never changed from here. */
static int owrx_search(sdr_t *s, uint64_t now)
{
    s->tuned = false;
    /* the device that has the profile asked for first, then the others */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < s->n_prof; i++) {
            char dev[64];
            snprintf(dev, sizeof dev, "%.*s", (int)strcspn(s->prof_id[i], "|"), s->prof_id[i]);
            if (pass == 0 && !(s->cfg.profile[0] && strcasestr(s->prof_name[i], s->cfg.profile)))
                continue;
            bool done = !strcmp(dev, s->sdr_id) && s->n_tried > 0;
            for (int t = 0; t < s->n_tried; t++)
                if (!strcmp(s->tried[t], dev)) done = true;
            if (done || s->n_tried >= MAX_PROFILES)
                continue;
            snprintf(s->tried[s->n_tried++], sizeof s->tried[0], "%s", dev);
            if (!strcmp(dev, s->sdr_id))
                continue;                           /* that is where we are */
            s->search_at = now + 6000;
            return ws_text(s, "{\"type\":\"setsdr\",\"params\":{\"sdr\":\"%s\"}}", dev);
        }
    }
    say(s, true, false, "%s: no device is on a profile with %.4f MHz now (\"%s\" is on %.3f MHz) — waiting; "
        "profiles are its listeners' to choose, this program never changes one",
        s->host, s->cfg.freq_hz / 1e6, owrx_profile_name(s), s->center / 1e6);
    s->n_tried = 0;
    s->search_at = now + 30000;
    return 0;
}

static int owrx_open(sdr_t *s)
{
    char path[200];
    size_t l = strlen(s->path);
    snprintf(path, sizeof path, "%s%sws/", s->path, l && s->path[l - 1] == '/' ? "" : "/");
    if (ws_handshake(s, path, NULL) != 0)
        return -1;
    memset(&s->adpcm, 0, sizeof s->adpcm);
    s->owrx_adpcm = false;
    s->center = s->span = 0;
    s->sdr_id[0] = s->profile_id[0] = '\0';
    s->n_prof = s->n_tried = 0;
    s->tuned = false;
    s->search_at = 0;
    if (ws_text(s, "SERVER DE CLIENT client=tailchaser type=receiver") != 0 ||
        ws_text(s, "{\"type\":\"connectionproperties\",\"params\":{\"output_rate\":12000,\"hd_output_rate\":48000}}") != 0)
        return -1;
    say(s, true, false, "connected to %s", s->host);
    return 0;
}

static int owrx_message(sdr_t *s, int op, uint8_t *p, size_t n)
{
    if (op == 2) {
        if (n > 1 && p[0] == 2 && s->tuned) {
            if (s->owrx_adpcm) {
                owrx_adpcm(s, p + 1, n - 1);
            } else {
                int16_t pcm[1024];
                size_t k = (n - 1) / 2, i = 0;
                while (i < k) {
                    int m = 0;
                    for (; m < 1024 && i < k; m++, i++)
                        pcm[m] = (int16_t)(p[1 + 2 * i] | (p[2 + 2 * i] << 8));
                    emit(s, pcm, m, 12000);
                }
            }
        }
        return 0;
    }
    const char *t = (const char *)p;
    if (!strncmp(t, "CLIENT DE SERVER", 16)) {
        const char *v = strstr(t, "version=v");
        s->adpcm.plain = v && (v[9] == '0' || !strncmp(v + 9, "1.0", 3) || !strncmp(v + 9, "1.1", 3));
        pthread_mutex_lock(&s->mu);
        snprintf(s->st.server, sizeof s->st.server, "%.90s", t + 17);
        pthread_mutex_unlock(&s->mu);
        return 0;
    }
    char type[32] = "";
    if (t[0] != '{' || !jstr(t, "type", type, sizeof type))
        return 0;
    if (!strcmp(type, "config")) {
        double v;
        char str[64];
        bool moved = false;
        if (jnum(t, "center_freq", &v)) { moved |= v != s->center; s->center = v; }
        if (jnum(t, "samp_rate", &v))   { moved |= v != s->span;   s->span = v; }
        if (jstr(t, "sdr_id", str, sizeof str))     snprintf(s->sdr_id, sizeof s->sdr_id, "%s", str);
        if (jstr(t, "profile_id", str, sizeof str)) snprintf(s->profile_id, sizeof s->profile_id, "%s", str);
        if (jstr(t, "audio_compression", str, sizeof str)) s->owrx_adpcm = !strcmp(str, "adpcm");
        if (s->center > 0 && s->span > 0 && (moved || !s->tuned)) {
            if (owrx_covers(s))
                return owrx_tune(s);
            if (s->tuned || !s->search_at)
                s->search_at = mono_ms() + (s->n_prof ? 0 : 1500);  /* the list of profiles follows */
            s->tuned = false;
        }
    } else if (!strcmp(type, "profiles")) {
        s->n_prof = 0;
        const char *q = t;
        while (s->n_prof < MAX_PROFILES && (q = strstr(q, "\"id\"")) != NULL) {
            const char *end = strchr(q, '}');
            char obj[256];
            snprintf(obj, sizeof obj, "{%.*s", (int)((end ? end : q + strlen(q)) - q), q);
            if (jstr(obj, "id", s->prof_id[s->n_prof], sizeof s->prof_id[0])) {
                if (!jstr(obj, "name", s->prof_name[s->n_prof], sizeof s->prof_name[0]))
                    s->prof_name[s->n_prof][0] = '\0';
                s->n_prof++;
            }
            q += 4;
        }
    } else if (!strcmp(type, "smeter")) {
        double v;
        if (jnum(t, "value", &v) && v > 0) {
            pthread_mutex_lock(&s->mu);
            s->st.smeter_db = (float)(10.0 * log10(v));
            pthread_mutex_unlock(&s->mu);
        }
    } else if (!strcmp(type, "backoff")) {
        char why[96] = "";
        jstr(t, "reason", why, sizeof why);
        say(s, false, false, "%s refuses: %s", s->host, why[0] ? why : "too many listeners");
        s->retry_s = 120;
        return -1;
    } else if (!strcmp(type, "sdr_error") || !strcmp(type, "demodulator_error")) {
        char why[120] = "";
        jstr(t, "value", why, sizeof why);
        log_warn("sdr: %s says: %s", s->host, why);
    }
    return 0;
}

static int owrx_tick(sdr_t *s, uint64_t now)
{
    if (s->search_at && now >= s->search_at) {
        if (owrx_covers(s))
            return owrx_tune(s);
        return owrx_search(s, now);
    }
    return 0;
}

static const sdr_backend_t BE_OPENWEBRX = { "openwebrx", owrx_open, owrx_message, owrx_tick };

/* ------------------------------------------------------------------ */
/* KiwiSDR, Web-888, UberSDR                                           */

#include "sdr_kiwi.inc"
#include "sdr_uber.inc"

/* ------------------------------------------------------------------ */

static const sdr_backend_t *backend_of(const char *type)
{
    if (!strcasecmp(type, "openwebrx")) return &BE_OPENWEBRX;
    if (!strcasecmp(type, "kiwisdr"))   return &BE_KIWI;
    if (!strcasecmp(type, "web888"))    return &BE_KIWI;
    if (!strcasecmp(type, "ubersdr"))   return &BE_UBER;
    return NULL;
}

static int parse_url(sdr_t *s, const char *url)
{
    const char *p = strstr(url, "://");
    s->tls = false;
    if (p) {
        s->tls = !strncasecmp(url, "https", 5) || !strncasecmp(url, "wss", 3);
        p += 3;
    } else {
        p = url;
    }
    size_t hl = strcspn(p, ":/");
    if (hl == 0 || hl >= sizeof s->host)
        return -1;
    snprintf(s->host, sizeof s->host, "%.*s", (int)hl, p);
    p += hl;
    s->port = s->tls ? 443 : 80;
    if (*p == ':') {
        s->port = atoi(p + 1);
        p += strcspn(p, "/");
    }
    snprintf(s->path, sizeof s->path, "%s", *p ? p : "/");
    return s->port > 0 && s->port < 65536 ? 0 : -1;
}

static void pause_ms(sdr_t *s, int ms)
{
    for (uint64_t until = mono_ms() + (uint64_t)ms; mono_ms() < until && !atomic_load(&s->stop);)
        usleep(100000);
}

#include "sdr_local.inc"

static void *sdr_main(void *arg)
{
    sdr_t *s = arg;
    int fails = 0;
    while (!atomic_load(&s->stop)) {
        s->retry_s = 0;
        say(s, false, false, "connecting to %s (%s)", s->host, s->be->name);
        bool up = io_connect(s, s->host, s->port, s->tls) == 0 && s->be->open(s) == 0;
        if (!up && !s->retry_s)
            say(s, false, false, "%s does not answer as a %s receiver — trying again", s->host, s->be->name);
        if (up) {
            pthread_mutex_lock(&s->mu);
            s->st.connects++;
            pthread_mutex_unlock(&s->mu);
            s->opened_at = s->last_audio = mono_ms();
            const char *why = "the connection was lost";
            while (!atomic_load(&s->stop)) {
                int op;
                uint8_t *p;
                size_t n;
                int rc = ws_recv(s, 200, &op, &p, &n);
                if (rc < 0 || (rc > 0 && s->be->message(s, op, p, n) != 0))
                    break;
                uint64_t now = mono_ms();
                if (s->be->tick(s, now) != 0)
                    break;
                pthread_mutex_lock(&s->mu);
                bool listening = s->st.listening;
                pthread_mutex_unlock(&s->mu);
                if (listening && now - s->last_audio > SILENT_MS) {
                    why = "no audio came any more";
                    break;
                }
            }
            if (!atomic_load(&s->stop) && !s->retry_s)
                say(s, false, false, "%s: %s — connecting again", s->host, why);
            if (mono_ms() - s->opened_at > 60000)
                fails = 0;
        }
        io_close(s);
        pthread_mutex_lock(&s->mu);
        s->st.connected = s->st.listening = false;
        pthread_mutex_unlock(&s->mu);
        if (++fails > 6) fails = 6;
        pause_ms(s, 1000 * (s->retry_s ? s->retry_s : 5 * fails));
    }
    return NULL;
}

sdr_t *sdr_start(const sdr_config_t *cfg, int out_rate)
{
    sdr_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->cfg = *cfg;
    s->out_rate = out_rate;
    s->fd = -1;
    bool local = sdr_is_local(cfg->type);
    s->be = local ? NULL : backend_of(cfg->type);
    if (!local && !s->be) {
        log_error("[sdr] type = %s: openwebrx, kiwisdr, web888 or ubersdr", cfg->type);
        free(s);
        return NULL;
    }
    if (!local && parse_url(s, cfg->url) != 0) {
        log_error("[sdr] url = %s: not an address", cfg->url);
        free(s);
        return NULL;
    }
    s->ring_cap = RING_MS * out_rate / 1000;
    s->ring = malloc(sizeof(int16_t) * (size_t)s->ring_cap);
    if (!s->ring) {
        free(s);
        return NULL;
    }
    pthread_mutex_init(&s->mu, NULL);
    snprintf(s->st.state, sizeof s->st.state, "starting");
    if (pthread_create(&s->thread, NULL, local ? local_main : sdr_main, s) != 0) {
        free(s->ring);
        free(s);
        return NULL;
    }
    s->running = true;
    return s;
}

void sdr_stop(sdr_t *s)
{
    if (!s)
        return;
    atomic_store(&s->stop, true);
    if (s->running)
        pthread_join(s->thread, NULL);
    io_close(s);
    if (s->ctx) SSL_CTX_free(s->ctx);
    if (s->opus) opus_decoder_destroy(s->opus);
    free(s->in);
    free(s->frag);
    free(s->ring);
    pthread_mutex_destroy(&s->mu);
    free(s);
}
