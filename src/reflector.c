/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "reflector.h"
#include "refmsg.h"
#include "refcrypto.h"
#include "tls.h"
#include "pki.h"
#include "srv.h"
#include "jitter.h"
#include "util.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/hmac.h>
#include <openssl/evp.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/time.h>

#define RXBUF_CAP    (128 * 1024)
#define FRAME_CAP    (128 * 1024)
#define EVQ_CAP      32
#define CMDQ_CAP     16
#define CMD_BUF      2048
#define UDP_PT_CAP   2048
#define RX_POLL_MS   200

struct reflector {
    reflector_cfg_t cfg;

    /* threads */
    pthread_t net_thread;
    bool      net_running;
    pthread_t udp_thread;
    bool      udp_thread_started;

    atomic_bool should_run;       /* whole client lifetime */
    atomic_bool conn_udp_run;     /* per-connection UDP loop */

    /* sockets (network thread owns fd/tls; udp_sock shared under lock for TX) */
    int     fd;
    tls_t  *tls;
    int     udp_sock;
    struct sockaddr_in udp_addr;
    char    resolved_host[256];   /* host we actually connected to (for UDP); under lock */
    int     resolved_port;

    /* crypto + identity */
    refcrypto_t rc;
    _Atomic uint16_t client_id;
    atomic_int  proto_major;   /* negotiated proto major (3 = TLS+GCM, 2 = legacy plaintext) */
    uint16_t    udp_seq;       /* proto-2 plaintext UDP sequence number */

    /* RX sequencing (UDP thread only) + post-TG-switch gate (main arms, UDP reads) */
    jitter_t                jitter;
    atomic_uint_least64_t   gate_until_ms;

    /* state */
    atomic_bool connected;
    atomic_bool udp_ready;
    atomic_bool waiting_for_cert;
    bool        cert_obtained;
    atomic_ulong n_connects, n_rx_frames, n_tx_frames;
    _Atomic uint32_t selected_tg;   /* written by main (select) + net (reconnect) */

    /* RX audio ring (UDP thread -> main) */
    frame_ring_t *rx_ring;
    int           rx_eventfd;

    /* shared queues */
    pthread_mutex_t lock;
    reflector_event_t evq[EVQ_CAP];
    int ev_head, ev_tail;
    int ev_eventfd;
    struct { uint8_t buf[CMD_BUF]; int len; } cmdq[CMDQ_CAP];
    int cmd_head, cmd_tail;

    /* network-thread receive reassembly */
    uint8_t *rxbuf;
    size_t   rxlen;
    uint8_t *framebuf;
};

/* forward declarations */
static int tg_exclude_local(const uint32_t *src, int n, uint32_t excl,
                            uint32_t *out, int max);
static void teardown_udp(reflector_t *r);

/* ------------------------------------------------------------------ */
/* small helpers                                                       */

