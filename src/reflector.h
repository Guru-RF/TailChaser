/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — SvxReflector client (protocol 3, with the protocol-2
 * fallback).
 *
 * Ties together srv + tls + pki + refmsg + refcrypto into the full client:
 * SRV resolve -> TCP connect -> ProtoVer -> CA bundle -> STARTTLS -> (cert
 * enrollment via CSR if needed) -> auth -> ServerInfo -> NodeInfo ->
 * StartUDPEncryption -> UDP audio.
 *
 * Runs a network thread (TCP control over TLS) and, once UDP is up, a UDP
 * receive thread. The main loop drives it through the thread-safe API below:
 * it sends the received RF audio out (reflector_send_audio), drains network
 * audio (reflector_rx_audio_pop) and connection/talker events
 * (reflector_get_event), and switches talkgroups (reflector_select_tg).
 *
 * From SmartSPOT (src/reflector.c), where it runs against be.svx.link. Changes
 * here: the end of an incoming over is passed in-band (a zero-length frame),
 * commands from the main loop go out within 200 ms instead of 1 s, a status
 * getter, and the hotspot-only monitor juggling is gone.
 */
#ifndef SVXSR_REFLECTOR_H
#define SVXSR_REFLECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include "refmsg.h"

#define REFLECTOR_MAX_MONITOR 64
#define REFLECTOR_MAX_OPUS    512     /* largest Opus packet accepted / sent */

typedef struct {
    char     domain[128];        /* SRV lookup: _svxreflector._tcp.<domain>   */
    char     host[256];          /* or static "h:p,h:p" (used when no SRV)    */
    int      port;
    char     callsign[32];       /* the node's login, e.g. ON0ORA-S           */
    char     auth_key[128];      /* password reflectors only ("" = cert auth) */
    bool     use_authkey;        /* true: auth via HMAC-SHA1(auth_key, challenge) */
    char     email[96];          /* goes into the certificate request         */
    char     state_dir[256];     /* <CALLSIGN>.key/.csr/.crt + ca-bundle.crt  */
    char     location[64];
    char     sysop[32];
    char     latitude[24];       /* POSIX decimal string; empty = no fix */
    char     longitude[24];
    char     locator[16];
    char     qth_name[48];
    char     sw_version[32];
    char     rxtx_json[2200];     /* "rx":{...},"tx":{...} of the qth entry    */
    char     extra_json[96];     /* more top-level members, each with a trailing comma */
    bool     hidden;
    bool     verify_host;        /* the certificate must name the reflector   */
    uint32_t default_tg;
    uint32_t monitor_tgs[REFLECTOR_MAX_MONITOR];
    int      n_monitor;
    int      tcp_heartbeat_ms;
    int      udp_heartbeat_ms;
    int      reconnect_max_ms;
} reflector_cfg_t;

typedef enum {
    RE_CONNECTED,
    RE_DISCONNECTED,
    RE_TALKER_START,
    RE_TALKER_STOP,
} reflector_event_type_t;

typedef struct {
    reflector_event_type_t type;
    uint32_t               tg;
    char                   callsign[32];
} reflector_event_t;

typedef struct {
    bool          connected;        /* logged in                               */
    bool          udp_ready;        /* audio path up                           */
    bool          waiting_for_cert; /* CSR sent, the sysop has not signed yet  */
    int           proto_major;      /* 3 = TLS + encrypted UDP, 2 = legacy     */
    unsigned      client_id;
    char          host[256];        /* the server we are (or were last) on     */
    int           port;
    uint32_t      selected_tg;
    unsigned long connects;         /* successful logins since start           */
    unsigned long rx_frames;        /* audio frames received                   */
    unsigned long tx_frames;        /* audio frames sent                       */
} reflector_status_t;

typedef struct reflector reflector_t;

reflector_t *reflector_new(const reflector_cfg_t *cfg);
void         reflector_free(reflector_t *r);

int  reflector_start(reflector_t *r);   /* spawns the network thread */
void reflector_stop(reflector_t *r);    /* joins all threads */

bool reflector_connected(const reflector_t *r);
void reflector_get_status(reflector_t *r, reflector_status_t *out);

/* TX: send one Opus frame as MsgUdpAudio on the selected TG. 0/-1. */
int  reflector_send_audio(reflector_t *r, const uint8_t *opus, int len);
/* Tell the server we stopped transmitting (MsgUdpFlushSamples). */
void reflector_tx_stop(reflector_t *r);
/* Switch the selected talkgroup (+re-send the monitor list, +UDP heartbeat).
 * drop_in_flight: an over was coming in on the talkgroup being left; throw
 * away what is still on its way (250 ms). While not logged in the selection is
 * only remembered; it is applied when the session comes up. */
void reflector_select_tg(reflector_t *r, uint32_t tg, bool drop_in_flight);

/* What the receivers of the node hear and which one is listened to
 * (MsgSignalStrengthValues), and whether the transmitter is on (MsgTxStatus):
 * the reflector passes it on in its status, for the dashboards. The letters
 * are those of the node information. false = not logged in: say it again. */
bool reflector_send_rx_status(reflector_t *r, const refmsg_rx_t *rx, int n);
bool reflector_send_tx_status(reflector_t *r, char id, bool transmit);

/* Events (network thread -> main). Add the fd to poll(); drain with get_event. */
int  reflector_event_fd(const reflector_t *r);
int  reflector_get_event(reflector_t *r, reflector_event_t *out);   /* 0/-1 */

/* Network audio (UDP thread -> main): one Opus frame per call. Returns its
 * length, 0 for "the talker has stopped" (the server's MsgUdpFlushSamples), or
 * -1 when nothing is waiting. */
int  reflector_rx_audio_fd(const reflector_t *r);
int  reflector_rx_audio_pop(reflector_t *r, uint8_t *out, int cap);

#endif /* SVXSR_REFLECTOR_H */
