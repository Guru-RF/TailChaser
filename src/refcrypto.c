/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "refcrypto.h"
#include "util.h"
#include "log.h"

#include <string.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

void refcrypto_init(refcrypto_t *rc)
{
    memset(rc, 0, sizeof *rc);
}

int refcrypto_gen_tx(refcrypto_t *rc, uint16_t client_id,
                     uint8_t out_iv6[6], uint8_t out_key16[16])
{
    if (RAND_bytes(rc->tx_iv_rand, 6) != 1) return -1;
    if (RAND_bytes(rc->tx_key, 16) != 1) return -1;
    rc->client_id    = client_id;
    rc->tx_counter   = 0;
    rc->sent_initial = 0;
    rc->tx_ready     = 1;
    memcpy(out_iv6, rc->tx_iv_rand, 6);
    memcpy(out_key16, rc->tx_key, 16);
    return 0;
}

void refcrypto_use_tx_for_rx(refcrypto_t *rc)
{
    memcpy(rc->rx_key, rc->tx_key, 16);
    memcpy(rc->rx_iv_rand, rc->tx_iv_rand, 6);
    rc->rx_ready = rc->tx_ready;
}

int refcrypto_set_rx(refcrypto_t *rc, const uint8_t *iv, size_t iv_len,
                     const uint8_t *key, size_t key_len)
{
    if (key_len != 16) return -1;
    memset(rc->rx_iv_rand, 0, 6);
    memcpy(rc->rx_iv_rand, iv, iv_len < 6 ? iv_len : 6);   /* zero-pad to 6 */
    memcpy(rc->rx_key, key, 16);
    rc->rx_ready = 1;
    return 0;
}

int refcrypto_rx_ready(const refcrypto_t *rc) { return rc->rx_ready; }

/* Build the 12-byte IV: iv_rand(6) || client_id BE(2) || counter BE(4). */
static void build_iv(uint8_t iv[12], const uint8_t iv_rand[6],
                     uint16_t client_id, uint32_t counter)
{
    memcpy(iv, iv_rand, 6);
    wr_be16(iv + 6, client_id);
    wr_be32(iv + 8, counter);
}

int refcrypto_encrypt(refcrypto_t *rc, const uint8_t *pt, size_t pt_len,
                      uint8_t *out, size_t out_cap)
{
    if (!rc->tx_ready) return -1;

    int initial = !rc->sent_initial;
    uint32_t counter = initial ? 0u : rc->tx_counter;

    uint8_t iv[12];
    build_iv(iv, rc->tx_iv_rand, rc->client_id, counter);

    uint8_t aad[6];
    size_t aad_len = 4;
    wr_be32(aad, counter);
    if (initial) {
        wr_be16(aad + 4, rc->client_id);
        aad_len = 6;
    }

    if (out_cap < aad_len + 8 + pt_len) return -1;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int ok = 0, len = 0;
    uint8_t tag16[16];
    uint8_t *ct = out + aad_len + 8;
    int ct_len = 0;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, NULL, NULL) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1) goto done;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, rc->tx_key, iv) != 1) goto done;
    if (EVP_EncryptUpdate(ctx, NULL, &len, aad, (int)aad_len) != 1) goto done;
    if (pt_len > 0) {
        if (EVP_EncryptUpdate(ctx, ct, &len, pt, (int)pt_len) != 1) goto done;
        ct_len = len;
    }
    if (EVP_EncryptFinal_ex(ctx, ct + ct_len, &len) != 1) goto done;
    ct_len += len;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag16) != 1) goto done;

    memcpy(out, aad, aad_len);
    memcpy(out + aad_len, tag16, 8);     /* truncated 8-byte tag */
    ok = 1;

done:
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return -1;

    if (initial) {
        rc->tx_counter   = 1;
        rc->sent_initial = 1;
    } else {
        rc->tx_counter += 1;
    }
    return (int)(aad_len + 8 + (size_t)ct_len);
}

int refcrypto_decrypt(refcrypto_t *rc, const uint8_t *wire, size_t wire_len,
                      uint8_t *out, size_t out_cap)
{
    if (!rc->rx_ready) return -1;
    if (wire_len < 4 + 8) return -1;          /* AAD(4) + TAG(8) */

    uint32_t counter = rd_be32(wire);
    const uint8_t *aad = wire;                /* 4 bytes */
    uint8_t tag[8];
    memcpy(tag, wire + 4, 8);
    const uint8_t *ct = wire + 12;
    size_t ct_len = wire_len - 12;
    if (out_cap < ct_len) return -1;

    uint8_t iv[12];
    build_iv(iv, rc->rx_iv_rand, 0 /* server -> client uses client_id 0 */, counter);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int ok = 0, len = 0, out_len = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, NULL, NULL) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1) goto done;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, rc->rx_key, iv) != 1) goto done;
    if (EVP_DecryptUpdate(ctx, NULL, &len, aad, 4) != 1) goto done;
    if (ct_len > 0) {
        if (EVP_DecryptUpdate(ctx, out, &len, ct, (int)ct_len) != 1) goto done;
        out_len = len;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 8, tag) != 1) goto done;
    /* DecryptFinal verifies the (truncated) tag: >0 = authentic. */
    if (EVP_DecryptFinal_ex(ctx, out + out_len, &len) != 1) goto done;
    out_len += len;
    ok = 1;

done:
    EVP_CIPHER_CTX_free(ctx);
    return ok ? out_len : -1;
}

int refcrypto_selftest(void)
{
    refcrypto_t rc;
    refcrypto_init(&rc);

    uint8_t iv[6], key[16];
    if (refcrypto_gen_tx(&rc, 0 /* client_id 0 so TX/RX IVs align */, iv, key) != 0) {
        log_error("refcrypto selftest: gen_tx failed");
        return -1;
    }
    refcrypto_use_tx_for_rx(&rc);

    const uint8_t pt[] = "SmartSPOT refcrypto round-trip";
    size_t pt_len = sizeof pt;   /* include the trailing NUL — arbitrary payload */
    uint8_t wire[256], out[256];

    /* First packet uses the initial 6-byte AAD (server-side format); skip it. */
    if (refcrypto_encrypt(&rc, pt, pt_len, wire, sizeof wire) < 0) {
        log_error("refcrypto selftest: initial encrypt failed");
        return -1;
    }
    /* Second packet uses the 4-byte-AAD format our decrypt() expects. */
    int w = refcrypto_encrypt(&rc, pt, pt_len, wire, sizeof wire);
    if (w < 0) { log_error("refcrypto selftest: encrypt failed"); return -1; }

    int o = refcrypto_decrypt(&rc, wire, (size_t)w, out, sizeof out);
    if (o != (int)pt_len || memcmp(out, pt, pt_len) != 0) {
        log_error("refcrypto selftest: round-trip mismatch (o=%d)", o);
        return -1;
    }

    /* Tamper one ciphertext byte — GCM must reject it. */
    wire[w - 1] ^= 0xff;
    if (refcrypto_decrypt(&rc, wire, (size_t)w, out, sizeof out) >= 0) {
        log_error("refcrypto selftest: tampered packet was NOT rejected");
        return -1;
    }

    log_info("refcrypto selftest: OK (AES-128-GCM round-trip + tamper-reject)");
    return 0;
}
