/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — SvxLink "Net" transceiver uplink (server side).
 *
 * Speaks the RemoteTrx TCP protocol that SvxLink's remotetrx NetUplink speaks,
 * so a repeater's `TYPE=Net` receiver connects to this box exactly as it
 * connects to a remotetrx. Receive direction only: we report squelch, signal
 * level, DTMF digits and audio; transmit-side messages from the repeater are
 * accepted and ignored.
 *
 * Wire format (svxlink/trx/NetTrxMsg.h): every message is a packed C struct
 * sent as-is, little-endian — { uint32 type; uint32 size; payload }. The
 * version handshake is an exact match, so `proto_minor` must be the version
 * the repeater's SvxLink was built with (2.7 up to 25.05.x; 2.8 adds a 128-byte
 * activity-info field to the squelch message).
 *
 * One client at a time, non-blocking, driven from the caller's poll loop.
 */
#ifndef SVXRX_NETTRX_H
#define SVXRX_NETTRX_H

#include <stdbool.h>
#include <stdint.h>

typedef struct nettrx nettrx_t;

typedef struct {
    int   listen_port;
    char  auth_key[128];
    int   proto_major, proto_minor;   /* minor 0 = auto: 8, falling back to 7 */
    char  rx_id;
    int   sample_rate;      /* of the PCM handed to nettrx_audio()   */
    float preamp_db;        /* software gain applied before encoding */
} nettrx_cfg_t;

typedef struct {
    bool connected;         /* a client is connected and authenticated */
    char peer[64];          /* "ip:port"                               */
    char codec[16];         /* RX audio codec the repeater selected    */
    int  mute_state;        /* 0 none, 1 content, 2 all                */
    uint64_t connected_since_ms;
    unsigned long connects; /* accepted + authenticated since start    */
    unsigned long auth_failures;
    unsigned long audio_frames_sent;
    int  proto_minor;       /* protocol minor version offered right now */
} nettrx_status_t;

nettrx_t *nettrx_new(const nettrx_cfg_t *cfg);
void      nettrx_free(nettrx_t *t);

/* poll() integration */
int  nettrx_listen_fd(const nettrx_t *t);
int  nettrx_client_fd(const nettrx_t *t);       /* -1 when nobody is connected */
bool nettrx_want_write(const nettrx_t *t);
void nettrx_on_accept(nettrx_t *t);
void nettrx_on_readable(nettrx_t *t);
void nettrx_on_writable(nettrx_t *t);
void nettrx_tick(nettrx_t *t, uint64_t now_ms); /* heartbeats + timeouts; call >= 1/s */

/* What the receiver reports. All are no-ops while nobody is connected. */
void nettrx_squelch(nettrx_t *t, bool open, float siglev);
void nettrx_siglev(nettrx_t *t, float siglev);
void nettrx_audio(nettrx_t *t, const int16_t *pcm, int n);
void nettrx_dtmf(nettrx_t *t, char digit, int duration_ms);

void nettrx_get_status(const nettrx_t *t, nettrx_status_t *out);

#endif /* SVXRX_NETTRX_H */
