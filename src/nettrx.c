/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "nettrx.h"
#include "log.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <opus.h>

/* ---- message types (svxlink/trx/NetTrxMsg.h) ---- */
enum {
    MSG_PROTO_VER            = 0,
    MSG_HEARTBEAT            = 1,
    MSG_AUTH_CHALLENGE       = 10,
    MSG_AUTH_RESPONSE        = 11,
    MSG_AUTH_OK              = 12,
    MSG_RX_AUDIO_CODEC_SEL   = 100,
    MSG_TX_AUDIO_CODEC_SEL   = 101,
    MSG_AUDIO                = 102,
    MSG_SET_MUTE_STATE       = 200,
    MSG_ADD_TONE_DETECTOR    = 201,
    MSG_RESET                = 202,
    MSG_SET_RX_FQ            = 203,
    MSG_SET_RX_MODULATION    = 204,
    MSG_SQUELCH              = 250,
    MSG_DTMF                 = 251,
    MSG_TONE                 = 252,
    MSG_SEL5                 = 253,
    MSG_SIGLEV_UPDATE        = 254,
    MSG_FLUSH                = 303,
    MSG_ALL_SAMPLES_FLUSHED  = 352,
};

#define HDR_LEN          8
#define CHALLENGE_LEN    20
#define DIGEST_LEN       20
#define AUTH_DEADLINE_MS 10000   /* from the connection to the login, whatever is sent */
#define AUDIO_BUFSIZE    (4 * 512)      /* MsgAudio::BUFSIZE = sizeof(float)*512 */
#define MAX_MSG_LEN      4096
#define ACTIVITY_INFO_LEN 128           /* proto >= 2.8: MAX_ACTIVITY_INFO_LEN+1 */

#define HEARTBEAT_TX_MS  10000
#define HEARTBEAT_RX_MS  15000
#define TXBUF_CAP        (512 * 1024)   /* ~8 s of RAW audio: a stalled peer is dropped */

enum { MUTE_NONE = 0, MUTE_CONTENT = 1, MUTE_ALL = 2 };
enum { ST_DISC = 0, ST_AUTH, ST_READY };
enum { CODEC_NONE = 0, CODEC_RAW, CODEC_S16, CODEC_OPUS, CODEC_NULL };

struct nettrx {
    int   minor;            /* protocol minor version offered right now */
    bool  auto_minor;       /* cfg.proto_minor == 0: find it out         */
    bool  peer_hung_up;
    nettrx_cfg_t cfg;
    int   lfd, cfd, state;
    float gain;

    uint8_t  rx[MAX_MSG_LEN];
    int      rx_len;
    uint8_t *tx;
    int      tx_len;

    uint8_t  challenge[CHALLENGE_LEN];
    uint64_t now_ms, last_rx_ms, last_hb_ms, accept_ms;

    int      mute;
    int      codec;
    OpusEncoder *opus;
    int      opus_frame;        /* samples per Opus frame */
    float    opus_buf[2880];
    int      opus_fill;

    bool     sql_open;          /* what the receiver last reported */
    float    sql_siglev;
    bool     sql_sent_open;     /* what the client was last told   */

    nettrx_status_t st;
};

/* ---- little-endian field helpers ---- */
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void putf(uint8_t *p, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    put32(p, u);
}

/* ---- connection ---- */

static void drop_client(nettrx_t *t, const char *why)
{
    if (t->cfd < 0)
        return;
    log_info("uplink: client %s disconnected (%s)", t->st.peer, why);
    /* A repeater that hangs up before it has logged in did not like our
     * protocol version (it must match exactly). On "auto", offer the other one
     * to its next attempt. */
    if (t->auto_minor && t->state != ST_READY && t->state != ST_DISC && t->peer_hung_up) {
        t->minor = t->minor == 8 ? 7 : 8;
        log_info("uplink: the repeater hung up before logging in — offering RemoteTrx protocol %d.%d next",
                 t->cfg.proto_major, t->minor);
    }
    t->peer_hung_up = false;
    close(t->cfd);
    t->cfd = -1;
    t->state = ST_DISC;
    t->rx_len = t->tx_len = 0;
    t->codec = CODEC_NONE;
    t->mute = MUTE_ALL;
    t->sql_sent_open = false;
    t->st.connected = false;
    t->st.codec[0] = '\0';
    t->st.mute_state = MUTE_ALL;
}

