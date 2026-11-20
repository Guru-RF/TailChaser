/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "refmsg.h"
#include "util.h"

#include <string.h>

/* ---- builders ---- */

int refmsg_build_proto_ver(uint8_t *out, size_t cap, uint16_t major, uint16_t minor)
{
    if (cap < 6) return -1;
    wr_be16(out + 0, RT_PROTO_VER);
    wr_be16(out + 2, major);
    wr_be16(out + 4, minor);
    return 6;
}

static int build_type_only(uint8_t *out, size_t cap, uint16_t type)
{
    if (cap < 2) return -1;
    wr_be16(out, type);
    return 2;
}

int refmsg_build_heartbeat(uint8_t *out, size_t cap)
{
    return build_type_only(out, cap, RT_HEARTBEAT);
}

int refmsg_build_ca_bundle_request(uint8_t *out, size_t cap)
{
    return build_type_only(out, cap, RT_CA_BUNDLE_REQUEST);
}

int refmsg_build_start_encryption_request(uint8_t *out, size_t cap)
{
    return build_type_only(out, cap, RT_START_ENCRYPTION_REQUEST);
}

int refmsg_build_client_csr(uint8_t *out, size_t cap, const char *csr_pem)
{
    size_t slen = strlen(csr_pem);
    if (slen > 0xffff) return -1;
    size_t need = 2 + 2 + slen;
    if (cap < need) return -1;
    wr_be16(out + 0, RT_CLIENT_CSR);
    wr_be16(out + 2, (uint16_t)slen);
    memcpy(out + 4, csr_pem, slen);
    return (int)need;
}

int refmsg_build_auth_response(uint8_t *out, size_t cap, const char *callsign,
                               const uint8_t *digest, size_t digest_len)
{
    size_t cslen = strlen(callsign);
    if (cslen > 0xffff || digest_len > 0xffff) return -1;
    /* MsgAuthResponse = ASYNC_MSG_MEMBERS(m_callsign, m_digest): a std::string
     * then a std::vector<uint8_t>, each packed as [u16 len][data]. The digest
     * MUST carry its own u16 length prefix (a fixed 20 raw bytes is rejected as
     * "Illegal MsgAuthResponse"). */
    size_t need = 2 + 2 + cslen + 2 + digest_len;
    if (cap < need) return -1;
    wr_be16(out + 0, RT_AUTH_RESPONSE);
    wr_be16(out + 2, (uint16_t)cslen);
    memcpy(out + 4, callsign, cslen);
    wr_be16(out + 4 + cslen, (uint16_t)digest_len);
    if (digest_len)
        memcpy(out + 6 + cslen, digest, digest_len);
    return (int)need;
}

int refmsg_build_node_info(uint8_t *out, size_t cap,
                           const uint8_t *iv, size_t iv_len,
                           const uint8_t *key, size_t key_len,
                           const char *json)
{
    size_t jlen = strlen(json);
    if (iv_len > 0xffff || key_len > 0xffff || jlen > 0xffff) return -1;
    size_t need = 2 + (2 + iv_len) + (2 + key_len) + (2 + jlen);
    if (cap < need) return -1;
    size_t o = 0;
    wr_be16(out + o, RT_NODE_INFO); o += 2;   /* MsgNodeInfo = 111 */
    wr_be16(out + o, (uint16_t)iv_len);  o += 2;
    memcpy(out + o, iv, iv_len);         o += iv_len;
    wr_be16(out + o, (uint16_t)key_len); o += 2;
    memcpy(out + o, key, key_len);       o += key_len;
    wr_be16(out + o, (uint16_t)jlen);    o += 2;
    memcpy(out + o, json, jlen);         o += jlen;
    return (int)o;
}

int refmsg_build_select_tg(uint8_t *out, size_t cap, uint32_t tg)
{
    if (cap < 6) return -1;
    wr_be16(out + 0, RT_SELECT_TG);
    wr_be32(out + 2, tg);
    return 6;
}

int refmsg_build_tg_monitor(uint8_t *out, size_t cap, const uint32_t *tgs, int n)
{
    if (n < 0) n = 0;
    if (n > 256) n = 256;
    uint32_t sorted[256];
    for (int i = 0; i < n; i++) sorted[i] = tgs[i];
    /* ascending insertion sort (n is tiny) */
    for (int i = 1; i < n; i++) {
        uint32_t k = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > k) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = k;
    }
    size_t need = 2 + 2 + (size_t)n * 4;
    if (cap < need) return -1;
    wr_be16(out + 0, RT_TG_MONITOR);
    wr_be16(out + 2, (uint16_t)n);
    for (int i = 0; i < n; i++)
        wr_be32(out + 4 + (size_t)i * 4, sorted[i]);
    return (int)need;
}