static void sock_timeout_ms(int fd, int ms)
{
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

static void push_event(reflector_t *r, reflector_event_type_t type,
                       uint32_t tg, const char *cs)
{
    pthread_mutex_lock(&r->lock);
    int next = (r->ev_head + 1) % EVQ_CAP;
    if (next != r->ev_tail) {
        r->evq[r->ev_head].type = type;
        r->evq[r->ev_head].tg   = tg;
        snprintf(r->evq[r->ev_head].callsign, sizeof r->evq[r->ev_head].callsign,
                 "%s", cs ? cs : "");
        r->ev_head = next;
    }
    pthread_mutex_unlock(&r->lock);
    uint64_t one = 1;
    if (write(r->ev_eventfd, &one, sizeof one) < 0) { /* non-fatal */ }
}

int reflector_get_event(reflector_t *r, reflector_event_t *out)
{
    uint64_t drain;
    while (read(r->ev_eventfd, &drain, sizeof drain) > 0) { /* clear */ }
    pthread_mutex_lock(&r->lock);
    int rc = -1;
    if (r->ev_tail != r->ev_head) {
        *out = r->evq[r->ev_tail];
        r->ev_tail = (r->ev_tail + 1) % EVQ_CAP;
        rc = 0;
    }
    pthread_mutex_unlock(&r->lock);
    return rc;
}

int reflector_event_fd(const reflector_t *r)    { return r->ev_eventfd; }
int reflector_rx_audio_fd(const reflector_t *r) { return r->rx_eventfd; }
bool reflector_connected(const reflector_t *r)  { return atomic_load(&r->connected); }

void reflector_get_status(reflector_t *r, reflector_status_t *out)
{
    memset(out, 0, sizeof *out);
    out->connected        = atomic_load(&r->connected);
    out->udp_ready        = atomic_load(&r->udp_ready);
    out->waiting_for_cert = atomic_load(&r->waiting_for_cert);
    out->proto_major      = atomic_load(&r->proto_major);
    out->client_id        = atomic_load(&r->client_id);
    out->selected_tg      = atomic_load(&r->selected_tg);
    out->connects         = atomic_load(&r->n_connects);
    out->rx_frames        = atomic_load(&r->n_rx_frames);
    out->tx_frames        = atomic_load(&r->n_tx_frames);
    pthread_mutex_lock(&r->lock);
    snprintf(out->host, sizeof out->host, "%s", r->resolved_host);
    out->port = r->resolved_port;
    pthread_mutex_unlock(&r->lock);
}

int reflector_rx_audio_pop(reflector_t *r, uint8_t *out, int cap)
{
    uint64_t drain;
    while (read(r->rx_eventfd, &drain, sizeof drain) > 0) { /* clear */ }
    ssize_t n = frame_ring_pop(r->rx_ring, out, (size_t)cap);
    return n < 0 ? -1 : (int)n;       /* 0 = the talker stopped (flush marker) */
}

/* Enqueue a pre-built TCP payload for the network thread to send. */
static void cmd_enqueue(reflector_t *r, const uint8_t *payload, int plen)
{
    if (plen <= 0 || plen > CMD_BUF) return;
    pthread_mutex_lock(&r->lock);
    int next = (r->cmd_head + 1) % CMDQ_CAP;
    if (next != r->cmd_tail) {
        memcpy(r->cmdq[r->cmd_head].buf, payload, (size_t)plen);
        r->cmdq[r->cmd_head].len = plen;
        r->cmd_head = next;
    }
    pthread_mutex_unlock(&r->lock);
}

/* ------------------------------------------------------------------ */
/* TCP framing (network thread is the sole TLS writer)                 */

static int raw_send_all(int fd, const uint8_t *buf, int len)
{
    int off = 0;
    uint64_t until = mono_ms() + 10000;      /* a peer that stopped reading is given up */
    while (off < len) {
        ssize_t n = send(fd, buf + off, (size_t)(len - off), 0);
        if (n > 0) { off += (int)n; continue; }
        if (mono_ms() > until) return -1;
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return -1;
    }
    return len;
}

/* Frame `payload` ([type..]) as [u32 len][payload] and send (TLS or raw). */
static int send_frame(reflector_t *r, const uint8_t *payload, int plen)
{
    if (plen < 0 || plen > (int)(FRAME_CAP - 4)) return -1;
    uint8_t hdr[4];
    wr_be32(hdr, (uint32_t)plen);
    if (r->tls) {
        if (tls_write(r->tls, hdr, 4) != 4) return -1;
        if (tls_write(r->tls, payload, plen) != plen) return -1;
    } else {
        if (raw_send_all(r->fd, hdr, 4) != 4) return -1;
        if (raw_send_all(r->fd, payload, plen) != plen) return -1;
    }
    return 0;
}

/* Read more bytes into rxbuf. Returns >0 appended, -2 timeout, -1 closed. */
static int recv_more(reflector_t *r)
{
    uint8_t tmp[8192];
    int n;
    if (r->tls) {
        n = tls_read(r->tls, tmp, (int)sizeof tmp);
        if (n == -2) return -2;
        if (n <= 0) return -1;
    } else {
        ssize_t rn = recv(r->fd, tmp, sizeof tmp, 0);
        if (rn == 0) return -1;
        if (rn < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return -2;
            return -1;
        }
        n = (int)rn;
    }
    if (r->rxlen + (size_t)n > RXBUF_CAP) {
        log_warn("reflector: rx buffer overflow — dropping the connection");
        r->rxlen = 0;
        return -1;
    }
    memcpy(r->rxbuf + r->rxlen, tmp, (size_t)n);
    r->rxlen += (size_t)n;
    return n;
}

/* Pop one complete frame payload into framebuf. Returns plen, or -1 if none. */
static int next_frame(reflector_t *r)
{
    if (r->rxlen < 4) return -1;
    uint32_t flen = rd_be32(r->rxbuf);
    if (flen == 0 || flen >= 1000000u || flen > FRAME_CAP) {
        log_warn("reflector: bad/oversized frame length %u, clearing buffer", flen);
        r->rxlen = 0;
        return -1;
    }
    if (r->rxlen < 4 + (size_t)flen) return -1;
    memcpy(r->framebuf, r->rxbuf + 4, flen);
    size_t consumed = 4 + (size_t)flen;
    memmove(r->rxbuf, r->rxbuf + consumed, r->rxlen - consumed);
    r->rxlen -= consumed;
    return (int)flen;
}

/* ------------------------------------------------------------------ */
/* UDP TX (lock held by caller)                                        */

static void udp_send_locked(reflector_t *r, uint16_t type, const uint8_t *body, int blen)
{
    if (!atomic_load(&r->udp_ready) || r->udp_sock < 0) return;

    if (atomic_load(&r->proto_major) < 3) {
        /* legacy proto-2 ReflectorUdpMsgV2: plaintext [u16 type][u16 client_id]
         * [u16 seq][payload] (audio payload is a u16-len-prefixed vector). */
        uint8_t pkt[UDP_PT_CAP];
        int o = 0;
        wr_be16(pkt + o, type);          o += 2;
        wr_be16(pkt + o, atomic_load(&r->client_id));  o += 2;
        wr_be16(pkt + o, r->udp_seq++);  o += 2;
        if (type == RU_AUDIO && blen > 0) {
            if (o + 2 + blen > (int)sizeof pkt) return;
            wr_be16(pkt + o, (uint16_t)blen); o += 2;
            memcpy(pkt + o, body, (size_t)blen); o += blen;
        } else if (blen > 0) {
            if (o + blen > (int)sizeof pkt) return;
            memcpy(pkt + o, body, (size_t)blen); o += blen;
        }
        sendto(r->udp_sock, pkt, (size_t)o, 0,
               (struct sockaddr *)&r->udp_addr, sizeof r->udp_addr);
        return;
    }

    uint8_t pt[UDP_PT_CAP];
    int o = 0;
    wr_be16(pt + o, type); o += 2;
    if (type == RU_AUDIO && blen > 0) {
        if (o + 2 + blen > (int)sizeof pt) return;
        wr_be16(pt + o, (uint16_t)blen); o += 2;
        memcpy(pt + o, body, (size_t)blen); o += blen;
    } else if (blen > 0) {
        if (o + blen > (int)sizeof pt) return;
        memcpy(pt + o, body, (size_t)blen); o += blen;
    }

    uint8_t wire[UDP_PT_CAP + 32];
    int w = refcrypto_encrypt(&r->rc, pt, (size_t)o, wire, sizeof wire);
    if (w < 0) return;
    sendto(r->udp_sock, wire, (size_t)w, 0,
           (struct sockaddr *)&r->udp_addr, sizeof r->udp_addr);
}

int reflector_send_audio(reflector_t *r, const uint8_t *opus, int len)
{
    if (!atomic_load(&r->udp_ready)) return -1;
    pthread_mutex_lock(&r->lock);
    udp_send_locked(r, RU_AUDIO, opus, len);
    pthread_mutex_unlock(&r->lock);
    atomic_fetch_add(&r->n_tx_frames, 1);
    return 0;
}

void reflector_tx_stop(reflector_t *r)
{
    pthread_mutex_lock(&r->lock);
    udp_send_locked(r, RU_FLUSH_SAMPLES, NULL, 0);
    pthread_mutex_unlock(&r->lock);
}

/* Snapshot the monitor set under the lock (it is mutated by the lock command
 * from the main thread and read by both the main and network threads). */
bool reflector_send_rx_status(reflector_t *r, const refmsg_rx_t *rx, int n)
{
    uint8_t m[64];
    if (!atomic_load(&r->udp_ready))
        return false;
    int len = refmsg_build_siglev(m, sizeof m, rx, n);
    if (len > 0) cmd_enqueue(r, m, len);
    return true;
}

bool reflector_send_tx_status(reflector_t *r, char id, bool transmit)
{
    uint8_t m[8];
    if (!atomic_load(&r->udp_ready))
        return false;
    int len = refmsg_build_tx_status(m, sizeof m, id, transmit);
    if (len > 0) cmd_enqueue(r, m, len);
    return true;
}

static int monitor_snapshot(reflector_t *r, uint32_t *out, int cap)
{
    pthread_mutex_lock(&r->lock);
    int n = r->cfg.n_monitor;
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) out[i] = r->cfg.monitor_tgs[i];
    pthread_mutex_unlock(&r->lock);
    return n;
}