static void flush_tx(nettrx_t *t)
{
    while (t->cfd >= 0 && t->tx_len > 0) {
        ssize_t w = send(t->cfd, t->tx, (size_t)t->tx_len, MSG_NOSIGNAL);
        if (w > 0) {
            memmove(t->tx, t->tx + w, (size_t)(t->tx_len - (int)w));
            t->tx_len -= (int)w;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else {
            drop_client(t, w < 0 ? strerror(errno) : "send returned 0");
            return;
        }
    }
}

/* Queue one message: header + payload. */
static void send_msg(nettrx_t *t, uint32_t type, const void *payload, int len)
{
    if (t->cfd < 0)
        return;
    if (t->tx_len + HDR_LEN + len > TXBUF_CAP) {
        drop_client(t, "send buffer overflow — the repeater is not reading");
        return;
    }
    uint8_t *p = t->tx + t->tx_len;
    put32(p, type);
    put32(p + 4, (uint32_t)(HDR_LEN + len));
    if (len > 0)
        memcpy(p + HDR_LEN, payload, (size_t)len);
    t->tx_len += HDR_LEN + len;
    flush_tx(t);
}

static void send_squelch(nettrx_t *t, bool open, float siglev)
{
    /* bool is_open; float signal_strength; char sql_rx_id; [char info[128] if >= 2.8] */
    uint8_t b[6 + ACTIVITY_INFO_LEN];
    memset(b, 0, sizeof b);
    b[0] = open ? 1 : 0;
    putf(b + 1, siglev);
    b[5] = (uint8_t)t->cfg.rx_id;
    bool v28 = t->cfg.proto_major > 2 || (t->cfg.proto_major == 2 && t->minor >= 8);
    send_msg(t, MSG_SQUELCH, b, v28 ? 6 + ACTIVITY_INFO_LEN : 6);
    t->sql_sent_open = open;
}

/* ---- codec ---- */

static void set_codec(nettrx_t *t, const uint8_t *pl, int len)
{
    /* char name[32]; uint8 option_cnt; char options[256] (len-prefixed name/value pairs) */
    if (len < 32 + 1) {
        log_warn("uplink: short codec-select message ignored");
        return;
    }
    char name[33];
    memcpy(name, pl, 32);
    name[32] = '\0';
    int cnt = pl[32];
    const uint8_t *o = pl + 33, *end = pl + len;

    t->opus_fill = 0;
    if (!strcmp(name, "RAW"))        t->codec = CODEC_RAW;
    else if (!strcmp(name, "S16"))   t->codec = CODEC_S16;
    else if (!strcmp(name, "OPUS"))  t->codec = CODEC_OPUS;
    else if (!strcmp(name, "NULL") || !strcmp(name, "DUMMY")) t->codec = CODEC_NULL;
    else {
        t->codec = CODEC_NONE;
        log_error("uplink: the repeater asked for RX audio codec \"%s\", which this receiver "
                  "does not have (RAW, S16, OPUS). No audio will be sent — set CODEC=RAW, "
                  "S16 or OPUS in the repeater's Net receiver section.", name);
        snprintf(t->st.codec, sizeof t->st.codec, "%.10s?", name);
        return;
    }
    snprintf(t->st.codec, sizeof t->st.codec, "%.15s", name);
    log_info("uplink: using codec \"%s\" for RX audio", name);

    if (t->codec != CODEC_OPUS)
        return;

    /* SvxLink's AudioEncoderOpus defaults. */
    if (!t->opus) {
        int err = 0;
        t->opus = opus_encoder_create(t->cfg.sample_rate, 1, OPUS_APPLICATION_AUDIO, &err);
        if (!t->opus || err != OPUS_OK) {
            log_error("uplink: opus encoder: %s", opus_strerror(err));
            t->opus = NULL;
            t->codec = CODEC_NONE;
            return;
        }
    } else {
        opus_encoder_ctl(t->opus, OPUS_RESET_STATE);
    }
    float frame_ms = 20.0f;
    opus_encoder_ctl(t->opus, OPUS_SET_BITRATE(20000));
    opus_encoder_ctl(t->opus, OPUS_SET_VBR(1));
    opus_encoder_ctl(t->opus, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_MEDIUMBAND));
    opus_encoder_ctl(t->opus, OPUS_SET_BANDWIDTH(OPUS_AUTO));
    opus_encoder_ctl(t->opus, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(t->opus, OPUS_SET_DTX(0));
    opus_encoder_ctl(t->opus, OPUS_SET_LSB_DEPTH(16));

    for (int i = 0; i < cnt && o < end; i++) {
        char on[64], ov[64];
        int nl = *o++;
        if (o + nl >= end) break;
        snprintf(on, sizeof on, "%.*s", nl < 63 ? nl : 63, (const char *)o);
        o += nl;
        int vl = *o++;
        if (o + vl > end) break;
        snprintf(ov, sizeof ov, "%.*s", vl < 63 ? vl : 63, (const char *)o);
        o += vl;
        if (!strcmp(on, "FRAME_SIZE"))      frame_ms = (float)atof(ov);
        else if (!strcmp(on, "COMPLEXITY")) opus_encoder_ctl(t->opus, OPUS_SET_COMPLEXITY(atoi(ov)));
        else if (!strcmp(on, "BITRATE"))    opus_encoder_ctl(t->opus, OPUS_SET_BITRATE(atoi(ov)));
        else if (!strcmp(on, "VBR"))        opus_encoder_ctl(t->opus, OPUS_SET_VBR(atoi(ov) != 0));
        else if (!strcmp(on, "CVBR"))       opus_encoder_ctl(t->opus, OPUS_SET_VBR_CONSTRAINT(atoi(ov) != 0));
        else log_warn("uplink: unknown Opus option %s=%s ignored", on, ov);
    }
    /* Opus accepts 2.5/5/10/20/40/60 ms frames only. */
    static const float ok[] = { 2.5f, 5.0f, 10.0f, 20.0f, 40.0f, 60.0f };
    float pick = 20.0f;
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++)
        if (fabsf(ok[i] - frame_ms) < 0.01f) pick = ok[i];
    t->opus_frame = (int)(pick * (float)t->cfg.sample_rate / 1000.0f);
    log_info("uplink: opus frame %.1f ms", pick);
}

