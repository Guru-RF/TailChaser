/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — sound card, both directions.
 *
 * One real-time thread. Every 20 ms it reads a period from the sound card (the
 * SA818 AF output), queues it as a mono frame for the main loop, and writes a
 * period to the sound card (the SA818 microphone input): the next frame of the
 * transmit queue while transmit audio is enabled, silence otherwise. Capture
 * and playback are the same codec, so one clock drives both.
 *
 * The main loop owns the transmitter (engine.c): it keys PTT and, once the
 * carrier has been up for the TX delay, keeps a few frames queued here. It is
 * clocked by the captured frames, so it fills the queue exactly as fast as
 * this thread empties it.
 *
 * For bench testing without a radio, alsa_device = "file:a.wav,pause:2000,b.wav"
 * plays 16 kHz WAV files in real time instead: each WAV is one squelch opening
 * (3 s of closed squelch after it), "pause:<ms>" is closed squelch. What would
 * have gone to the transmitter is written to [audio] tx_record as a stereo WAV:
 * left = the audio, right = full scale while PTT is keyed.
 */
#ifndef SVXSR_AUDIO_H
#define SVXSR_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

#define AUDIO_MAX_FRAME 960

typedef struct audio audio_t;

typedef struct {
    int16_t pcm[AUDIO_MAX_FRAME];
    int     n;
    int     sim_cos;        /* file input: 1 open, 0 closed; -1 = real hardware     */
    bool    tx;             /* PTT was keyed: the receiver heard nothing useful     */
} audio_frame_t;

audio_t *audio_new(const audio_config_t *cfg);
int      audio_start(audio_t *a);
void     audio_stop(audio_t *a);
void     audio_free(audio_t *a);

bool audio_is_file(const audio_t *a);

/* Instead of a sound card: what a web SDR hears (before audio_start). */
struct sdr;
void audio_use_sdr(audio_t *a, struct sdr *sdr);

/* ---- receive: thread -> main ---- */
int  audio_fd(const audio_t *a);                    /* POLLIN when frames wait   */
int  audio_pop(audio_t *a, audio_frame_t *out);     /* 0 = got one, -1 = empty   */
bool audio_failed(const audio_t *a);                /* the thread gave up        */
unsigned long audio_overruns(const audio_t *a);     /* capture periods lost      */
unsigned long audio_underruns(const audio_t *a);    /* playback xruns            */

/* ---- transmit: main -> thread ---- */
/* Tell the thread PTT is keyed (frames are marked, the bench recording shows it). */
void audio_set_ptt(audio_t *a, bool keyed);
/* Queue one frame (exactly one period of samples). 0, or -1 if the queue is full. */
int  audio_tx_push(audio_t *a, const int16_t *pcm, int n);
int  audio_tx_queued(const audio_t *a);
/* Throw away what is queued (a transmission that is being abandoned). */
void audio_tx_flush(audio_t *a);
/* How long the last frame takes from the queue to the transmitter at most:
 * the time to wait after the queue ran empty before the carrier may drop. */
int  audio_tx_latency_ms(const audio_t *a);

#endif /* SVXSR_AUDIO_H */