void reflector_select_tg(reflector_t *r, uint32_t tg, bool drop_in_flight)
{
    uint8_t m[8 + REFLECTOR_MAX_MONITOR * 4];
    int n;

    atomic_store(&r->selected_tg, tg);

    /* Not logged in (yet): nothing to send. The selection is asserted by
     * apply_after_udp() as soon as the session is up. */
    if (!atomic_load(&r->udp_ready))
        return;

    /* Leaving a talkgroup in the middle of an over: drop its audio that is
     * still in flight, or it would be played as if it came from the new one. */
    if (drop_in_flight)
        atomic_store(&r->gate_until_ms, mono_ms() + 250);

    n = refmsg_build_select_tg(m, sizeof m, tg);
    if (n > 0) cmd_enqueue(r, m, n);

    uint32_t snap[REFLECTOR_MAX_MONITOR], mon[REFLECTOR_MAX_MONITOR];
    int ns = monitor_snapshot(r, snap, REFLECTOR_MAX_MONITOR);
    int nm = tg_exclude_local(snap, ns, tg, mon, REFLECTOR_MAX_MONITOR);
    n = refmsg_build_tg_monitor(m, sizeof m, mon, nm);
    if (n > 0) cmd_enqueue(r, m, n);

    /* immediate UDP heartbeat re-asserts our endpoint after a TG switch */
    pthread_mutex_lock(&r->lock);
    udp_send_locked(r, RU_HEARTBEAT, NULL, 0);
    pthread_mutex_unlock(&r->lock);
}

/* MsgTgMonitor must not list the selected talkgroup. */
static int tg_exclude_local(const uint32_t *src, int n, uint32_t excl,
                            uint32_t *out, int max)
{
    int c = 0;
    for (int i = 0; i < n && c < max; i++)
        if (src[i] != excl) out[c++] = src[i];
    return c;
}

/* ------------------------------------------------------------------ */
/* NodeInfo JSON                                                       */

/* JSON string body: drop control characters, escape quotes and backslashes. */
static const char *jesc(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 2 < cap; in++) {
        unsigned char ch = (unsigned char)*in;
        if (ch == '"' || ch == '\\') { out[o++] = '\\'; out[o++] = (char)ch; }
        else if (ch >= 0x20)         { out[o++] = (char)ch; }
    }
    out[o] = '\0';
    return out;
}

/* A coordinate as a JSON number — as a string if it is not one. */
static const char *jnum(const char *in, char *out, size_t cap)
{
    char *end = NULL, tmp[80];
    double v = strtod(in, &end);
    if (end != in && *end == '\0')
        snprintf(out, cap, "%.6f", v);
    else
        snprintf(out, cap, "\"%s\"", jesc(in, tmp, sizeof tmp));
    return out;
}