/* ---- parsers ---- */

int refmsg_type(const uint8_t *p, size_t plen)
{
    if (plen < 2) return -1;
    return (int)rd_be16(p);
}

int refmsg_parse_auth_challenge(const uint8_t *p, size_t plen, uint8_t out20[20])
{
    /* MsgAuthChallenge = [type(2)][m_challenge]. m_challenge is a
     * std::vector<uint8_t> packed as [u16 len][bytes], so the challenge starts
     * at p+4, NOT p+2. (For cert auth the challenge is unused, so this slipped
     * by; authkey HMAC needs the real bytes.) */
    if (plen < 4) return -1;
    uint16_t clen = (uint16_t)((p[2] << 8) | p[3]);
    if (clen != 20 || plen < 4u + 20u) return -1;
    memcpy(out20, p + 4, 20);
    return 0;
}

int refmsg_parse_server_info_client_id(const uint8_t *p, size_t plen, uint16_t *client_id)
{
    /* type(2) reserved(2) clientID(2) ... */
    if (plen < 6) return -1;
    *client_id = rd_be16(p + 4);
    return 0;
}

int refmsg_parse_string_at(const uint8_t *p, size_t plen, size_t off,
                           char *out, size_t cap)
{
    if (off + 2 > plen) return -1;
    size_t len = rd_be16(p + off);
    size_t start = off + 2;
    if (start + len > plen) return -1;
    size_t n = (len < cap - 1) ? len : (cap ? cap - 1 : 0);
    if (cap) {
        memcpy(out, p + start, n);
        out[n] = '\0';
    }
    return 0;
}

int refmsg_parse_talker(const uint8_t *p, size_t plen,
                        uint32_t *tg, char *cs, size_t cs_cap)
{
    if (plen < 6) return -1;
    *tg = rd_be32(p + 2);
    return refmsg_parse_string_at(p, plen, 6, cs, cs_cap);
}

int refmsg_parse_start_udp_enc(const uint8_t *p, size_t plen,
                               uint8_t *iv, size_t *iv_len,
                               uint8_t *key, size_t *key_len)
{
    if (plen <= 4)
        return 1;   /* empty/short => reuse TX cipher for RX */
    /* Faithful to the Swift parse: iv_rand(4) + key(16) after the type.
     * NOTE: be.svx.link uses the empty/reuse path above; this branch is
     * [NEEDS LIVE VERIFICATION]. refcrypto zero-pads the IV random to 6. */
    if (plen < 2 + 4 + 16) return -1;
    memcpy(iv, p + 2, 4);   *iv_len = 4;
    memcpy(key, p + 6, 16); *key_len = 16;
    return 0;
}

/* Manual substring search in a byte range. */
static int find_bytes(const uint8_t *hay, size_t hlen, size_t from,
                      const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || hlen < nlen) return -1;
    for (size_t i = from; i + nlen <= hlen; i++) {
        if (memcmp(hay + i, needle, nlen) == 0)
            return (int)i;
    }
    return -1;
}

int refmsg_extract_pem(const uint8_t *p, size_t plen, char *out, size_t cap)
{
    size_t outlen = 0;
    size_t from = 0;
    if (cap == 0) return -1;
    out[0] = '\0';

    while (from < plen) {
        int beg = find_bytes(p, plen, from, "-----BEGIN");
        if (beg < 0) break;
        int end = find_bytes(p, plen, (size_t)beg, "-----END");
        if (end < 0) break;
        /* advance to end-of-line after the END marker */
        size_t line_end = (size_t)end;
        while (line_end < plen && p[line_end] != 0x0A) line_end++;
        if (line_end < plen) line_end++;   /* include the newline */

        size_t block = line_end - (size_t)beg;
        int need_nl = (line_end == 0 || p[line_end - 1] != 0x0A);
        if (outlen + block + (need_nl ? 1 : 0) + 1 > cap)   /* +1 NUL */
            return -1;
        memcpy(out + outlen, p + beg, block);
        outlen += block;
        if (need_nl) out[outlen++] = '\n';
        out[outlen] = '\0';
        from = line_end;
    }
    return outlen ? (int)outlen : -1;
}
