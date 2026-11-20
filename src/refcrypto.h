/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — SvxReflector v3 UDP crypto (AES-128-GCM, OpenSSL EVP).
 *
 * Mirrors SVXConnect-OSX CryptoManager.swift. Per-direction keys:
 *   TX (client->server): we generate iv_rand(6)+key(16), send in MsgNodeInfo.
 *   RX (server->client): server confirms with an (usually empty)
 *       MsgStartUDPEncryption -> we reuse our TX cipher for RX.
 *
 * Wire datagram (both directions):  [ AAD ][ TAG(8) ][ CIPHERTEXT ]
 *   IV (12)  = iv_rand(6) || client_id BE(2) || counter BE(4)
 *   AAD      = counter BE(4)              (subsequent packets)
 *            = counter BE(4) || client_id BE(2)   (first TX packet only)
 *   TAG      = first 8 bytes of the 16-byte GCM tag
 * TX uses our assigned client_id in the IV; RX uses client_id = 0x0000.
 *
 * [NEEDS LIVE VERIFICATION] The first-packet AAD, the RX client_id=0 and the
 * empty-StartUDPEncryption reuse were reverse-engineered in SVXConnect against
 * be.svx.link. Validate with refcrypto_selftest() (mechanics) and a live
 * capture before trusting end-to-end.
 */
#ifndef SMARTSPOT_REFCRYPTO_H
#define SMARTSPOT_REFCRYPTO_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t  tx_key[16];
    uint8_t  tx_iv_rand[6];
    uint8_t  rx_key[16];
    uint8_t  rx_iv_rand[6];
    uint16_t client_id;
    uint32_t tx_counter;
    int      tx_ready;
    int      rx_ready;
    int      sent_initial;     /* set after the first (initial-AAD) TX packet */
} refcrypto_t;

void refcrypto_init(refcrypto_t *rc);

/* Generate fresh TX iv_rand(6) + key(16); reset counter + sent_initial.
 * Copies the generated material out for MsgNodeInfo. Returns 0 / -1. */
int  refcrypto_gen_tx(refcrypto_t *rc, uint16_t client_id,
                      uint8_t out_iv6[6], uint8_t out_key16[16]);

/* Reuse the TX cipher for RX (empty MsgStartUDPEncryption). */
void refcrypto_use_tx_for_rx(refcrypto_t *rc);

/* Configure RX from explicit MsgStartUDPEncryption params (iv zero-padded to
 * 6 bytes). Returns 0 / -1. */
int  refcrypto_set_rx(refcrypto_t *rc, const uint8_t *iv, size_t iv_len,
                      const uint8_t *key, size_t key_len);

int  refcrypto_rx_ready(const refcrypto_t *rc);

/* Encrypt plaintext -> out (AAD|TAG8|CT). Returns wire length or -1. */
int  refcrypto_encrypt(refcrypto_t *rc, const uint8_t *pt, size_t pt_len,
                       uint8_t *out, size_t out_cap);

/* Decrypt wire -> out plaintext. Returns plaintext length, or -1 on
 * auth failure / malformed input. */
int  refcrypto_decrypt(refcrypto_t *rc, const uint8_t *wire, size_t wire_len,
                       uint8_t *out, size_t out_cap);

/* Round-trip self-test of the GCM mechanics + truncated-tag handling
 * (client_id=0 so TX/RX IVs align). Returns 0 on success, -1 on failure. */
int  refcrypto_selftest(void);

#endif /* SMARTSPOT_REFCRYPTO_H */
