/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* A receiver on the USB port: an RTL-SDR or a HydraSDR RFOne.
 *
 * No library of either is linked: the radio's own command line tool (rtl_sdr,
 * hydrasdr_rx) is started and gives the samples on a pipe. What comes is a
 * piece of spectrum, not audio, so the receiver is made here:
 *
 *   samples ─► mix the channel to 0 Hz ─► down to about 48 kHz (CIC)
 *           ─► channel filter ─► FM discriminator ─► de-emphasis ─► audio
 *
 * The radio is tuned next to the channel, not on it ([sdr] offset): at its
 * centre a cheap receiver has a spur of its own. From there on it is the
 * same as a web SDR: the squelch and the signal strength are made from the
 * hiss, in the engine — but nothing is late.
 */
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>

#define IQ_TAPS  65
#define IQ_SIN   4096

typedef struct {
    int      fmt, bytes;            /* 0 = u8, 1 = s16, 2 = f32; bytes per I/Q pair */
    int      rate, D, ch_rate;
    uint32_t ph, step;
    float    sin_tab[IQ_SIN];
    int64_t  ii[4], iq[4], ci[4], cq[4];
    int      cnt;
    float    norm;
    float    fir[IQ_TAPS], hi[IQ_TAPS], hq[IQ_TAPS];
    int      hpos;
    float    pi, pq, k, de, de_a, dc;
    double   pw;
    int      pwn;
    float    level_db;
    int16_t  out[4096];
    int      nout;
} iq_t;

static bool sdr_is_local(const char *type) { return !strcasecmp(type, "rtlsdr") || !strcasecmp(type, "hydrasdr"); }

static void iq_setup(iq_t *q, int fmt, int rate, double offset_hz, int half_hz)
{
    memset(q, 0, sizeof *q);
    q->fmt = fmt;
    q->bytes = fmt == 0 ? 2 : fmt == 1 ? 4 : 8;
    q->rate = rate;
    q->D = (rate + 24000) / 48000;
    if (q->D < 1) q->D = 1;
    q->ch_rate = (rate + q->D / 2) / q->D;
    q->step = (uint32_t)(int64_t)llround(-offset_hz / rate * 4294967296.0);
    for (int i = 0; i < IQ_SIN; i++)
        q->sin_tab[i] = (float)sin(2.0 * M_PI * i / IQ_SIN);
    q->norm = 1.0f / (float)pow(q->D, 4);
    double fc = (double)half_hz / q->ch_rate, sum = 0.0, w[IQ_TAPS];
    for (int i = 0; i < IQ_TAPS; i++) {
        double x = i - (IQ_TAPS - 1) / 2.0;
        double sinc = fabs(x) < 1e-9 ? 2.0 * fc : sin(2.0 * M_PI * fc * x) / (M_PI * x);
        w[i] = sinc * (0.54 - 0.46 * cos(2.0 * M_PI * i / (IQ_TAPS - 1)));
        sum += w[i];
    }
    for (int i = 0; i < IQ_TAPS; i++)
        q->fir[i] = (float)(w[i] / sum);
    /* one scale for narrow and wide: the hiss of an empty channel then has
     * one level, whatever the deviation, and that is what the squelch uses */
    q->k = (float)(q->ch_rate / (2.0 * M_PI * 2500.0) * 0.63 * 32767.0);
    q->de_a = (float)(1.0 - exp(-2.0 * M_PI * 300.0 / q->ch_rate));
    q->level_db = -120.0f;
}

/* One sample of the channel, at ch_rate. */
static void iq_channel(sdr_t *s, iq_t *q, float i, float qq)
{
    q->hi[q->hpos] = i;
    q->hq[q->hpos] = qq;
    if (++q->hpos == IQ_TAPS) q->hpos = 0;
    float ai = 0.0f, aq = 0.0f;
    int p = q->hpos;
    for (int k = 0; k < IQ_TAPS; k++) {
        ai += q->fir[k] * q->hi[p];
        aq += q->fir[k] * q->hq[p];
        if (++p == IQ_TAPS) p = 0;
    }
    q->pw += (double)ai * ai + (double)aq * aq;
    if (++q->pwn >= q->ch_rate / 5) {               /* the carrier, in dB below full scale */
        q->level_db = (float)(10.0 * log10(q->pw / q->pwn / (32768.0 * 32768.0) + 1e-14));
        q->pw = 0.0;
        q->pwn = 0;
        pthread_mutex_lock(&s->mu);
        s->st.smeter_db = q->level_db;
        pthread_mutex_unlock(&s->mu);
    }
    /* the discriminator: how far the phase went since the last sample */
    float d = atan2f(aq * q->pi - ai * q->pq, ai * q->pi + aq * q->pq);
    q->pi = ai;
    q->pq = aq;
    q->de += q->de_a * (d * q->k - q->de);          /* de-emphasis, 6 dB/octave from 300 Hz */
    q->dc += 0.0005f * (q->de - q->dc);             /* off frequency = a DC offset: out */
    float v = q->de - q->dc;
    q->out[q->nout++] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : lrintf(v));
    if (q->nout == (int)(sizeof q->out / sizeof q->out[0])) {
        emit(s, q->out, q->nout, q->ch_rate);
        q->nout = 0;
    }
}

