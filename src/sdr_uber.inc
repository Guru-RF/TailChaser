/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* UberSDR (ka9q_ubersdr). Part of sdr.c.
 *
 *   POST /connection  {"user_session_id": "<uuid>"}       -> {"allowed": true, ...}
 *   ws://host/ws?frequency=<Hz>&mode=nfm&bandwidthLow=..&bandwidthHigh=..
 *               &format=opus&version=2&user_session_id=<uuid>
 *   it sends binary, one Opus packet each, all little-endian:
 *     time(8) sample rate(4) channels(1) power(4, float dBFS) noise(4) Opus...
 *   and JSON text ({"type":"error",...}); we send {"type":"ping"}.
 *
 * Its other audio format is PCM in zstd; a server built without Opus sends
 * that whatever is asked for, and is not supported here.
 */

static int uber_open(sdr_t *s)
{
    uint8_t r[16];
    RAND_bytes(r, sizeof r);
    r[6] = (uint8_t)((r[6] & 0x0f) | 0x40);
    r[8] = (uint8_t)((r[8] & 0x3f) | 0x80);
    snprintf(s->session, sizeof s->session,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
    size_t l = strlen(s->path);
    int bl = (int)(l && s->path[l - 1] == '/' ? l - 1 : l);

    char body[200], req[700];
    int bn = snprintf(body, sizeof body, "{\"user_session_id\":\"%s\",\"password\":\"%s\"}", s->session, s->cfg.password);
    int n = snprintf(req, sizeof req,
                     "POST %.*s/connection HTTP/1.1\r\nHost: %s\r\nUser-Agent: TailChaser-SDRReceiver/" TAILCHASER_VERSION
                     "\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
                     bl, s->path, s->host, bn, body);
    int code = http_exchange(s, req, (size_t)n, true);
    const char *ans = code > 0 ? strstr((char *)s->in, "\r\n\r\n") : NULL;
    if (!ans || !strstr(ans, "\"allowed\":true")) {
        char why[100] = "";
        if (ans) jstr(ans, "reason", why, sizeof why);
        if (code > 0) {
            say(s, false, false, "%s refuses (HTTP %d)%s%s", s->host, code, why[0] ? ": " : "", why);
            s->retry_s = code == 429 || code == 503 ? 120 : 60;
        }
        return -1;
    }
    io_close(s);
    if (io_connect(s, s->host, s->port, s->tls) != 0)
        return -1;

    char path[420];
    int half = s->cfg.bandwidth_hz / 2;
    snprintf(path, sizeof path, "%.*s/ws?frequency=%ld&mode=nfm&bandwidthLow=%d&bandwidthHigh=%d&format=opus&version=2"
             "&user_session_id=%s%s%s", bl, s->path, lrint(s->cfg.freq_hz), -half, half, s->session,
             s->cfg.password[0] ? "&password=" : "", s->cfg.password);
    if (ws_handshake(s, path, NULL) != 0)
        return -1;
    s->next_ka = mono_ms() + 10000;
    if (ws_text(s, "{\"type\":\"set_squelch\",\"squelchOpen\":-999}") != 0)
        return -1;
    say(s, true, true, "listening on %.4f MHz — %s", s->cfg.freq_hz / 1e6, s->host);
    return 0;
}

static int uber_message(sdr_t *s, int op, uint8_t *p, size_t n)
{
    if (op == 1) {
        char type[24] = "", why[120] = "";
        if (jstr((char *)p, "type", type, sizeof type) && !strcmp(type, "error")) {
            jstr((char *)p, "error", why, sizeof why);
            say(s, false, false, "%s: %s", s->host, why[0] ? why : "an error");
            s->retry_s = strstr(why, "terminated") ? 5 : 60;       /* the hourly session limit: come back */
            return -1;
        }
        return 0;
    }
    if (n >= 4 && p[0] == 0x28 && p[1] == 0xb5 && p[2] == 0x2f && p[3] == 0xfd) {
        say(s, false, false, "%s sends PCM in zstd: it was built without Opus, which this program needs", s->host);
        s->retry_s = 3600;
        return -1;
    }
    if (n < 22)
        return 0;
    int rate = (int)((uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24));
    if ((rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000) || p[12] != 1)
        return 0;
    float power;
    memcpy(&power, p + 13, 4);
    pthread_mutex_lock(&s->mu);
    s->st.smeter_db = power;
    pthread_mutex_unlock(&s->mu);
    if (!s->opus || s->opus_rate != rate) {
        int err = 0;
        if (s->opus) opus_decoder_destroy(s->opus);
        s->opus = opus_decoder_create(rate, 1, &err);
        s->opus_rate = rate;
        if (!s->opus)
            return -1;
    }
    int16_t pcm[5760];
    int got = opus_decode(s->opus, p + 21, (opus_int32)(n - 21), pcm, 5760, 0);
    if (got > 0)
        emit(s, pcm, got, rate);
    return 0;
}

static int uber_tick(sdr_t *s, uint64_t now)
{
    if (now >= s->next_ka) {
        s->next_ka = now + 10000;
        return ws_text(s, "{\"type\":\"ping\"}");
    }
    return 0;
}

static const sdr_backend_t BE_UBER = { "ubersdr", uber_open, uber_message, uber_tick };