static void build_node_info_json(reflector_t *r, char *out, size_t cap)
{
    char pos[256] = "", e1[160], e2[80], e3[80], e4[120], e5[80];
    if (r->cfg.latitude[0] && r->cfg.longitude[0]) {
        snprintf(pos, sizeof pos,
                 ",\"pos\":{\"lat\":%s,\"long\":%s,\"loc\":\"%s\"}",
                 jnum(r->cfg.latitude, e1, sizeof e1), jnum(r->cfg.longitude, e2, sizeof e2),
                 jesc(r->cfg.locator, e3, sizeof e3));
    }
    const char *sysop = r->cfg.sysop[0] ? r->cfg.sysop : r->cfg.callsign;
    const char *qth   = r->cfg.qth_name[0] ? r->cfg.qth_name : "TailChaser";
    snprintf(out, cap,
        "{\"nodeLocation\":\"%s\",\"hidden\":%s,\"sysop\":\"%s\",%s"
        "\"sw\":\"TailChaser\",\"swVer\":\"%s\","
        "\"qth\":[{\"name\":\"%s\",%s%s}]}",
        jesc(r->cfg.location, e1, sizeof e1), r->cfg.hidden ? "true" : "false",
        jesc(sysop, e2, sizeof e2), r->cfg.extra_json,
        jesc(r->cfg.sw_version, e5, sizeof e5), jesc(qth, e4, sizeof e4),
        r->cfg.rxtx_json[0] ? r->cfg.rxtx_json
                            : "\"rx\":{\"A\":{\"name\":\"Rx1\"}},\"tx\":{\"A\":{\"name\":\"Tx1\"}}",
        pos);
}

static void send_node_info(reflector_t *r)
{
    uint8_t iv[6], key[16];
    pthread_mutex_lock(&r->lock);
    int g = refcrypto_gen_tx(&r->rc, atomic_load(&r->client_id), iv, key);
    pthread_mutex_unlock(&r->lock);
    if (g != 0) { log_error("reflector: gen_tx failed"); return; }

    char json[3200];
    build_node_info_json(r, json, sizeof json);

    uint8_t msg[4096];
    int n = refmsg_build_node_info(msg, sizeof msg, iv, 6, key, 16, json);
    if (n > 0) send_frame(r, msg, n);
    log_info("reflector: sent NodeInfo (%zu B JSON)", strlen(json));
}

/* ------------------------------------------------------------------ */
/* UDP receive thread                                                  */

/* Hand one received frame to the main loop. len 0 = the server's "talker has
 * stopped" (MsgUdpFlushSamples), passed in-band so it stays in order with the
 * audio. */
static void push_rx(reflector_t *r, const uint8_t *data, size_t len)
{
    if (frame_ring_push(r->rx_ring, data, len) == 0) {
        if (len > 0) atomic_fetch_add(&r->n_rx_frames, 1);
        uint64_t one = 1;
        if (write(r->rx_eventfd, &one, sizeof one) < 0) { /* non-fatal */ }
    }
}

static void *udp_loop(void *arg)
{
    reflector_t *r = arg;
    uint8_t buf[65536];
    uint8_t pt[2048];
    /* Capture the fd once: setup_udp sets r->udp_sock before spawning us, and
     * teardown closes that fd (unblocking recvfrom -> EBADF) before joining, so
     * we never read a reused descriptor mid-loop. */
    int fd = r->udp_sock;

    while (atomic_load(&r->conn_udp_run) && atomic_load(&r->should_run)) {
        struct sockaddr_storage from;
        socklen_t fromlen = sizeof from;
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n == 0) continue;                     /* an empty datagram is nothing */
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            break;
        }
        if (atomic_load(&r->proto_major) < 3) {
            /* nothing protects this audio: at least it has to come from the reflector */
            const struct sockaddr_in *f4 = (const struct sockaddr_in *)&from;
            if (from.ss_family != AF_INET || f4->sin_addr.s_addr != r->udp_addr.sin_addr.s_addr ||
                f4->sin_port != r->udp_addr.sin_port)
                continue;
            /* legacy proto-2 plaintext: [u16 type][u16 client_id][u16 seq][payload] */
            if (n < 6) continue;
            uint16_t type = rd_be16(buf);
            if (type == RU_FLUSH_SAMPLES) { push_rx(r, buf, 0); continue; }
            if (type != RU_AUDIO) continue;
            if (n < 8) continue;
            int alen = rd_be16(buf + 6);          /* after type+client_id+seq */
            if (alen <= 0 || 8 + alen > n || alen > REFLECTOR_MAX_OPUS) continue;
            { static bool rx_logged = false; if (!rx_logged) {
                log_info("reflector: proto-2 UDP audio RX active"); rx_logged = true; } }
            push_rx(r, buf + 8, (size_t)alen);
            continue;
        }

        if (n < 12) continue;                     /* AAD(4) + TAG(8) minimum */

        uint32_t counter = rd_be32(buf);          /* AAD = per-direction counter */

        /* Post-TG-switch gate: drop frames still in flight from the old TG. */
        uint64_t gate = atomic_load(&r->gate_until_ms);
        if (gate && mono_ms() < gate) continue;

        /* Replay / late / duplicate reject (pre-decrypt; does not advance hi). */
        if (jitter_is_replay(&r->jitter, counter)) continue;

        int plen = refcrypto_decrypt(&r->rc, buf, (size_t)n, pt, sizeof pt);
        if (plen < 2) continue;                   /* bad tag / forged -> dropped */

        /* Authenticated: now it is safe to advance the sequence high-water. */
        jitter_commit(&r->jitter, counter);

        uint16_t type = rd_be16(pt);
        if (type == RU_FLUSH_SAMPLES) { push_rx(r, pt, 0); continue; }
        if (type != RU_AUDIO) continue;
        if (plen < 4) continue;
        int alen = rd_be16(pt + 2);
        if (alen <= 0 || 4 + alen > plen) continue;
        if (alen > REFLECTOR_MAX_OPUS) continue;

        push_rx(r, pt + 4, (size_t)alen);
    }
    log_debug("reflector: UDP receive loop exited");
    return NULL;
}