static void iq_run(sdr_t *s, iq_t *q, const uint8_t *p, size_t pairs)
{
    for (size_t n = 0; n < pairs; n++, p += q->bytes) {
        float i, qq;
        if (q->fmt == 0) {
            i = ((float)p[0] - 127.5f) * 256.0f;
            qq = ((float)p[1] - 127.5f) * 256.0f;
        } else if (q->fmt == 1) {
            int16_t v[2];
            memcpy(v, p, 4);
            i = v[0];
            qq = v[1];
        } else {
            float v[2];
            memcpy(v, p, 8);
            i = v[0] * 32768.0f;
            qq = v[1] * 32768.0f;
        }
        float sn = q->sin_tab[q->ph >> 20], cs = q->sin_tab[((q->ph >> 20) + IQ_SIN / 4) & (IQ_SIN - 1)];
        q->ph += q->step;
        int64_t mi = lrintf(i * cs - qq * sn), mq = lrintf(i * sn + qq * cs);
        q->ii[0] += mi;       q->iq[0] += mq;
        q->ii[1] += q->ii[0]; q->iq[1] += q->iq[0];
        q->ii[2] += q->ii[1]; q->iq[2] += q->iq[1];
        q->ii[3] += q->ii[2]; q->iq[3] += q->iq[2];
        if (++q->cnt < q->D)
            continue;
        q->cnt = 0;
        int64_t vi = q->ii[3], vq = q->iq[3];
        for (int k = 0; k < 4; k++) {
            int64_t ti = vi - q->ci[k], tq = vq - q->cq[k];
            q->ci[k] = vi;
            q->cq[k] = vq;
            vi = ti;
            vq = tq;
        }
        iq_channel(s, q, (float)vi * q->norm, (float)vq * q->norm);
    }
    if (q->nout > 0) {
        emit(s, q->out, q->nout, q->ch_rate);
        q->nout = 0;
    }
}

static const char *local_name(const sdr_t *s) { return !strcasecmp(s->cfg.type, "rtlsdr") ? "RTL-SDR" : "HydraSDR RFOne"; }

static int local_rate(const sdr_config_t *c)
{
    return c->sample_rate > 0 ? c->sample_rate : !strcasecmp(c->type, "rtlsdr") ? 240000 : 2500000;
}

/* The command that gives the samples, and what they look like. */
static int local_command(const sdr_t *s, char *cmd, size_t cap, double lo_hz, int rate)
{
    const sdr_config_t *c = &s->cfg;
    int fmt = !strcasecmp(c->type, "rtlsdr") ? 0 : 1, n = 0;
    if (c->command[0]) {
        snprintf(cmd, cap, "%s", c->command);
    } else if (fmt == 0) {
        n = snprintf(cmd, cap, "exec rtl_sdr -f %.0f -s %d -g %.1f -p %d", lo_hz, rate, c->gain < 0 ? 0.0 : (double)c->gain, c->ppm);
        if (c->device[0] && (size_t)n < cap) n += snprintf(cmd + n, cap - (size_t)n, " -d '%s'", c->device);
        if (c->bias_tee && (size_t)n < cap) n += snprintf(cmd + n, cap - (size_t)n, " -T");
        if ((size_t)n < cap) snprintf(cmd + n, cap - (size_t)n, " -");
    } else {
        /* no correction of its own in the tool: a half ppm receiver hardly needs one */
        n = snprintf(cmd, cap, "exec hydrasdr_rx -r - -t 2 -f %.6f -a %d -g %d",
                     lo_hz * (1.0 + c->ppm * 1e-6) / 1e6, rate, c->gain < 0 ? 12 : (int)lrintf(c->gain));
        if (c->device[0] && (size_t)n < cap) n += snprintf(cmd + n, cap - (size_t)n, " -s '%s'", c->device);
        if (c->bias_tee && (size_t)n < cap) snprintf(cmd + n, cap - (size_t)n, " -b 1");
    }
    if (!strcasecmp(c->iq_format, "u8")) fmt = 0;
    else if (!strcasecmp(c->iq_format, "s16")) fmt = 1;
    else if (!strcasecmp(c->iq_format, "f32")) fmt = 2;
    return fmt;
}

