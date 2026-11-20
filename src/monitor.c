/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "monitor.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include "log.h"

#define MAX_LISTENERS 4
#define STORE         32000         /* samples: 2 s */
#define CHUNK         1600          /* samples per message: 100 ms */

typedef struct {
    monitor_t      *m;
    int             fd, which;
    bool            used, quit;
    pthread_t       thread;
    int16_t         buf[STORE];
    int             head, len;
} listener_t;

struct monitor {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    listener_t      l[MAX_LISTENERS];
    atomic_int      n[2];           /* listeners per stream */
};

monitor_t *monitor_new(void)
{
    monitor_t *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    pthread_mutex_init(&m->mu, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&m->cv, &ca);
    pthread_condattr_destroy(&ca);
    for (int i = 0; i < MAX_LISTENERS; i++)
        m->l[i].fd = -1;
    return m;
}

void monitor_feed(monitor_t *m, int which, const int16_t *pcm, int n)
{
    if (!m || atomic_load(&m->n[which]) == 0)
        return;
    pthread_mutex_lock(&m->mu);
    for (int i = 0; i < MAX_LISTENERS; i++) {
        listener_t *l = &m->l[i];
        if (!l->used || l->which != which)
            continue;
        for (int k = 0; k < n; k++) {
            if (l->len == STORE) {              /* a slow listener loses the oldest */
                l->head = (l->head + 1) % STORE;
                l->len--;
            }
            l->buf[(l->head + l->len++) % STORE] = pcm[k];
        }
    }
    pthread_cond_broadcast(&m->cv);
    pthread_mutex_unlock(&m->mu);
}

static int ws_out(int fd, int op, const void *p, size_t n)
{
    uint8_t h[4];
    size_t hl = 2;
    h[0] = (uint8_t)(0x80 | op);
    if (n < 126) {
        h[1] = (uint8_t)n;
    } else {
        h[1] = 126;
        h[2] = (uint8_t)(n >> 8);
        h[3] = (uint8_t)n;
        hl = 4;
    }
    struct iovec { const void *b; size_t l; } v[2] = { { h, hl }, { p, n } };
    for (int i = 0; i < 2; i++) {
        const uint8_t *b = v[i].b;
        size_t left = v[i].l;
        while (left > 0) {
            ssize_t w = send(fd, b, left, MSG_NOSIGNAL);
            if (w <= 0)
                return -1;
            b += w;
            left -= (size_t)w;
        }
    }
    return 0;
}

static void *listener_main(void *arg)
{
    listener_t *l = arg;
    monitor_t *m = l->m;
    int16_t out[CHUNK];
    int idle = 0;
    for (;;) {
        pthread_mutex_lock(&m->mu);
        if (l->len < CHUNK && !l->quit) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_nsec += 250000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_nsec -= 1000000000L; ts.tv_sec++; }
            pthread_cond_timedwait(&m->cv, &m->mu, &ts);
        }
        bool quit = l->quit;
        /* whole chunks while audio flows; the rest of an over once it has stopped */
        int n = l->len >= CHUNK ? CHUNK : idle >= 1 ? l->len : 0;
        for (int i = 0; i < n; i++)
            out[i] = l->buf[(l->head + i) % STORE];
        l->head = (l->head + n) % STORE;
        l->len -= n;
        pthread_mutex_unlock(&m->mu);
        if (quit)
            break;
        if (n > 0) {
            idle = 0;
            if (ws_out(l->fd, 2, out, sizeof(int16_t) * (size_t)n) != 0)     /* little-endian hosts only */
                break;
        } else if (++idle % 20 == 0 && ws_out(l->fd, 9, "", 0) != 0) {       /* a ping every 5 s */
            break;
        }
        /* what the browser says: nothing we need — but a close, or the end */
        struct pollfd p = { l->fd, POLLIN, 0 };
        if (poll(&p, 1, 0) > 0) {
            uint8_t in[256];
            ssize_t r = recv(l->fd, in, sizeof in, MSG_DONTWAIT);
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR) || (r > 0 && (in[0] & 15) == 8))
                break;
        }
    }
    pthread_mutex_lock(&m->mu);
    close(l->fd);
    l->fd = -1;
    l->used = false;
    atomic_fetch_sub(&m->n[l->which], 1);
    pthread_mutex_unlock(&m->mu);
    log_info("monitor: a listener on the %s left", l->which == MONITOR_RX ? "input" : "output");
    return NULL;
}

bool monitor_accept(monitor_t *m, int fd, int which, const char *ws_key)
{
    if (!m || !ws_key[0] || strlen(ws_key) > 64)
        return false;
    pthread_mutex_lock(&m->mu);
    listener_t *l = NULL;
    for (int i = 0; i < MAX_LISTENERS && !l; i++)
        if (!m->l[i].used && m->l[i].fd < 0)
            l = &m->l[i];
    if (l) {
        l->used = true;
        l->quit = false;
        l->m = m;
        l->fd = fd;
        l->which = which;
        l->head = l->len = 0;
    }
    pthread_mutex_unlock(&m->mu);
    if (!l)
        return false;                           /* full: the caller says so */

    char cat[128], acc[64], resp[256];
    unsigned char sha[SHA_DIGEST_LENGTH];
    int cn = snprintf(cat, sizeof cat, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", ws_key);
    SHA1((const unsigned char *)cat, (size_t)cn, sha);
    EVP_EncodeBlock((unsigned char *)acc, sha, sizeof sha);
    int rn = snprintf(resp, sizeof resp, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                      "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    bool ok = send(fd, resp, (size_t)rn, MSG_NOSIGNAL) == rn;
    if (ok) {
        atomic_fetch_add(&m->n[which], 1);
        ok = pthread_create(&l->thread, &at, listener_main, l) == 0;
        if (!ok)
            atomic_fetch_sub(&m->n[which], 1);
    }
    pthread_attr_destroy(&at);
    if (!ok) {
        pthread_mutex_lock(&m->mu);
        l->used = false;
        l->fd = -1;
        pthread_mutex_unlock(&m->mu);
        return false;
    }
    log_info("monitor: somebody listens to the %s", which == MONITOR_RX ? "input" : "output");
    return true;
}

void monitor_free(monitor_t *m)
{
    if (!m)
        return;
    pthread_mutex_lock(&m->mu);
    for (int i = 0; i < MAX_LISTENERS; i++)
        m->l[i].quit = true;
    pthread_cond_broadcast(&m->cv);
    pthread_mutex_unlock(&m->mu);
    for (int tries = 0; tries < 20 && (atomic_load(&m->n[0]) || atomic_load(&m->n[1])); tries++)
        usleep(50000);
    /* the threads are detached; the little that is left goes with the process */
}
