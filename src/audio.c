/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "audio.h"
#include "tag.h"
#include "log.h"
#include "sdr.h"
#include "util.h"
#include "wav.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

#define QUEUE_FRAMES 64         /* 1.28 s of received audio before frames are dropped */
#define TX_QUEUE     16         /* the engine keeps about three frames in it          */
#define FILE_GAP_MS  3000       /* closed squelch after each test file                */

struct audio {
    audio_config_t cfg;
    bool        is_file;
    int         period;
    snd_pcm_t  *cap, *play;
    pthread_t   thread;
    bool        running;
    atomic_bool stop, failed;
    atomic_ulong overruns, underruns;
    int         efd;

    /* received frames: thread -> main */
    pthread_mutex_t mu;
    audio_frame_t q[QUEUE_FRAMES];
    int         q_head, q_len;

    /* transmit frames: main -> thread */
    frame_ring_t *txq;
    atomic_bool ptt, tx_flush;
    struct tag *tag;            /* the callsign tag, added while keyed */

    struct sdr *sdr;            /* the receiver is a web SDR */
    FILE       *rec;            /* bench recording of the transmit side */
    long        rec_frames;
};

/* ---- received frames ---- */

static void enqueue(audio_t *a, const int16_t *pcm, int n, int sim_cos, bool tx)
{
    pthread_mutex_lock(&a->mu);
    if (a->q_len == QUEUE_FRAMES) {             /* main loop stalled: drop the oldest */
        a->q_head = (a->q_head + 1) % QUEUE_FRAMES;
        a->q_len--;
        atomic_fetch_add(&a->overruns, 1);
    }
    audio_frame_t *f = &a->q[(a->q_head + a->q_len) % QUEUE_FRAMES];
    memcpy(f->pcm, pcm, sizeof(int16_t) * (size_t)n);
    f->n = n;
    f->sim_cos = sim_cos;
    f->tx = tx;
    a->q_len++;
    pthread_mutex_unlock(&a->mu);
    uint64_t one = 1;
    if (write(a->efd, &one, sizeof one) < 0) { /* counter saturated: harmless */ }
}

int audio_pop(audio_t *a, audio_frame_t *out)
{
    int rc = -1;
    pthread_mutex_lock(&a->mu);
    if (a->q_len > 0) {
        *out = a->q[a->q_head];
        a->q_head = (a->q_head + 1) % QUEUE_FRAMES;
        a->q_len--;
        rc = 0;
    }
    pthread_mutex_unlock(&a->mu);
    return rc;
}

/* ---- transmit frames ---- */

void audio_set_ptt(audio_t *a, bool keyed)   { atomic_store(&a->ptt, keyed); }
void audio_tx_flush(audio_t *a)              { atomic_store(&a->tx_flush, true); }
void audio_set_tag(audio_t *a, struct tag *tag) { a->tag = tag; }
int  audio_tx_queued(const audio_t *a)       { return (int)frame_ring_count(a->txq); }

int audio_tx_push(audio_t *a, const int16_t *pcm, int n)
{
    if (n != a->period)
        return -1;
    return frame_ring_push(a->txq, pcm, sizeof(int16_t) * (size_t)n) == 0 ? 0 : -1;
}

int audio_tx_latency_ms(const audio_t *a)
{
    int period_ms = a->period * 1000 / a->cfg.sample_rate;
    /* the playback buffer, the period being written, the codec's own delay */
    return a->is_file ? 2 * period_ms : (a->cfg.buffer_periods + 2) * period_ms;
}

/* The next period for the transmitter: a queued frame, or silence. Runs on the
 * audio thread, which is the only reader of the queue. */
