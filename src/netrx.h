/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — the link from a transmitter to its receiver (client side).
 *
 * The counterpart of nettrx.c: it connects to a tailchaser-receiver over TCP
 * and speaks the RemoteTrx protocol the way an SvxLink repeater's `TYPE=Net`
 * receiver does — log in, pick a codec, un-mute — and is then told when the
 * squelch opens and closes, how strong the signal is, which DTMF digits were
 * pressed, and is sent the audio.
 *
 * Non-blocking, driven from the caller's poll loop. It reconnects by itself.
 * Whatever protocol version the receiver announces (2.7 or 2.8) is accepted.
 *
 * Audio is delivered as 16 kHz mono 16-bit PCM in pieces of any length; the
 * caller frames it.
 */
#ifndef TAILCHASER_NETRX_H
#define TAILCHASER_NETRX_H

#include <stdbool.h>
#include <stdint.h>

typedef struct netrx netrx_t;

typedef struct {
    char host[128];
    int  port;
    char auth_key[128];
    char codec[8];          /* "S16" | "RAW" | "OPUS" */
    int  sample_rate;
} netrx_cfg_t;

typedef struct {
    void *ctx;
    void (*squelch)(void *ctx, bool open, float siglev);
    void (*audio)(void *ctx, const int16_t *pcm, int n);
    void (*dtmf)(void *ctx, char digit, int duration_ms);
    void (*siglev)(void *ctx, float siglev);
} netrx_sink_t;

typedef struct {
    bool connected;         /* logged in                               */
    char peer[160];         /* "host:port"                             */
    char codec[8];
    int  proto_major, proto_minor;  /* what the receiver announced     */
    bool squelch_open;
    float siglev;
    char rx_id;
    unsigned long connects, auth_failures, audio_frames;
} netrx_status_t;

netrx_t *netrx_new(const netrx_cfg_t *cfg, const netrx_sink_t *sink);
void     netrx_free(netrx_t *n);

/* poll() integration: fd is -1 while waiting to reconnect. */
int  netrx_fd(const netrx_t *n);
bool netrx_want_write(const netrx_t *n);
void netrx_on_readable(netrx_t *n);
void netrx_on_writable(netrx_t *n);
void netrx_tick(netrx_t *n, uint64_t now_ms);   /* connect, heartbeats, time-outs; call >= 5/s */

void netrx_get_status(const netrx_t *n, netrx_status_t *out);

#endif /* TAILCHASER_NETRX_H */
