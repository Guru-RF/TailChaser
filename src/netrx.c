/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "netrx.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <opus.h>

/* ---- message types (svxlink/trx/NetTrxMsg.h; see nettrx.c) ---- */
enum {
    MSG_PROTO_VER            = 0,
    MSG_HEARTBEAT            = 1,
    MSG_AUTH_CHALLENGE       = 10,
    MSG_AUTH_RESPONSE        = 11,
    MSG_AUTH_OK              = 12,
    MSG_RX_AUDIO_CODEC_SEL   = 100,
    MSG_AUDIO                = 102,
    MSG_SET_MUTE_STATE       = 200,
    MSG_SQUELCH              = 250,
    MSG_DTMF                 = 251,
    MSG_SIGLEV_UPDATE        = 254,
};

#define HDR_LEN          8
#define CHALLENGE_LEN    20
#define DIGEST_LEN       20
#define MAX_MSG_LEN      4096
#define RXBUF            (4 * MAX_MSG_LEN)
#define TXBUF            2048
#define CODEC_SEL_LEN    (32 + 1 + 256)     /* name, option count, options */

#define HEARTBEAT_TX_MS  10000
#define HEARTBEAT_RX_MS  15000
#define CONNECT_MS       5000               /* a connection attempt may take this long */
#define RETRY_MIN_MS     1000
#define RETRY_MAX_MS     10000

enum { ST_IDLE = 0, ST_CONNECTING, ST_VERSION, ST_AUTH, ST_READY };
enum { CODEC_S16 = 0, CODEC_RAW, CODEC_OPUS };

/* A name being looked up: on a thread of its own, because a resolver that
 * does not answer must not hold up the loop that runs the transmitter. */
typedef struct {
    pthread_mutex_t mu;
    bool   done, orphan;
    int    gai;
    struct addrinfo *res;
    char   host[256], port[16];
} lookup_t;

static void *lookup_run(void *arg)
{
    lookup_t *l = arg;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(l->host, l->port, &hints, &res);
    pthread_mutex_lock(&l->mu);
    bool orphan = l->orphan;
    l->gai = gai;
    l->res = res;
    l->done = true;
    pthread_mutex_unlock(&l->mu);
    if (orphan) {                       /* nobody is waiting any more */
        if (res) freeaddrinfo(res);
        pthread_mutex_destroy(&l->mu);
        free(l);
    }
    return NULL;
}

struct netrx {
    lookup_t *lookup;
    bool challenged;            /* the receiver asked for the key before saying OK */
    netrx_cfg_t  cfg;
    netrx_sink_t sink;
    int   fd, state, codec;
    uint64_t now_ms, last_rx_ms, last_hb_ms, state_since_ms, retry_at_ms;
    int   retry_ms;
    bool  warned;               /* the "cannot reach" line was logged for this outage */

    uint8_t rx[RXBUF];
    int     rx_len;
    uint8_t tx[TXBUF];
    int     tx_len;

