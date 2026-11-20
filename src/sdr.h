/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — a receiver somewhere on the internet: a web SDR.
 *
 * tailchaser-sdrreceiver has no radio of its own. It listens to one FM
 * channel on a web SDR — OpenWebRX, KiwiSDR, Web-888 or UberSDR — as one of
 * its listeners, and what it hears takes the place of the sound card: the
 * squelch, the gate and the uplink to the repeater are the same as in a
 * tailchaser-receiver.
 *
 * A thread of its own keeps the connection: it connects, tunes, decodes,
 * brings the audio to the program's sample rate and keeps a little of it in
 * store against the jitter of the internet. It reconnects by itself. It never
 * changes anything other listeners share (an OpenWebRX profile): when no
 * device of the receiver is on the band, it waits until one is.
 */
#ifndef TAILCHASER_SDR_H
#define TAILCHASER_SDR_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

typedef struct sdr sdr_t;

typedef struct {
    bool  connected;        /* the websocket is up                           */
    bool  listening;        /* tuned, audio is coming                        */
    char  state[160];       /* in words, for the log and the status page     */
    char  server[96];       /* what the receiver says it is                  */
    int   in_rate;          /* the receiver's audio rate                     */
    float smeter_db;        /* the receiver's own S-meter, as it gives it    */
    int   buffered_ms;
    unsigned long connects, dropped_ms, underruns;
} sdr_status_t;

sdr_t *sdr_start(const sdr_config_t *cfg, int out_rate);
void   sdr_stop(sdr_t *s);

/* Exactly n samples, or nothing (0): called once per period by the audio
 * thread. Nothing = not connected, not tuned, or the store ran empty. */
int    sdr_take(sdr_t *s, int16_t *out, int n);

void   sdr_get_status(sdr_t *s, sdr_status_t *out);

#endif /* TAILCHASER_SDR_H */