static void next_tx_frame(audio_t *a, int16_t *out)
{
    if (atomic_exchange(&a->tx_flush, false)) {
        int16_t scratch[AUDIO_MAX_FRAME];
        while (frame_ring_pop(a->txq, scratch, sizeof scratch) >= 0) { /* drop */ }
    }
    if (frame_ring_pop(a->txq, out, sizeof(int16_t) * (size_t)a->period) <= 0)
        memset(out, 0, sizeof(int16_t) * (size_t)a->period);
    if (a->tag)                         /* also in the tail, where there is nothing else */
        tag_mix(a->tag, out, a->period, atomic_load(&a->ptt));
}

/* ---- ALSA ---- */

static int configure_pcm(audio_t *a, snd_pcm_t *pcm, const char *tag)
{
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    int err;
    if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
        (err = snd_pcm_hw_params_set_channels(pcm, hw, 2)) < 0) {
        log_error("audio: %s hw params (access/format/channels): %s", tag, snd_strerror(err));
        return -1;
    }
    unsigned int rate = (unsigned int)a->cfg.sample_rate;
    if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0)) < 0) {
        log_error("audio: %s set_rate: %s", tag, snd_strerror(err));
        return -1;
    }
    if (rate != (unsigned int)a->cfg.sample_rate) {
        log_error("audio: the device gives %u Hz, not %d — use a plughw: device so ALSA "
                  "resamples", rate, a->cfg.sample_rate);
        return -1;
    }
    snd_pcm_uframes_t period = (snd_pcm_uframes_t)a->period;
    if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0)) < 0) {
        log_error("audio: %s set_period: %s", tag, snd_strerror(err));
        return -1;
    }
    snd_pcm_uframes_t buffer = period * (snd_pcm_uframes_t)a->cfg.buffer_periods;
    if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0 ||
        (err = snd_pcm_hw_params(pcm, hw)) < 0) {
        log_error("audio: %s hw_params apply: %s", tag, snd_strerror(err));
        return -1;
    }
    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_set_avail_min(pcm, sw, period);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, period);
    if ((err = snd_pcm_sw_params(pcm, sw)) < 0) {
        log_error("audio: %s sw_params: %s", tag, snd_strerror(err));
        return -1;
    }
    log_info("audio: %s %s opened (rate=%u period=%lu buffer=%lu)", tag, a->cfg.alsa_device,
             rate, (unsigned long)period, (unsigned long)buffer);
    return 0;
}

/* Fill the playback buffer with silence up to its working level, so the
 * periods written one by one after that never find it empty. */
static void prime_playback(audio_t *a, const int16_t *silence)
{
    for (int i = 0; i < a->cfg.buffer_periods - 1; i++)
        if (snd_pcm_writei(a->play, silence, (snd_pcm_uframes_t)a->period) < 0)
            break;
}