static void setup_udp(reflector_t *r)
{
    /* Re-entrancy guard: a duplicate RT_START_UDP_ENCRYPTION within one
     * connection must stop+join the live UDP thread before re-arming jitter
     * and spawning a new one — otherwise jitter_reset races the running thread
     * and the thread handle + socket leak. */
    if (r->udp_thread_started)
        teardown_udp(r);

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { log_error("reflector: udp socket: %s", strerror(errno)); return; }

    struct sockaddr_in local;
    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_port = 0;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&local, sizeof local) != 0)
        log_warn("reflector: udp bind: %s (relying on implicit bind)", strerror(errno));

    jitter_reset(&r->jitter);
    atomic_store(&r->gate_until_ms, 0);

    /* The audio goes to the host AND port the control connection went to: the
     * port can come from the SRV record or from "host:port", not only from
     * the port setting. */
    int uport = r->resolved_port > 0 ? r->resolved_port : r->cfg.port;
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%d", uport);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    const char *uhost = r->resolved_host[0] ? r->resolved_host : r->cfg.domain;
    if (getaddrinfo(uhost, portstr, &hints, &res) != 0 || !res) {
        log_error("reflector: udp getaddrinfo failed");
        close(s);
        return;
    }
    memcpy(&r->udp_addr, res->ai_addr, sizeof r->udp_addr);
    freeaddrinfo(res);

    sock_timeout_ms(s, 1000);

    pthread_mutex_lock(&r->lock);
    r->udp_sock = s;
    atomic_store(&r->udp_ready, true);
    pthread_mutex_unlock(&r->lock);

    atomic_store(&r->conn_udp_run, true);
    if (pthread_create(&r->udp_thread, NULL, udp_loop, r) == 0)
        r->udp_thread_started = true;
    else
        log_error("reflector: udp pthread_create failed");

    /* initial UDP heartbeat to open the NAT pinhole / announce our endpoint */
    pthread_mutex_lock(&r->lock);
    udp_send_locked(r, RU_HEARTBEAT, NULL, 0);
    pthread_mutex_unlock(&r->lock);

    log_info("reflector: UDP up to %s:%d", uhost, uport);
}

static void teardown_udp(reflector_t *r)
{
    atomic_store(&r->udp_ready, false);
    atomic_store(&r->conn_udp_run, false);
    pthread_mutex_lock(&r->lock);
    int s = r->udp_sock;
    r->udp_sock = -1;
    pthread_mutex_unlock(&r->lock);
    if (s >= 0) close(s);          /* unblocks recvfrom */
    if (r->udp_thread_started) {
        pthread_join(r->udp_thread, NULL);
        r->udp_thread_started = false;
    }
}

/* ------------------------------------------------------------------ */
/* per-connection handling                                             */

typedef enum { CONN_CLOSED, CONN_CERT, CONN_STOP } conn_result_t;

/* apply selected TG + monitor list after UDP comes up (network thread). */
static void apply_after_udp(reflector_t *r)
{
    /* Re-assert the operator's CURRENT TG (selected_tg, initialised to
     * default_tg) so an automatic reconnect doesn't silently bounce them back
     * to the default and diverge from the engine's view. */
    uint32_t sel = atomic_load(&r->selected_tg);
    uint8_t m[8 + REFLECTOR_MAX_MONITOR * 4];
    int n = refmsg_build_select_tg(m, sizeof m, sel);
    if (n > 0) send_frame(r, m, n);
    uint32_t snap[REFLECTOR_MAX_MONITOR], mon[REFLECTOR_MAX_MONITOR];
    int ns = monitor_snapshot(r, snap, REFLECTOR_MAX_MONITOR);
    int nm = tg_exclude_local(snap, ns, sel, mon, REFLECTOR_MAX_MONITOR);
    n = refmsg_build_tg_monitor(m, sizeof m, mon, nm);
    if (n > 0) send_frame(r, m, n);
    log_info("reflector: selected TG %u, monitoring %d TG(s)", sel, nm);
}