/* ---- incoming messages ---- */

static void handle_msg(nettrx_t *t, uint32_t type, const uint8_t *pl, int len)
{
    if (t->state == ST_AUTH) {
        if (type != MSG_AUTH_RESPONSE || len != DIGEST_LEN) {
            drop_client(t, "protocol error during authentication");
            return;
        }
        unsigned char mac[EVP_MAX_MD_SIZE];
        unsigned int maclen = 0;
        HMAC(EVP_sha1(), t->cfg.auth_key, (int)strlen(t->cfg.auth_key),
             t->challenge, CHALLENGE_LEN, mac, &maclen);
        if (maclen != DIGEST_LEN || ct_memcmp(mac, pl, DIGEST_LEN) != 0) {
            t->st.auth_failures++;
            log_error("uplink: authentication FAILED for %s — auth_key here does not match "
                      "AUTH_KEY on the repeater", t->st.peer);
            drop_client(t, "bad auth key");
            return;
        }
        send_msg(t, MSG_AUTH_OK, NULL, 0);
        t->state = ST_READY;
        t->st.connected = true;
        t->st.connected_since_ms = t->now_ms;
        t->st.connects++;
        log_info("uplink: repeater %s authenticated", t->st.peer);
        return;
    }

    switch (type) {
    case MSG_HEARTBEAT:
        break;
    case MSG_RX_AUDIO_CODEC_SEL:
        set_codec(t, pl, len);
        break;
    case MSG_SET_MUTE_STATE: {
        if (len < 4) break;
        int m = (int)get32(pl);
        if (m < MUTE_NONE || m > MUTE_ALL) break;
        static const char *const names[] = { "NONE", "CONTENT", "ALL" };
        log_info("uplink: SetMuteState(%s)", names[m]);
        int was = t->mute;
        t->mute = m;
        t->st.mute_state = m;
        /* Coming out of MUTE_ALL the repeater has forgotten our squelch state. */
        if (was == MUTE_ALL && m != MUTE_ALL && t->sql_open)
            send_squelch(t, true, t->sql_siglev);
        if (m == MUTE_ALL)
            t->sql_sent_open = false;
        break;
    }
    case MSG_FLUSH:
        send_msg(t, MSG_ALL_SAMPLES_FLUSHED, NULL, 0);
        break;
    case MSG_ADD_TONE_DETECTOR:
        log_info("uplink: the repeater asked for a tone detector — not supported, ignored");
        break;
    case MSG_SET_RX_FQ:
        if (len >= 4)
            log_info("uplink: the repeater asked for RX frequency %u Hz — ignored, the "
                     "frequency is set in [radio]", get32(pl));
        break;
    default:
        /* MsgReset, RX modulation, and everything transmitter-side: nothing to do. */
        break;
    }
}