static void *alsa_main(void *arg)
{
    audio_t *a = arg;
    int period = a->period;
    int16_t *capbuf  = malloc(sizeof(int16_t) * (size_t)period * 2);
    int16_t *playbuf = calloc((size_t)period * 2, sizeof(int16_t));
    int16_t *silence = calloc((size_t)period * 2, sizeof(int16_t));
    int16_t mono[AUDIO_MAX_FRAME];
    if (!capbuf || !playbuf || !silence) { atomic_store(&a->failed, true); goto out; }
    int rx_ch = a->cfg.channel == AUDIO_CH_RIGHT ? 1 : 0;
    int tx_ch = a->cfg.tx_channel == AUDIO_CH_RIGHT ? 1 : 0;

    if (a->cap)
        snd_pcm_prepare(a->cap);
    if (a->play) {
        snd_pcm_prepare(a->play);
        prime_playback(a, silence);
    }
    if (a->cap)
        snd_pcm_start(a->cap);

    while (!atomic_load(&a->stop)) {
        /* ---- one period in ---- */
        int got = 0;
        if (!a->cap) {
            /* A transmitter has no receiver on its sound card: the period it
             * writes below is the clock, and the main loop gets a silent frame
             * for every one of them. */
            memset(capbuf, 0, sizeof(int16_t) * (size_t)period * 2);
            got = period;
        }
        while (got < period && !atomic_load(&a->stop)) {
            snd_pcm_sframes_t r = snd_pcm_readi(a->cap, capbuf + got * 2,
                                                (snd_pcm_uframes_t)(period - got));
            if (r == -EPIPE || r == -ESTRPIPE) {
                atomic_fetch_add(&a->overruns, 1);
                if (r == -ESTRPIPE)
                    while (snd_pcm_resume(a->cap) == -EAGAIN) usleep(10000);
                snd_pcm_prepare(a->cap);
                snd_pcm_start(a->cap);
                got = 0;
                continue;
            }
            if (r < 0) {
                log_error("audio: capture read: %s", snd_strerror((int)r));
                atomic_store(&a->failed, true);
                goto out;
            }
            got += (int)r;
        }
        if (got < period)
            break;
        for (int i = 0; i < period; i++)
            mono[i] = capbuf[i * 2 + rx_ch];
        enqueue(a, mono, period, -1, atomic_load(&a->ptt));

        /* ---- one period out ---- */
        if (!a->play)
            continue;                   /* a receiver: the capture is the clock */
        next_tx_frame(a, mono);
        for (int i = 0; i < period; i++) {
            playbuf[i * 2 + tx_ch]       = mono[i];
            playbuf[i * 2 + (1 - tx_ch)] = 0;
        }
        snd_pcm_sframes_t w = snd_pcm_writei(a->play, playbuf, (snd_pcm_uframes_t)period);
        if (w == -EPIPE || w == -ESTRPIPE) {
            atomic_fetch_add(&a->underruns, 1);
            if (w == -ESTRPIPE)
                while (snd_pcm_resume(a->play) == -EAGAIN) usleep(10000);
            snd_pcm_prepare(a->play);
            prime_playback(a, silence);
            w = snd_pcm_writei(a->play, playbuf, (snd_pcm_uframes_t)period);
        }
        if (w < 0) {
            log_error("audio: playback write: %s", snd_strerror((int)w));
            atomic_store(&a->failed, true);
            goto out;
        }
    }
out:
    if (a->cap)  snd_pcm_drop(a->cap);
    if (a->play) snd_pcm_drop(a->play);
    free(capbuf);
    free(playbuf);
    free(silence);
    if (atomic_load(&a->failed)) {
        uint64_t one = 1;                       /* wake the main loop so it notices */
        if (write(a->efd, &one, sizeof one) < 0) { }
    }
    return NULL;
}

/* ---- WAV file input + transmit recording (bench testing) ---- */

static void rec_header(FILE *f, int rate, long frames)
{
    unsigned data = (unsigned)frames * 4u, riff = 36u + data, r = (unsigned)rate, br = r * 4u;
    unsigned char h[44] = {
        'R','I','F','F', (unsigned char)riff, (unsigned char)(riff >> 8),
        (unsigned char)(riff >> 16), (unsigned char)(riff >> 24),
        'W','A','V','E','f','m','t',' ', 16,0,0,0, 1,0, 2,0,
        (unsigned char)r, (unsigned char)(r >> 8), (unsigned char)(r >> 16), (unsigned char)(r >> 24),
        (unsigned char)br, (unsigned char)(br >> 8), (unsigned char)(br >> 16), (unsigned char)(br >> 24),
        4,0, 16,0, 'd','a','t','a',
        (unsigned char)data, (unsigned char)(data >> 8),
        (unsigned char)(data >> 16), (unsigned char)(data >> 24),
    };
    fwrite(h, 1, 44, f);
}

static void sleep_until(struct timespec *t, long add_ns)
{
    t->tv_nsec += add_ns;
    while (t->tv_nsec >= 1000000000L) { t->tv_nsec -= 1000000000L; t->tv_sec++; }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, t, NULL);
}

/* One 20 ms tick of the simulated sound card: a received frame in, a
 * transmit frame out (into the recording). */