    OpusDecoder *opus;
    netrx_status_t st;
};

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static float getf(const uint8_t *p)
{
    uint32_t u = get32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* A signal level from the network: a number between 0 and 100, whatever was sent. */
static float level(float v)
{
    return !isfinite(v) ? 0.0f : v < 0.0f ? 0.0f : v > 100.0f ? 100.0f : v;
}

/* ---- connection ---- */

static void drop(netrx_t *n, const char *why)
{
    if (n->fd < 0)
        return;
    bool was_ready = n->state == ST_READY;
    if (was_ready || !n->warned)
        log_warn("receiver: %s %s (%s) — trying again", was_ready ? "lost" : "cannot reach",
                 n->st.peer, why);
    n->warned = true;
    close(n->fd);
    n->fd = -1;
    n->state = ST_IDLE;
    n->rx_len = n->tx_len = 0;
    n->st.connected = false;
    /* The receiver can no longer say that its squelch closed. */
    if (n->st.squelch_open) {
        n->st.squelch_open = false;
        if (n->sink.squelch)
            n->sink.squelch(n->sink.ctx, false, n->st.siglev);
    }
    n->retry_at_ms = n->now_ms + (uint64_t)n->retry_ms;
    n->retry_ms = n->retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : n->retry_ms * 2;
}

static void flush_tx(netrx_t *n)
{
    while (n->fd >= 0 && n->tx_len > 0) {
        ssize_t w = send(n->fd, n->tx, (size_t)n->tx_len, MSG_NOSIGNAL);
        if (w > 0) {
            memmove(n->tx, n->tx + w, (size_t)(n->tx_len - (int)w));
            n->tx_len -= (int)w;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else {
            drop(n, w < 0 ? strerror(errno) : "send returned 0");
            return;
        }
    }
}

static void send_msg(netrx_t *n, uint32_t type, const void *payload, int len)
{
    if (n->fd < 0)
        return;
    if (n->tx_len + HDR_LEN + len > TXBUF) {
        drop(n, "the receiver is not reading");
        return;
    }
    uint8_t *p = n->tx + n->tx_len;
    put32(p, type);
    put32(p + 4, (uint32_t)(HDR_LEN + len));
    if (len > 0)
        memcpy(p + HDR_LEN, payload, (size_t)len);
    n->tx_len += HDR_LEN + len;
    flush_tx(n);
}

static void start_connect(netrx_t *n)
{
    if (!n->lookup) {
        lookup_t *l = calloc(1, sizeof *l);
        if (!l)
            return;
        pthread_mutex_init(&l->mu, NULL);
        snprintf(l->host, sizeof l->host, "%s", n->cfg.host);
        snprintf(l->port, sizeof l->port, "%d", n->cfg.port);
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        int rc = pthread_create(&th, &at, lookup_run, l);
        pthread_attr_destroy(&at);
        if (rc != 0) {
            pthread_mutex_destroy(&l->mu);
            free(l);
            n->retry_at_ms = n->now_ms + (uint64_t)n->retry_ms;
            return;
        }
        n->lookup = l;
        return;
    }
    lookup_t *l = n->lookup;
    pthread_mutex_lock(&l->mu);
    bool done = l->done;
    pthread_mutex_unlock(&l->mu);
    if (!done)
        return;
    struct addrinfo *res = l->res;
    int gai = l->gai;
    pthread_mutex_destroy(&l->mu);
    free(l);
    n->lookup = NULL;
    n->retry_at_ms = n->now_ms + (uint64_t)n->retry_ms;
    if (gai != 0 || !res) {
        if (!n->warned)
            log_warn("receiver: cannot resolve %s: %s — trying again", n->cfg.host, gai_strerror(gai));
        n->warned = true;
        n->retry_ms = n->retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : n->retry_ms * 2;
        return;
    }
    int fd = socket(res->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        freeaddrinfo(res);
        return;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0 && errno != EINPROGRESS) {
        n->fd = fd;
        n->state = ST_CONNECTING;
        drop(n, strerror(errno));
        return;
    }
    n->fd = fd;
    n->state = ST_CONNECTING;
    n->state_since_ms = n->last_rx_ms = n->last_hb_ms = n->now_ms;
    n->rx_len = n->tx_len = 0;
}

/* Logged in: say which codec we want and that we want to hear everything. */
static void go_ready(netrx_t *n)
{
    n->state = ST_READY;
    n->st.connected = true;
    n->st.connects++;
    n->retry_ms = RETRY_MIN_MS;
    n->warned = false;
    log_info("receiver: connected to %s (RemoteTrx protocol %d.%d, audio %s)", n->st.peer,
             n->st.proto_major, n->st.proto_minor, n->cfg.codec);

    uint8_t sel[CODEC_SEL_LEN];
    memset(sel, 0, sizeof sel);
    snprintf((char *)sel, 32, "%s", n->cfg.codec);
    send_msg(n, MSG_RX_AUDIO_CODEC_SEL, sel, sizeof sel);
    uint8_t mute[4];
    put32(mute, 0);                             /* MUTE_NONE */
    send_msg(n, MSG_SET_MUTE_STATE, mute, 4);
}

/* ---- incoming ---- */

static void on_audio(netrx_t *n, const uint8_t *pl, int len)
{
    if (len < 4 || !n->sink.audio)
        return;
    int size = (int)get32(pl);
    const uint8_t *d = pl + 4;
    if (size <= 0 || size > len - 4)
        return;
    int16_t pcm[2880];
    int cnt = 0;
    switch (n->codec) {
    case CODEC_S16:
        cnt = size / 2;
        if (cnt > 2880) cnt = 2880;
        for (int i = 0; i < cnt; i++)
            pcm[i] = (int16_t)((uint16_t)d[2 * i] | (uint16_t)d[2 * i + 1] << 8);
        break;
    case CODEC_RAW:
        cnt = size / 4;
        if (cnt > 2880) cnt = 2880;
        for (int i = 0; i < cnt; i++) {
            float v = getf(d + 4 * i) * 32767.0f;
            pcm[i] = !isfinite(v) ? 0 : v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int16_t)v;
        }
        break;
    case CODEC_OPUS:
        if (n->opus)
            cnt = opus_decode(n->opus, d, size, pcm, 2880, 0);
        break;
    }
    if (cnt > 0) {
        n->st.audio_frames++;
        n->sink.audio(n->sink.ctx, pcm, cnt);
    }
}

static void handle_msg(netrx_t *n, uint32_t type, const uint8_t *pl, int len)
{
    switch (n->state) {
    case ST_VERSION:
        if (type != MSG_PROTO_VER || len < 4) {
            drop(n, "that is not a RemoteTrx receiver");
            return;
        }
        n->st.proto_major = pl[0] | pl[1] << 8;
        n->st.proto_minor = pl[2] | pl[3] << 8;
        if (n->st.proto_major != 2) {
            drop(n, "unknown RemoteTrx protocol version");
            return;
        }
        n->state = ST_AUTH;
        n->challenged = false;
        return;
    case ST_AUTH:
        if (type == MSG_AUTH_CHALLENGE && len == CHALLENGE_LEN) {
            unsigned char mac[EVP_MAX_MD_SIZE];
            unsigned int maclen = 0;
            HMAC(EVP_sha1(), n->cfg.auth_key, (int)strlen(n->cfg.auth_key), pl, CHALLENGE_LEN,
                 mac, &maclen);
            send_msg(n, MSG_AUTH_RESPONSE, mac, DIGEST_LEN);
            n->challenged = true;
        } else if (type == MSG_AUTH_OK && (n->challenged || n->cfg.auth_key[0] == '\0')) {
            go_ready(n);
        } else {
            drop(n, "protocol error during login");
        }
        return;
    default:
        break;
    }

    switch (type) {
    case MSG_SQUELCH: {
        /* bool is_open; float signal_strength; char sql_rx_id; [info, 2.8] */
        if (len < 6) break;
        bool open = pl[0] != 0;
        n->st.siglev = level(getf(pl + 1));
        n->st.rx_id = (char)pl[5];
        if (open != n->st.squelch_open) {
            n->st.squelch_open = open;
            if (n->codec == CODEC_OPUS && n->opus && open)
                opus_decoder_ctl(n->opus, OPUS_RESET_STATE);
            if (n->sink.squelch)
                n->sink.squelch(n->sink.ctx, open, n->st.siglev);
        }
        break;
    }
    case MSG_SIGLEV_UPDATE:
        if (len < 5) break;
        n->st.siglev = level(getf(pl));
        if (n->sink.siglev)
            n->sink.siglev(n->sink.ctx, n->st.siglev);
        break;
    case MSG_DTMF:
        if (len < 5) break;
        if (n->sink.dtmf)
            n->sink.dtmf(n->sink.ctx, (char)pl[0], (int)get32(pl + 1));
        break;
    case MSG_AUDIO:
        on_audio(n, pl, len);
        break;
    default:                    /* heartbeat, AllSamplesFlushed, tones: nothing to do */
        break;
    }
}

void netrx_on_readable(netrx_t *n)
{
    if (n->fd < 0 || n->state == ST_CONNECTING)
        return;
    for (;;) {
        ssize_t r = recv(n->fd, n->rx + n->rx_len, sizeof n->rx - (size_t)n->rx_len, 0);
        if (r == 0) {
            if (n->state == ST_AUTH) {
                n->st.auth_failures++;
                if (!n->warned)
                    log_error("receiver: %s hung up during login — [receiver] auth_key does not "
                              "match its [uplink] auth_key", n->st.peer);
            }
            drop(n, "closed by the receiver");
            return;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                drop(n, strerror(errno));
            return;
        }
        n->rx_len += (int)r;
        n->last_rx_ms = n->now_ms;

        int off = 0;
        while (n->fd >= 0 && n->rx_len - off >= HDR_LEN) {
            uint32_t type = get32(n->rx + off);
            uint32_t size = get32(n->rx + off + 4);
            if (size < HDR_LEN || size > MAX_MSG_LEN) {
                drop(n, "illegal message length");
                return;
            }
            if ((uint32_t)(n->rx_len - off) < size)
                break;
            handle_msg(n, type, n->rx + off + HDR_LEN, (int)size - HDR_LEN);
            off += (int)size;
        }
        if (n->fd < 0)
            return;
        memmove(n->rx, n->rx + off, (size_t)(n->rx_len - off));
        n->rx_len -= off;
    }
}

void netrx_on_writable(netrx_t *n)
{
    if (n->fd < 0)
        return;
    if (n->state == ST_CONNECTING) {
        int err = 0;
        socklen_t sl = sizeof err;
        if (getsockopt(n->fd, SOL_SOCKET, SO_ERROR, &err, &sl) != 0 || err != 0) {
            drop(n, strerror(err ? err : errno));
            return;
        }
        n->state = ST_VERSION;
        n->state_since_ms = n->last_rx_ms = n->now_ms;
        return;
    }
    flush_tx(n);
}

void netrx_tick(netrx_t *n, uint64_t now_ms)
{
    n->now_ms = now_ms;
    if (n->fd < 0) {
        if (n->lookup || now_ms >= n->retry_at_ms)
            start_connect(n);
        return;
    }
    if (n->state == ST_CONNECTING) {
        if (now_ms - n->state_since_ms > CONNECT_MS)
            drop(n, "no answer");
        return;
    }
    if (now_ms - n->last_rx_ms > HEARTBEAT_RX_MS) {
        drop(n, n->state == ST_READY ? "heartbeat time-out" : "login time-out");
        return;
    }
    if (n->state == ST_READY && now_ms - n->last_hb_ms >= HEARTBEAT_TX_MS) {
        n->last_hb_ms = now_ms;
        send_msg(n, MSG_HEARTBEAT, NULL, 0);
    }
}

/* ---- lifecycle ---- */

netrx_t *netrx_new(const netrx_cfg_t *cfg, const netrx_sink_t *sink)
{
    netrx_t *n = calloc(1, sizeof *n);
    if (!n)
        return NULL;
    n->cfg = *cfg;
    n->sink = *sink;
    n->fd = -1;
    n->retry_ms = RETRY_MIN_MS;
    n->st.rx_id = '?';
    snprintf(n->st.peer, sizeof n->st.peer, "%s:%d", cfg->host, cfg->port);
    snprintf(n->st.codec, sizeof n->st.codec, "%s", cfg->codec);
    if (!strcmp(cfg->codec, "RAW"))       n->codec = CODEC_RAW;
    else if (!strcmp(cfg->codec, "OPUS")) n->codec = CODEC_OPUS;
    else                                  n->codec = CODEC_S16;
    if (n->codec == CODEC_OPUS) {
        int err = 0;
        n->opus = opus_decoder_create(cfg->sample_rate, 1, &err);
        if (!n->opus || err != OPUS_OK) {
            log_error("receiver: opus decoder: %s", opus_strerror(err));
            free(n);
            return NULL;
        }
    }
    log_info("receiver: %s (audio %s)", n->st.peer, cfg->codec);
    return n;
}

void netrx_free(netrx_t *n)
{
    if (!n)
        return;
    if (n->fd >= 0) close(n->fd);
    if (n->lookup) {
        lookup_t *l = n->lookup;
        pthread_mutex_lock(&l->mu);
        bool done = l->done;
        l->orphan = true;                   /* still running: it cleans up after itself */
        pthread_mutex_unlock(&l->mu);
        if (done) {
            if (l->res) freeaddrinfo(l->res);
            pthread_mutex_destroy(&l->mu);
            free(l);
        }
    }
    if (n->opus) opus_decoder_destroy(n->opus);
    free(n);
}

int  netrx_fd(const netrx_t *n) { return n->fd; }
bool netrx_want_write(const netrx_t *n)
{
    return n->fd >= 0 && (n->state == ST_CONNECTING || n->tx_len > 0);
}
void netrx_get_status(const netrx_t *n, netrx_status_t *out) { *out = n->st; }