void nettrx_on_readable(nettrx_t *t)
{
    if (t->cfd < 0)
        return;
    for (;;) {
        ssize_t r = recv(t->cfd, t->rx + t->rx_len, sizeof t->rx - (size_t)t->rx_len, 0);
        if (r == 0) { t->peer_hung_up = true; drop_client(t, "closed by the repeater"); return; }
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                drop_client(t, strerror(errno));
            return;
        }
        t->rx_len += (int)r;
        t->last_rx_ms = t->now_ms;

        int off = 0;
        while (t->cfd >= 0 && t->rx_len - off >= HDR_LEN) {
            uint32_t type = get32(t->rx + off);
            uint32_t size = get32(t->rx + off + 4);
            if (size < HDR_LEN || size > MAX_MSG_LEN) {
                drop_client(t, "illegal message length");
                return;
            }
            if ((uint32_t)(t->rx_len - off) < size)
                break;
            handle_msg(t, type, t->rx + off + HDR_LEN, (int)size - HDR_LEN);
            off += (int)size;
        }
        if (t->cfd < 0)
            return;
        memmove(t->rx, t->rx + off, (size_t)(t->rx_len - off));
        t->rx_len -= off;
    }
}

void nettrx_on_writable(nettrx_t *t) { flush_tx(t); }