/* Returns true to keep the connection running, false to drop it. */
static bool process_frame(reflector_t *r, const uint8_t *p, size_t plen,
                          conn_result_t *result, bool *want_drop)
{
    int type = refmsg_type(p, plen);
    if (type < 0) return true;

    switch (type) {
    case RT_PROTO_VER_DOWNGRADE: {
        /* Older (legacy proto 2.x) reflector: it told us its max version, so
         * re-send ProtoVer at that version and let the (plaintext, HMAC-auth)
         * proto-2 flow continue. */
        /* Only where a password is the login and this node holds no
         * certificate: a node that has one never talks in the clear, whoever
         * asks for it. */
        if (!r->cfg.use_authkey || pki_have_cert(r->cfg.state_dir, r->cfg.callsign)) {
            log_error("reflector: the server asks for the old protocol without encryption — refused%s",
                      r->cfg.use_authkey ? " (this node has a certificate)" : " (no auth_key set)");
            *want_drop = true; *result = CONN_CLOSED; return false;
        }
        if (plen >= 6) {
            uint16_t maj = (uint16_t)((p[2] << 8) | p[3]);
            uint16_t min = (uint16_t)((p[4] << 8) | p[5]);
            log_info("reflector: server max ProtoVer %u.%u — downgrading", maj, min);
            atomic_store(&r->proto_major, maj);   /* proto-2 => plaintext UDP, no TLS */
            uint8_t pv[6];
            int n = refmsg_build_proto_ver(pv, sizeof pv, maj, min);
            if (n > 0) send_frame(r, pv, n);
        }
        break;
    }
    case RT_HEARTBEAT: {
        uint8_t m[2]; int n = refmsg_build_heartbeat(m, sizeof m);
        if (n > 0) send_frame(r, m, n);
        break;
    }
    case RT_CA_INFO: {
        uint8_t m[2]; int n = refmsg_build_ca_bundle_request(m, sizeof m);
        if (n > 0) send_frame(r, m, n);
        break;
    }
    case RT_CA_BUNDLE_RESPONSE: {
        char pem[64 * 1024];
        if (refmsg_extract_pem(p, plen, pem, sizeof pem) > 0)
            pki_store_ca(r->cfg.state_dir, pem);
        uint8_t m[2]; int n = refmsg_build_start_encryption_request(m, sizeof m);
        if (n > 0) send_frame(r, m, n);
        break;
    }
    case RT_START_ENCRYPTION: {
        log_info("reflector: server ready for TLS");
        r->rxlen = 0;   /* nothing plaintext should remain before the handshake */
        if (r->tls) { *want_drop = true; *result = CONN_CLOSED; return false; }     /* twice: not a reflector */
        {
            char host[256];
            pthread_mutex_lock(&r->lock);
            snprintf(host, sizeof host, "%s", r->resolved_host);
            pthread_mutex_unlock(&r->lock);
            const char *names[] = { host, r->cfg.domain, NULL };
            r->tls = tls_client_start(r->fd, r->cfg.state_dir, r->cfg.callsign, names, r->cfg.verify_host);
        }
        if (!r->tls) { *want_drop = true; *result = CONN_CLOSED; return false; }
        break;
    }
    case RT_CLIENT_CSR_REQUEST: {
        char csr[8192];
        if (pki_ensure_csr(r->cfg.state_dir, r->cfg.callsign, r->cfg.email,
                           csr, sizeof csr) == 0) {
            uint8_t m[8192 + 8];
            int n = refmsg_build_client_csr(m, sizeof m, csr);
            if (n > 0) send_frame(r, m, n);
            atomic_store(&r->waiting_for_cert, true);
            log_info("reflector: certificate request sent — waiting for the reflector sysop to sign it");
        } else {
            log_error("reflector: CSR generation failed");
        }
        break;
    }
    case RT_CLIENT_CERT: {
        /* Comes in answer to the signing request, and also unasked: the
         * reflector renews a certificate before it expires and hands the new
         * one to the node, at login or in the middle of a session. */
        char pem[64 * 1024];
        if (refmsg_extract_pem(p, plen, pem, sizeof pem) <= 0) {
            log_info("reflector: no certificate yet — the request is waiting to be signed");
            break;
        }
        if (!pki_cert_matches_key(r->cfg.state_dir, r->cfg.callsign, pem)) {
            log_error("reflector: the certificate that was sent does not belong to this "
                      "node's key — ignored");
            break;
        }
        if (pki_store_cert(r->cfg.state_dir, r->cfg.callsign, pem) == 0) {
            log_info("reflector: certificate received — reconnecting with it");
            r->cert_obtained = true;
            *result = CONN_CERT;
            return false;
        }
        log_error("reflector: could not store the certificate");
        break;
    }
    case RT_AUTH_CHALLENGE: {
        uint8_t chal[20];
        if (refmsg_parse_auth_challenge(p, plen, chal) == 0) {
            char cs[32];
            size_t i = 0;
            for (; r->cfg.callsign[i] && i + 1 < sizeof cs; i++)
                cs[i] = (char)toupper((unsigned char)r->cfg.callsign[i]);
            cs[i] = '\0';
            uint8_t digest[20] = { 0 };   /* cert auth: identity proven by TLS */
            if (r->cfg.use_authkey && r->cfg.auth_key[0]) {
                /* authkey auth (username/password reflectors): SvxReflector wants
                 * HMAC-SHA1(auth_key, challenge) as the response digest. */
                unsigned int dl = 0;
                HMAC(EVP_sha1(), r->cfg.auth_key, (int)strlen(r->cfg.auth_key),
                     chal, sizeof chal, digest, &dl);
            }
            uint8_t m[128];
            int n = refmsg_build_auth_response(m, sizeof m, cs, digest, sizeof digest);
            if (n > 0) send_frame(r, m, n);
        }
        break;
    }
    case RT_AUTH_OK:
        log_info("reflector: authenticated");
        break;
    case RT_SERVER_INFO: {
        uint16_t cid = 0;
        if (refmsg_parse_server_info_client_id(p, plen, &cid) == 0)
            atomic_store(&r->client_id, cid);
        atomic_store(&r->connected, true);
        atomic_store(&r->waiting_for_cert, false);
        atomic_fetch_add(&r->n_connects, 1);
        log_info("reflector: logged in as %s (client id %u, protocol %d)", r->cfg.callsign,
                 (unsigned)atomic_load(&r->client_id), atomic_load(&r->proto_major));
        push_event(r, RE_CONNECTED, 0, NULL);
        if (atomic_load(&r->proto_major) >= 3) {
            send_node_info(r);     /* v3: gen TX key + NodeInfo; server replies START_UDP_ENCRYPTION */
        } else {
            /* legacy proto-2: plaintext UDP, no encryption handshake — bring UDP
             * up now and assert the TG selection. */
            setup_udp(r);
            apply_after_udp(r);
        }
        break;
    }
    case RT_START_UDP_ENCRYPTION: {
        uint8_t iv[8], key[32];
        size_t ivl = 0, keyl = 0;
        int pr = refmsg_parse_start_udp_enc(p, plen, iv, &ivl, key, &keyl);
        pthread_mutex_lock(&r->lock);
        if (pr == 1)       refcrypto_use_tx_for_rx(&r->rc);
        else if (pr == 0)  refcrypto_set_rx(&r->rc, iv, ivl, key, keyl);
        else               refcrypto_use_tx_for_rx(&r->rc);   /* fallback */
        pthread_mutex_unlock(&r->lock);
        setup_udp(r);
        apply_after_udp(r);
        break;
    }
    case RT_TALKER_START: {
        uint32_t tg; char cs[32];
        if (refmsg_parse_talker(p, plen, &tg, cs, sizeof cs) == 0)
            push_event(r, RE_TALKER_START, tg, cs);
        break;
    }
    case RT_TALKER_STOP: {
        uint32_t tg; char cs[32];
        if (refmsg_parse_talker(p, plen, &tg, cs, sizeof cs) == 0)
            push_event(r, RE_TALKER_STOP, tg, cs);
        break;
    }
    case RT_NODE_JOINED: {
        char cs[32];
        if (refmsg_parse_string_at(p, plen, 2, cs, sizeof cs) == 0)
            log_debug("reflector: node joined: %s", cs);
        break;
    }
    case RT_NODE_LEFT: {
        char cs[32];
        if (refmsg_parse_string_at(p, plen, 2, cs, sizeof cs) == 0)
            log_debug("reflector: node left: %s", cs);
        break;
    }
    case RT_ERROR: {
        char msg[256];
        if (refmsg_parse_string_at(p, plen, 2, msg, sizeof msg) == 0)
            log_warn("reflector: server error: %s", msg);
        break;
    }
    default:
        log_debug("reflector: unhandled msg type %d (%zu B)", type, plen);
        break;
    }
    return true;
}

