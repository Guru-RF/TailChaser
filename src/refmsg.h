/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — SvxReflector v3 message framing (pack/parse).
 *
 * Mirrors SVXConnect-OSX/SVXConnect/Protocol/ReflectorMessages.swift, which in
 * turn matches SvxLink's ReflectorMsg.h. All integers are BIG-ENDIAN; strings
 * are [u16 length][utf8 bytes].
 *
 * The TCP transport frame (added by reflector.c, not here) is:
 *     [u32 BE frame_length][ payload ]
 * where `payload` is what these build_* functions produce and what the
 * parse_* functions consume: it begins with [u16 BE type].
 */
#ifndef SMARTSPOT_REFMSG_H
#define SMARTSPOT_REFMSG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Protocol version we speak. */
#define REFMSG_PROTO_MAJOR 3
#define REFMSG_PROTO_MINOR 0

/* TCP control message types (ReflectorMsg.h). */
enum {
    RT_HEARTBEAT               = 1,
    RT_PROTO_VER               = 5,
    RT_PROTO_VER_DOWNGRADE     = 6,
    RT_AUTH_CHALLENGE          = 10,
    RT_AUTH_RESPONSE           = 11,
    RT_AUTH_OK                 = 12,
    RT_ERROR                   = 13,
    RT_START_ENCRYPTION_REQUEST= 14,
    RT_START_ENCRYPTION        = 15,
    RT_CLIENT_CSR_REQUEST      = 16,
    RT_CLIENT_CSR              = 17,
    RT_CLIENT_CERT             = 18,
    RT_CA_INFO                 = 19,
    RT_CA_BUNDLE_REQUEST       = 20,
    RT_CA_BUNDLE_RESPONSE      = 21,
    RT_SERVER_INFO             = 100,
    RT_NODE_INFO               = 111,
    RT_NODE_JOINED             = 102,
    RT_NODE_LEFT               = 103,
    RT_TALKER_START            = 104,
    RT_TALKER_STOP             = 105,
    RT_SELECT_TG               = 106,
    RT_TG_MONITOR              = 107,
    RT_SIGNAL_STRENGTH         = 112,
    RT_TX_STATUS               = 113,
    RT_START_UDP_ENCRYPTION    = 114,
};

/* UDP message types. */
enum {
    RU_HEARTBEAT          = 1,
    RU_AUDIO              = 101,
    RU_FLUSH_SAMPLES      = 102,
    RU_ALL_SAMPLES_FLUSHED= 103,
};

/* ---- builders ----
 * Each writes the payload (starting with the u16 type) into `out` and returns
 * the number of bytes written, or -1 if it would not fit in `cap`. */
int refmsg_build_proto_ver(uint8_t *out, size_t cap, uint16_t major, uint16_t minor);
int refmsg_build_heartbeat(uint8_t *out, size_t cap);
int refmsg_build_ca_bundle_request(uint8_t *out, size_t cap);
int refmsg_build_start_encryption_request(uint8_t *out, size_t cap);
int refmsg_build_client_csr(uint8_t *out, size_t cap, const char *csr_pem);
int refmsg_build_auth_response(uint8_t *out, size_t cap, const char *callsign,
                               const uint8_t *digest, size_t digest_len);
int refmsg_build_node_info(uint8_t *out, size_t cap,
                           const uint8_t *iv, size_t iv_len,
                           const uint8_t *key, size_t key_len,
                           const char *json);
int refmsg_build_select_tg(uint8_t *out, size_t cap, uint32_t tg);
/* tgs is copied + sorted ascending internally (max 256). */
/* One receiver of the node, as the reflector shows it (MsgSignalStrengthValues):
 * its letter in the node information, 0..100, and what it is doing. */
typedef struct {
    char    id;
    uint8_t siglev;
    bool    enabled, sql_open, active;
} refmsg_rx_t;
int refmsg_build_siglev(uint8_t *out, size_t cap, const refmsg_rx_t *rx, int n);
/* MsgTxStatus: the transmitter with this letter is on the air, or not. */
int refmsg_build_tx_status(uint8_t *out, size_t cap, char id, bool transmit);
int refmsg_build_tg_monitor(uint8_t *out, size_t cap, const uint32_t *tgs, int n);

/* ---- parsers (operate on the payload: [u16 type][fields], length = plen) ---- */
/* Returns the message type, or -1 if the payload is too short. */
int refmsg_type(const uint8_t *p, size_t plen);

/* Copy the 20-byte auth challenge. Returns 0 on success, -1 otherwise. */
int refmsg_parse_auth_challenge(const uint8_t *p, size_t plen, uint8_t out20[20]);

/* Extract clientID (offset 4) from MsgServerInfo. Returns 0 / -1. */
int refmsg_parse_server_info_client_id(const uint8_t *p, size_t plen, uint16_t *client_id);

/* MsgTalkerStart/Stop: tg(u32 at off 2) + callsign string at off 6. 0 / -1. */
int refmsg_parse_talker(const uint8_t *p, size_t plen,
                        uint32_t *tg, char *cs, size_t cs_cap);

/* Generic: read the string at offset `off` (u16 len + utf8) into out. 0 / -1. */
int refmsg_parse_string_at(const uint8_t *p, size_t plen, size_t off,
                           char *out, size_t cap);

/* MsgStartUDPEncryption: if the payload carries explicit params
 * (iv_rand[4] + key[16] after the type), copy them and return 0; if it is
 * empty/short (server says "reuse my TX cipher"), return 1; -1 on malformed. */
int refmsg_parse_start_udp_enc(const uint8_t *p, size_t plen,
                               uint8_t *iv, size_t *iv_len,
                               uint8_t *key, size_t *key_len);

/* Concatenate every PEM block ("-----BEGIN..-----END\n") found in the payload
 * into out (NUL-terminated). Used for MsgCABundleResponse and MsgClientCert.
 * Returns the string length, or -1 if none found / overflow. */
int refmsg_extract_pem(const uint8_t *p, size_t plen, char *out, size_t cap);

#endif /* SMARTSPOT_REFMSG_H */