static void file_tick(audio_t *a, const int16_t *pcm, int cos, struct timespec *t, long period_ns)
{
    bool keyed = atomic_load(&a->ptt);
    enqueue(a, pcm, a->period, cos, keyed);

    int16_t tx[AUDIO_MAX_FRAME];
    next_tx_frame(a, tx);
    if (a->rec) {
        int16_t st[2 * AUDIO_MAX_FRAME];
        for (int i = 0; i < a->period; i++) {
            st[2 * i]     = tx[i];
            st[2 * i + 1] = keyed ? 32767 : 0;
        }
        fwrite(st, sizeof(int16_t) * 2, (size_t)a->period, a->rec);
        a->rec_frames += a->period;
        if (a->rec_frames % (25L * a->period) == 0)
            fflush(a->rec);
    }
    sleep_until(t, period_ns);
}

static void *file_main(void *arg)
{
    audio_t *a = arg;
    int period = a->period;
    long period_ns = (long)(1000000000LL * period / a->cfg.sample_rate);
    static const int16_t silence[AUDIO_MAX_FRAME];
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);

    char list[1024];
    snprintf(list, sizeof list, "%s", a->cfg.alsa_device + 5);
    for (int i = 0; i < 1000 / 20 && !atomic_load(&a->stop); i++)       /* 1 s lead-in */
        file_tick(a, silence, 0, &t, period_ns);

    char *save = NULL;
    for (char *item = strtok_r(list, ",", &save); item && !atomic_load(&a->stop);
         item = strtok_r(NULL, ",", &save)) {
        if (!strncmp(item, "pause:", 6)) {
            int ms = atoi(item + 6);
            for (int i = 0; i < ms / 20 && !atomic_load(&a->stop); i++)
                file_tick(a, silence, 0, &t, period_ns);
            continue;
        }
        int n = 0, rate = 0;
        int16_t *pcm = wav_load(item, &n, &rate);
        if (!pcm || rate != a->cfg.sample_rate) {
            log_error("audio: %s: need a %d Hz 16-bit WAV", item, a->cfg.sample_rate);
            free(pcm);
            continue;
        }
        log_info("audio: [file] squelch opens: %s (%.2f s)", item, n / (double)rate);
        for (int off = 0; off + period <= n && !atomic_load(&a->stop); off += period)
            file_tick(a, pcm + off, 1, &t, period_ns);
        free(pcm);
        log_info("audio: [file] squelch closes");
        for (int i = 0; i < FILE_GAP_MS / 20 && !atomic_load(&a->stop); i++)
            file_tick(a, silence, 0, &t, period_ns);
    }
    log_info("audio: [file] playlist finished — squelch stays closed");
    while (!atomic_load(&a->stop))
        file_tick(a, silence, 0, &t, period_ns);
    return NULL;
}

/* ---- a web SDR instead of a sound card ---- */

/* The clock is ours: a frame every 20 ms, of what the SDR sent — or, while
 * it sends nothing, a frame of silence marked "squelch closed". */
static void *sdr_feed_main(void *arg)
{
    audio_t *a = arg;
    long period_ns = (long)(1000000000LL * a->period / a->cfg.sample_rate);
    static const int16_t silence[AUDIO_MAX_FRAME];
    int16_t pcm[AUDIO_MAX_FRAME];
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    while (!atomic_load(&a->stop)) {
        if (sdr_take(a->sdr, pcm, a->period) == a->period)
            enqueue(a, pcm, a->period, -1, false);
        else
            enqueue(a, silence, a->period, 0, false);
        sleep_until(&t, period_ns);
    }
    return NULL;
}

void audio_use_sdr(audio_t *a, struct sdr *sdr) { a->sdr = sdr; }

/* ---- lifecycle ---- */