static void *local_main(void *arg)
{
    sdr_t *s = arg;
    int rate = local_rate(&s->cfg), fails = 0;
    double offset = s->cfg.offset_hz ? s->cfg.offset_hz : rate / 4.0;
    char cmd[512];
    int fmt = local_command(s, cmd, sizeof cmd, s->cfg.freq_hz - offset, rate);
    const char *tool = s->cfg.command[0] ? "the command" : fmt == 0 ? "rtl_sdr" : "hydrasdr_rx";
    iq_t *q = malloc(sizeof *q);
    uint8_t *buf = malloc(1 << 16);
    pthread_mutex_lock(&s->mu);
    snprintf(s->st.server, sizeof s->st.server, "%s", local_name(s));
    pthread_mutex_unlock(&s->mu);
    log_debug("sdr: %s", cmd);
    while (q && buf && !atomic_load(&s->stop)) {
        int po[2], pe[2];
        if (pipe(po) != 0)
            break;
        if (pipe(pe) != 0) {
            close(po[0]); close(po[1]);
            break;
        }
        say(s, false, false, "starting the %s (%s)", local_name(s), tool);
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            dup2(po[1], 1);
            dup2(pe[1], 2);
            for (int fd = 3; fd < 256; fd++)
                close(fd);
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        close(po[1]);
        close(pe[1]);
        char last[120] = "";
        uint64_t began = mono_ms(), heard = began;
        size_t have = 0;
        bool up = false;
        const char *why = NULL;
        if (pid > 0) {
            iq_setup(q, fmt, rate, offset, (int)(s->cfg.bandwidth_hz * 0.7));
            while (!atomic_load(&s->stop)) {
                struct pollfd pf[2] = { { po[0], POLLIN, 0 }, { pe[0], POLLIN, 0 } };
                poll(pf, 2, 250);
                if (pf[1].revents & POLLIN) {       /* what the tool says: kept for when it stops */
                    char e[256];
                    ssize_t r = read(pe[0], e, sizeof e - 1);
                    if (r > 0) {
                        e[r] = '\0';
                        for (char *ln = strtok(e, "\r\n"); ln; ln = strtok(NULL, "\r\n"))
                            if (ln[0]) {
                                snprintf(last, sizeof last, "%s", ln);
                                log_debug("sdr: %s: %s", tool, ln);
                            }
                    }
                }
                if (pf[0].revents & (POLLIN | POLLHUP)) {
                    ssize_t r = read(po[0], buf + have, (1 << 16) - have);
                    if (r <= 0) {
                        why = "stopped";
                        break;
                    }
                    have += (size_t)r;
                    size_t pairs = have / (size_t)q->bytes;
                    iq_run(s, q, buf, pairs);
                    have -= pairs * (size_t)q->bytes;
                    memmove(buf, buf + pairs * (size_t)q->bytes, have);
                    heard = mono_ms();
                    if (!up) {
                        up = true;
                        pthread_mutex_lock(&s->mu);
                        s->st.connects++;
                        pthread_mutex_unlock(&s->mu);
                        say(s, true, true, "listening on %.4f MHz — %s here, %d kS/s", s->cfg.freq_hz / 1e6,
                            local_name(s), rate / 1000);
                    }
                } else if (mono_ms() - heard > SILENT_MS) {
                    why = "gives no samples";
                    break;
                }
            }
            kill(-pid, SIGTERM);
            for (int i = 0; i < 20 && waitpid(pid, NULL, WNOHANG) == 0; i++)
                usleep(50000);
            kill(-pid, SIGKILL);
            waitpid(pid, NULL, 0);
        }
        close(po[0]);
        close(pe[0]);
        pthread_mutex_lock(&s->mu);
        s->st.connected = s->st.listening = false;
        pthread_mutex_unlock(&s->mu);
        if (atomic_load(&s->stop))
            break;
        say(s, false, false, "%s %s%s%s — trying again", tool, why ? why : "could not be started",
            last[0] ? ": " : "", last);
        if (mono_ms() - began > 60000)
            fails = 0;
        if (++fails > 6) fails = 6;
        pause_ms(s, 2000 * fails);
    }
    free(q);
    free(buf);
    return NULL;
}