static bool tcp_connect(reflector_t *r, const char *host, int port)
{
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        log_error("reflector: getaddrinfo %s failed", host);
        return false;
    }
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) { freeaddrinfo(res); return false; }
    if (connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        log_error("reflector: connect %s:%d failed: %s", host, port, strerror(errno));
        close(s);
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);
    /* A short read timeout: the connection loop also sends what the main loop
     * queued (talkgroup switches), and those must not wait a second. */
    sock_timeout_ms(s, RX_POLL_MS);
    r->fd = s;
    log_info("reflector: TCP connected to %s:%d", host, port);
    return true;
}

static void drain_commands(reflector_t *r)
{
    for (;;) {
        uint8_t buf[CMD_BUF];
        int len = 0;
        pthread_mutex_lock(&r->lock);
        if (r->cmd_tail != r->cmd_head) {
            len = r->cmdq[r->cmd_tail].len;
            memcpy(buf, r->cmdq[r->cmd_tail].buf, (size_t)len);
            r->cmd_tail = (r->cmd_tail + 1) % CMDQ_CAP;
        }
        pthread_mutex_unlock(&r->lock);
        if (len <= 0) break;
        send_frame(r, buf, len);
    }
}

static conn_result_t run_connection(reflector_t *r, const char *host, int port)
{
    /* reset per-connection state */
    r->fd = -1;
    r->tls = NULL;
    r->rxlen = 0;
    atomic_store(&r->waiting_for_cert, false);
    r->cert_obtained = false;
    atomic_store(&r->proto_major, REFMSG_PROTO_MAJOR);   /* v3 until a downgrade says otherwise */
    r->udp_seq = 0;
    atomic_store(&r->connected, false);
    atomic_store(&r->udp_ready, false);
    r->udp_sock = -1;
    pthread_mutex_lock(&r->lock);
    r->cmd_head = r->cmd_tail = 0;      /* commands meant for the previous session */
    pthread_mutex_unlock(&r->lock);

    if (!tcp_connect(r, host, port))
        return CONN_CLOSED;

    pthread_mutex_lock(&r->lock);
    snprintf(r->resolved_host, sizeof r->resolved_host, "%s", host);   /* for UDP setup */
    r->resolved_port = port;
    pthread_mutex_unlock(&r->lock);

    /* send ProtoVer */
    uint8_t pv[8];
    int n = refmsg_build_proto_ver(pv, sizeof pv, REFMSG_PROTO_MAJOR, REFMSG_PROTO_MINOR);
    send_frame(r, pv, n);

    conn_result_t result = CONN_CLOSED;
    uint64_t tcp_hb = 0, udp_hb = 0, csr_deadline = 0;

    while (atomic_load(&r->should_run)) {
        /* process buffered frames */
        bool drop = false;
        int plen;
        while ((plen = next_frame(r)) >= 0) {
            if (!process_frame(r, r->framebuf, (size_t)plen, &result, &drop))
                goto done;      /* TLS failed, or the certificate arrived: reconnect */
            if (atomic_load(&r->waiting_for_cert) && csr_deadline == 0)
                csr_deadline = mono_ms() + 30000;
        }

        /* outbound commands from the main thread (TG switches) */
        drain_commands(r);

        /* heartbeats. NB: gate on the control channel being usable, not on TLS —
         * legacy proto-2 runs plaintext (r->tls == NULL) but still needs the
         * periodic TCP heartbeat or the reflector drops us on its timeout. */
        uint64_t now = mono_ms();
        if (r->tls || atomic_load(&r->connected)) {
            if (tcp_hb == 0) tcp_hb = now;
            if (now - tcp_hb >= (uint64_t)r->cfg.tcp_heartbeat_ms) {
                tcp_hb = now;
                uint8_t m[2]; int hn = refmsg_build_heartbeat(m, sizeof m);
                if (hn > 0) send_frame(r, m, hn);
            }
        }
        if (atomic_load(&r->udp_ready)) {
            if (udp_hb == 0) udp_hb = now;
            if (now - udp_hb >= (uint64_t)r->cfg.udp_heartbeat_ms) {
                udp_hb = now;
                pthread_mutex_lock(&r->lock);
                udp_send_locked(r, RU_HEARTBEAT, NULL, 0);
                pthread_mutex_unlock(&r->lock);
            }
        }
        /* cert poll: if the sysop hasn't signed in 30 s, reconnect to re-check */
        if (atomic_load(&r->waiting_for_cert) && csr_deadline && now >= csr_deadline) {
            log_info("reflector: cert poll — reconnecting to re-check signature");
            result = CONN_CLOSED;
            goto done;
        }

        /* read more from the wire */
        int rr = recv_more(r);
        if (rr == -1) { result = CONN_CLOSED; goto done; }
        /* rr == -2 (timeout) or rr > 0 (data): loop */
    }
    result = CONN_STOP;

done:
    teardown_udp(r);
    if (r->tls) { tls_free(r->tls); r->tls = NULL; }
    if (r->fd >= 0) { close(r->fd); r->fd = -1; }
    return result;
}

