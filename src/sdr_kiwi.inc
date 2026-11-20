/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* KiwiSDR and Web-888 (the same protocol; a Web-888 has other audio rates).
 * Part of sdr.c.
 *
 *   ws://host:8073/<time>/SND
 *   we send text:  SET auth..., SET AR OK..., SET mod=nbfm..., SET keepalive
 *   it sends binary, a 3-letter tag first:
 *     "MSG key=value key=value"
 *     "SND" flags(1) sequence(4 LE) S-meter(2 BE) audio (int16 BE)
 */

static bool kiwi_kv(const char *msg, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *p = msg; (p = strstr(p, key)) != NULL; p += kl) {
        if ((p == msg || p[-1] == ' ') && p[kl] == '=') {
            size_t n = strcspn(p + kl + 1, " \r\n");
            snprintf(out, cap, "%.*s", (int)n, p + kl + 1);
            return true;
        }
    }
    return false;
}

static int kiwi_open(sdr_t *s)
{
    char path[220];
    size_t l = strlen(s->path);
    snprintf(path, sizeof path, "%.*s/%ld/SND", (int)(l && s->path[l - 1] == '/' ? l - 1 : l), s->path,
             (long)util_wall_time());
    if (ws_handshake(s, path, NULL) != 0)
        return -1;
    s->kiwi_stage = 0;
    s->kiwi_rate = 12000;
    s->kiwi_offset_khz = 0.0;
    memset(&s->adpcm, 0, sizeof s->adpcm);
    s->next_ka = 0;
    if (ws_text(s, "SET auth t=kiwi p=%s", s->cfg.password[0] ? s->cfg.password : "#") != 0)
        return -1;
    say(s, true, false, "connected to %s", s->host);
    return 0;
}

static int kiwi_tune(sdr_t *s)
{
    int half = s->cfg.bandwidth_hz / 2;
    if (ws_text(s, "SET squelch=0 param=0.00") != 0 ||
        ws_text(s, "SET genattn=0") != 0 ||
        ws_text(s, "SET gen=0 mix=-1") != 0 ||
        ws_text(s, "SET mod=nbfm low_cut=%d high_cut=%d freq=%.3f", -half, half,
                s->cfg.freq_hz / 1000.0 - s->kiwi_offset_khz) != 0 ||
        ws_text(s, "SET agc=1 hang=0 thresh=-100 slope=6 decay=1000 manGain=50") != 0 ||
        ws_text(s, "SET compression=0") != 0 ||
        ws_text(s, "SET ident_user=%s", s->cfg.name[0] ? s->cfg.name : "TailChaser") != 0 ||
        ws_text(s, "SET keepalive") != 0)
        return -1;
    s->kiwi_stage = 2;
    s->last_audio = mono_ms();
    say(s, true, true, "listening on %.4f MHz — %s", s->cfg.freq_hz / 1e6, s->host);
    return 0;
}

static int kiwi_message(sdr_t *s, int op, uint8_t *p, size_t n)
{
    if (n < 3)
        return 0;
    if (!memcmp(p, "SND", 3)) {
        if (n < 10 || s->kiwi_stage < 2)
            return 0;
        int flags = p[3];
        pthread_mutex_lock(&s->mu);
        s->st.smeter_db = (float)((p[8] << 8) | p[9]) / 10.0f - 127.0f;
        pthread_mutex_unlock(&s->mu);
        const uint8_t *a = p + 10;
        size_t len = n - 10;
        int16_t pcm[1024];
        int m = 0;
        if (flags & 0x08)
            return 0;                               /* IQ: not what was asked for */
        if (flags & 0x10) {                         /* ADPCM after all */
            for (size_t i = 0; i < len; i++) {
                pcm[m++] = adpcm_nibble(&s->adpcm, a[i] & 15);
                pcm[m++] = adpcm_nibble(&s->adpcm, a[i] >> 4);
                if (m == 1024) { emit(s, pcm, m, s->kiwi_rate); m = 0; }
            }
        } else {
            bool le = flags & 0x80;
            for (size_t i = 0; i + 1 < len; i += 2) {
                pcm[m++] = (int16_t)(le ? a[i] | (a[i + 1] << 8) : (a[i] << 8) | a[i + 1]);
                if (m == 1024) { emit(s, pcm, m, s->kiwi_rate); m = 0; }
            }
        }
        emit(s, pcm, m, s->kiwi_rate);
        return 0;
    }
    if (memcmp(p, "MSG", 3) != 0)
        return 0;
    const char *t = (const char *)p + 3;
    char v[160];
    if (kiwi_kv(t, "too_busy", v, sizeof v)) {
        say(s, false, false, "%s is full (%s listeners)", s->host, v);
        s->retry_s = 120;
        return -1;
    }
    if (kiwi_kv(t, "badp", v, sizeof v) && atoi(v) != 0) {
        say(s, false, false, "%s refuses (badp=%s)%s", s->host, v,
            atoi(v) == 5 ? ": another connection from this address is there already"
                         : ": a password is needed ([sdr] password)");
        s->retry_s = 300;
        return -1;
    }
    if (kiwi_kv(t, "down", v, sizeof v) || strstr(t, " down")) {
        say(s, false, false, "%s is down for its owner", s->host);
        s->retry_s = 300;
        return -1;
    }
    if (kiwi_kv(t, "redirect", v, sizeof v)) {
        say(s, false, false, "%s sends its listeners elsewhere: %s — put that in [sdr] url", s->host, v);
        s->retry_s = 600;
        return -1;
    }
    if (kiwi_kv(t, "inactivity_timeout", v, sizeof v) && atoi(v) > 0)
        log_warn("sdr: %s disconnects listeners after %s minutes — its owner can give a password that lifts "
                 "this ([sdr] password); until then this program connects again each time", s->host, v);
    if (kiwi_kv(t, "ip_limit", v, sizeof v) && atoi(v) > 0)
        log_warn("sdr: %s allows %s minutes a day per address — ask its owner for a password", s->host, v);
    if (kiwi_kv(t, "kiwi_kick", v, sizeof v) || kiwi_kv(t, "password_timeout", v, sizeof v)) {
        say(s, false, false, "%s ended the session", s->host);
        s->retry_s = 30;
        return -1;
    }
    if (kiwi_kv(t, "freq_offset", v, sizeof v))
        s->kiwi_offset_khz = atof(v);
    if (kiwi_kv(t, "audio_rate", v, sizeof v) && atoi(v) > 0) {
        s->kiwi_rate = atoi(v);
        if (ws_text(s, "SET AR OK in=%d out=44100", s->kiwi_rate) != 0)
            return -1;
        if (s->kiwi_stage < 1) s->kiwi_stage = 1;
    }
    if (kiwi_kv(t, "sample_rate", v, sizeof v) && atof(v) > 0) {
        s->kiwi_rate = (int)lrint(atof(v));
        if (s->kiwi_stage < 2)
            return kiwi_tune(s);
    }
    return 0;
}

static int kiwi_tick(sdr_t *s, uint64_t now)
{
    if (now >= s->next_ka) {
        s->next_ka = now + 5000;
        return ws_text(s, "SET keepalive");
    }
    return 0;
}

static const sdr_backend_t BE_KIWI = { "kiwisdr", kiwi_open, kiwi_message, kiwi_tick };