void nettrx_on_accept(nettrx_t *t)
{
    struct sockaddr_in6 sa;
    socklen_t sl = sizeof sa;
    int fd = accept4(t->lfd, (struct sockaddr *)&sa, &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0)
        return;
    char ip[INET6_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET6, &sa.sin6_addr, ip, sizeof ip);
    const char *shown = !strncmp(ip, "::ffff:", 7) ? ip + 7 : ip;
    if (t->cfd >= 0 && t->state != ST_READY) {
        /* whoever holds the place without having logged in makes way */
        drop_client(t, "replaced by a new connection before it had logged in");
    }
    if (t->cfd >= 0) {
        log_warn("uplink: second connection from %s refused — only one repeater at a time "
                 "(%s is connected)", shown, t->st.peer);
        close(fd);
        return;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    t->cfd = fd;
    t->rx_len = t->tx_len = 0;
    t->mute = MUTE_ALL;
    t->st.mute_state = MUTE_ALL;
    t->codec = CODEC_NONE;
    t->sql_sent_open = false;
    t->last_rx_ms = t->last_hb_ms = t->accept_ms = t->now_ms;
    snprintf(t->st.peer, sizeof t->st.peer, "%s:%u", shown, ntohs(sa.sin6_port));
    log_info("uplink: client connected: %s", t->st.peer);

    uint8_t ver[4] = { (uint8_t)t->cfg.proto_major, (uint8_t)(t->cfg.proto_major >> 8),
                       (uint8_t)t->minor, (uint8_t)(t->minor >> 8) };
    t->state = ST_AUTH;
    send_msg(t, MSG_PROTO_VER, ver, 4);
    if (t->cfg.auth_key[0] == '\0') {
        send_msg(t, MSG_AUTH_OK, NULL, 0);
        t->state = ST_READY;
        t->st.connected = true;
        t->st.connected_since_ms = t->now_ms;
        t->st.connects++;
    } else {
        if (RAND_bytes(t->challenge, CHALLENGE_LEN) != 1) {
            drop_client(t, "no random numbers available");
            return;
        }
        send_msg(t, MSG_AUTH_CHALLENGE, t->challenge, CHALLENGE_LEN);
    }
}

void nettrx_tick(nettrx_t *t, uint64_t now_ms)
{
    t->now_ms = now_ms;
    if (t->cfd < 0)
        return;
    if (t->state != ST_READY && now_ms - t->accept_ms > AUTH_DEADLINE_MS) {
        drop_client(t, "no login within the time allowed");
        return;
    }
    if (now_ms - t->last_rx_ms > HEARTBEAT_RX_MS) {
        drop_client(t, t->state == ST_READY ? "heartbeat timeout" : "authentication timeout");
        return;
    }
    if (t->state == ST_READY && now_ms - t->last_hb_ms >= HEARTBEAT_TX_MS) {
        t->last_hb_ms = now_ms;
        send_msg(t, MSG_HEARTBEAT, NULL, 0);
    }
}

/* ---- receiver -> repeater ---- */

void nettrx_squelch(nettrx_t *t, bool open, float siglev)
{
    t->sql_open = open;
    t->sql_siglev = siglev;
    if (!open)
        t->opus_fill = 0;
    if (t->state != ST_READY || t->mute == MUTE_ALL)
        return;
    if (!open && !t->sql_sent_open)
        return;
    send_squelch(t, open, siglev);
}

void nettrx_siglev(nettrx_t *t, float siglev)
{
    t->sql_siglev = siglev;
    if (t->state != ST_READY || t->mute == MUTE_ALL || !t->sql_sent_open)
        return;
    uint8_t b[5];
    putf(b, siglev);
    b[4] = (uint8_t)t->cfg.rx_id;
    send_msg(t, MSG_SIGLEV_UPDATE, b, 5);
}

static void send_audio(nettrx_t *t, const void *data, int len)
{
    const uint8_t *p = data;
    while (len > 0 && t->cfd >= 0) {
        int n = len < AUDIO_BUFSIZE ? len : AUDIO_BUFSIZE;
        uint8_t b[4 + AUDIO_BUFSIZE];
        put32(b, (uint32_t)n);
        memcpy(b + 4, p, (size_t)n);
        send_msg(t, MSG_AUDIO, b, 4 + n);
        p += n;
        len -= n;
    }
    t->st.audio_frames_sent++;
}

void nettrx_audio(nettrx_t *t, const int16_t *pcm, int n)
{
    if (t->state != ST_READY || t->mute != MUTE_NONE || !t->sql_sent_open || n <= 0 || n > 2880)
        return;
    float g = t->gain;
    switch (t->codec) {
    case CODEC_RAW: {
        float f[2880];
        for (int i = 0; i < n; i++) f[i] = (float)pcm[i] * g / 32768.0f;
        send_audio(t, f, n * (int)sizeof(float));      /* native little-endian floats */
        break;
    }
    case CODEC_S16: {
        int16_t s[2880];
        for (int i = 0; i < n; i++) {
            float v = (float)pcm[i] * g;
            s[i] = v > 32767.0f ? 32767 : v < -32767.0f ? -32767 : (int16_t)v;
        }
        send_audio(t, s, n * (int)sizeof(int16_t));
        break;
    }
    case CODEC_OPUS:
        for (int i = 0; i < n; i++) {
            t->opus_buf[t->opus_fill++] = (float)pcm[i] * g / 32768.0f;
            if (t->opus_fill == t->opus_frame) {
                unsigned char out[1500];
                opus_int32 nb = opus_encode_float(t->opus, t->opus_buf, t->opus_frame, out, sizeof out);
                if (nb > 0)
                    send_audio(t, out, (int)nb);
                else if (nb < 0)
                    log_warn("uplink: opus encode: %s", opus_strerror(nb));
                t->opus_fill = 0;
            }
        }
        break;
    default:
        break;
    }
}

void nettrx_dtmf(nettrx_t *t, char digit, int duration_ms)
{
    if (t->state != ST_READY || t->mute != MUTE_NONE)
        return;
    uint8_t b[5];
    b[0] = (uint8_t)digit;
    put32(b + 1, (uint32_t)duration_ms);
    send_msg(t, MSG_DTMF, b, 5);
}

/* ---- lifecycle ---- */

nettrx_t *nettrx_new(const nettrx_cfg_t *cfg)
{
    nettrx_t *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->cfg = *cfg;
    t->auto_minor = cfg->proto_minor <= 0;
    t->minor = t->auto_minor ? 8 : cfg->proto_minor;
    t->cfd = -1;
    t->lfd = -1;
    t->mute = MUTE_ALL;
    t->st.mute_state = MUTE_ALL;
    t->gain = powf(10.0f, cfg->preamp_db / 20.0f);
    t->opus_frame = cfg->sample_rate / 50;
    t->tx = malloc(TXBUF_CAP);
    if (!t->tx) {
        free(t);
        return NULL;
    }

    /* Dual-stack listener (IPv4 clients show up as ::ffff:a.b.c.d). */
    int fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        log_error("uplink: socket: %s", strerror(errno));
        nettrx_free(t);
        return NULL;
    }
    int one = 1, zero = 0;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof sa);
    sa.sin6_family = AF_INET6;
    sa.sin6_addr = in6addr_any;
    sa.sin6_port = htons((uint16_t)cfg->listen_port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 4) != 0) {
        log_error("uplink: cannot listen on TCP port %d: %s", cfg->listen_port, strerror(errno));
        close(fd);
        nettrx_free(t);
        return NULL;
    }
    t->lfd = fd;
    log_info("uplink: waiting for the repeater on TCP port %d (RemoteTrx protocol %d.%d%s)",
             cfg->listen_port, cfg->proto_major, t->minor, t->auto_minor ? ", auto" : "");
    return t;
}

void nettrx_free(nettrx_t *t)
{
    if (!t)
        return;
    if (t->cfd >= 0) close(t->cfd);
    if (t->lfd >= 0) close(t->lfd);
    if (t->opus) opus_encoder_destroy(t->opus);
    free(t->tx);
    free(t);
}

int  nettrx_listen_fd(const nettrx_t *t) { return t->lfd; }
int  nettrx_client_fd(const nettrx_t *t) { return t->cfd; }
bool nettrx_want_write(const nettrx_t *t) { return t->cfd >= 0 && t->tx_len > 0; }

void nettrx_get_status(const nettrx_t *t, nettrx_status_t *out) { *out = t->st; out->proto_minor = t->minor; }