/* ------------------------------------------------------------------ */
/* network thread                                                      */

static void *net_loop(void *arg)
{
    reflector_t *r = arg;
    int fails = 0;

    while (atomic_load(&r->should_run)) {
        /* resolve: SRV first, then static host list, then bare domain */
        srv_record_t recs[8];
        int nr = 0;
        char host[256];
        int port = r->cfg.port;

        if (r->cfg.domain[0] && !strchr(r->cfg.domain, ':') && !strchr(r->cfg.domain, ','))
            nr = srv_resolve(r->cfg.domain, recs, 8);
        if (nr == 0 && r->cfg.host[0])
            nr = srv_parse_hostlist(r->cfg.host, (uint16_t)r->cfg.port, recs, 8);

        if (nr > 0) {
            snprintf(host, sizeof host, "%s", recs[0].host);
            port = recs[0].port;
        } else {
            snprintf(host, sizeof host, "%s", r->cfg.domain);
        }

        conn_result_t cr = run_connection(r, host, port);

        /* the session is over: say so now, not when the next one begins */
        atomic_store(&r->udp_ready, false);
        if (atomic_exchange(&r->connected, false))
            push_event(r, RE_DISCONNECTED, 0, NULL);

        if (cr == CONN_STOP || !atomic_load(&r->should_run))
            break;

        /* backoff: fast while waiting for the cert / right after enrollment */
        int delay_ms;
        if (cr == CONN_CERT)          { delay_ms = 1000; fails = 0; }
        else if (atomic_load(&r->waiting_for_cert)) { delay_ms = 2000; }
        else {
            fails++;
            switch (fails) {
            case 1: case 2: delay_ms = 3000; break;
            case 3:         delay_ms = 5000; break;
            case 4:         delay_ms = 10000; break;
            default:        delay_ms = 20000; break;
            }
        }
        if (delay_ms > r->cfg.reconnect_max_ms && r->cfg.reconnect_max_ms > 0)
            delay_ms = r->cfg.reconnect_max_ms;
        log_info("reflector: reconnecting in %d ms", delay_ms);

        /* sleep in small slices so stop() is responsive */
        for (int slept = 0; slept < delay_ms && atomic_load(&r->should_run); slept += 100)
            usleep(100 * 1000);
        fails = (cr == CONN_CERT) ? 0 : fails;
    }
    log_info("reflector: network thread exited");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */

reflector_t *reflector_new(const reflector_cfg_t *cfg)
{
    reflector_t *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->cfg = *cfg;
    if (r->cfg.tcp_heartbeat_ms <= 0) r->cfg.tcp_heartbeat_ms = 5000;
    if (r->cfg.udp_heartbeat_ms <= 0) r->cfg.udp_heartbeat_ms = 10000;
    if (r->cfg.port <= 0) r->cfg.port = SRV_DEFAULT_PORT;

    refcrypto_init(&r->rc);
    jitter_init(&r->jitter);
    atomic_init(&r->gate_until_ms, 0);
    atomic_init(&r->selected_tg, cfg->default_tg);   /* persists across reconnect */
    pthread_mutex_init(&r->lock, NULL);
    r->fd = -1;
    r->udp_sock = -1;
    r->ev_eventfd = eventfd(0, EFD_NONBLOCK);
    r->rx_eventfd = eventfd(0, EFD_NONBLOCK);
    r->rx_ring = frame_ring_new(64, REFLECTOR_MAX_OPUS);
    r->rxbuf = malloc(RXBUF_CAP);
    r->framebuf = malloc(FRAME_CAP);
    if (r->ev_eventfd < 0 || r->rx_eventfd < 0 || !r->rx_ring ||
        !r->rxbuf || !r->framebuf) {
        reflector_free(r);
        return NULL;
    }
    return r;
}

int reflector_start(reflector_t *r)
{
    atomic_store(&r->should_run, true);
    if (pthread_create(&r->net_thread, NULL, net_loop, r) != 0) {
        log_error("reflector: cannot start network thread");
        atomic_store(&r->should_run, false);
        return -1;
    }
    r->net_running = true;
    return 0;
}

void reflector_stop(reflector_t *r)
{
    atomic_store(&r->should_run, false);
    if (r->net_running) {
        pthread_join(r->net_thread, NULL);
        r->net_running = false;
    }
    /* udp thread is joined inside run_connection's teardown */
}

void reflector_free(reflector_t *r)
{
    if (!r) return;
    reflector_stop(r);
    if (r->rx_ring) frame_ring_free(r->rx_ring);
    if (r->ev_eventfd >= 0) close(r->ev_eventfd);
    if (r->rx_eventfd >= 0) close(r->rx_eventfd);
    free(r->rxbuf);
    free(r->framebuf);
    pthread_mutex_destroy(&r->lock);
    free(r);
}