audio_t *audio_new(const audio_config_t *cfg)
{
    audio_t *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->cfg = *cfg;
    a->period = cfg->period_frames;
    a->is_file = !strncmp(cfg->alsa_device, "file:", 5);
    pthread_mutex_init(&a->mu, NULL);
    a->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    a->txq = frame_ring_new(TX_QUEUE, sizeof(int16_t) * (size_t)a->period);
    if (a->efd < 0 || !a->txq || a->period <= 0 || a->period > AUDIO_MAX_FRAME) {
        audio_free(a);
        return NULL;
    }
    return a;
}

int audio_start(audio_t *a)
{
    if (a->sdr) {
        if (pthread_create(&a->thread, NULL, sdr_feed_main, a) != 0)
            return -1;
        a->running = true;
        return 0;
    }
    if (a->is_file) {
        if (a->cfg.tx_record[0]) {
            a->rec = fopen(a->cfg.tx_record, "wb");
            if (!a->rec) {
                log_error("audio: cannot write %s: %s", a->cfg.tx_record, strerror(errno));
                return -1;
            }
            rec_header(a->rec, a->cfg.sample_rate, 0x1fffffffL);   /* fixed up on a clean stop */
        }
    } else {
        int err;
        if (!a->cfg.capture && !a->cfg.playback) {
            log_error("audio: neither a receiver nor a transmitter — nothing to open");
            return -1;
        }
        if (a->cfg.capture) {
            err = snd_pcm_open(&a->cap, a->cfg.alsa_device, SND_PCM_STREAM_CAPTURE, 0);
            if (err < 0) {
                log_error("audio: open capture %s: %s", a->cfg.alsa_device, snd_strerror(err));
                return -1;
            }
            if (configure_pcm(a, a->cap, "capture") != 0)
                return -1;
        }
        if (a->cfg.playback) {
            err = snd_pcm_open(&a->play, a->cfg.alsa_device, SND_PCM_STREAM_PLAYBACK, 0);
            if (err < 0) {
                log_error("audio: open playback %s: %s", a->cfg.alsa_device, snd_strerror(err));
                return -1;
            }
            if (configure_pcm(a, a->play, "playback") != 0)
                return -1;
        }
    }
    if (pthread_create(&a->thread, NULL, a->is_file ? file_main : alsa_main, a) != 0) {
        log_error("audio: pthread_create failed");
        return -1;
    }
    a->running = true;
    if (!a->is_file) {
        /* Best-effort real-time priority; ignored without CAP_SYS_NICE. */
        struct sched_param sp = { .sched_priority = 10 };
        if (pthread_setschedparam(a->thread, SCHED_FIFO, &sp) != 0)
            log_warn("audio: could not set SCHED_FIFO (need CAP_SYS_NICE/LimitRTPRIO)");
    }
    return 0;
}

void audio_stop(audio_t *a)
{
    if (!a || !a->running)
        return;
    atomic_store(&a->stop, true);
    pthread_join(a->thread, NULL);
    a->running = false;
}

void audio_free(audio_t *a)
{
    if (!a)
        return;
    audio_stop(a);
    if (a->cap)  snd_pcm_close(a->cap);
    if (a->play) snd_pcm_close(a->play);
    if (a->rec) {
        fseek(a->rec, 0, SEEK_SET);
        rec_header(a->rec, a->cfg.sample_rate, a->rec_frames);
        fclose(a->rec);
    }
    if (a->efd >= 0) close(a->efd);
    if (a->txq) frame_ring_free(a->txq);
    tag_free(a->tag);
    pthread_mutex_destroy(&a->mu);
    free(a);
}

bool audio_is_file(const audio_t *a) { return a->is_file; }
int  audio_fd(const audio_t *a) { return a->efd; }
bool audio_failed(const audio_t *a) { return atomic_load(&a->failed); }
unsigned long audio_overruns(const audio_t *a) { return atomic_load(&a->overruns); }
unsigned long audio_underruns(const audio_t *a) { return atomic_load(&a->underruns); }
