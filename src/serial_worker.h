/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — serial worker thread.
 *
 * Owns the SA818 device handles after init and executes blocking AT commands
 * off the hot path (the audio thread's 20 ms deadline and the main poll loop).
 * The main thread submits commands; results come back through a queue that is
 * signalled via an eventfd the main loop can poll().
 *
 * Forked verbatim from AetherLink270-Base.
 */
#ifndef SMARTSPOT_SERIAL_WORKER_H
#define SMARTSPOT_SERIAL_WORKER_H

#include <stdbool.h>
#include <stdint.h>

#include "sa818.h"

typedef enum {
    SW_SET_CHANNEL = 1,
    SW_SET_VOLUME,
    SW_SET_FILTER,
    SW_SET_TAIL,
    SW_RSSI,
} sw_cmd_type_t;

typedef struct {
    sw_cmd_type_t type;
    uint8_t       radio_idx;
    /* SET_CHANNEL */
    int      bw;
    uint32_t tx_hz, rx_hz;
    int      ctcss_tx, ctcss_rx, squelch;
    /* SET_VOLUME */
    int      level;
    /* SET_FILTER */
    int      emphasis, highpass, lowpass;
    /* SET_TAIL */
    int      tail_open;
} sw_cmd_t;

typedef struct {
    sw_cmd_type_t type;
    uint8_t       radio_idx;
    int           status;     /* 0 = ok, -1 = error */
    int           rssi;       /* valid for SW_RSSI  */
} sw_result_t;

typedef struct serial_worker serial_worker_t;

serial_worker_t *serial_worker_new(void);
void             serial_worker_free(serial_worker_t *sw);

/* Hand an opened SA818 (owned by the worker afterwards) to slot radio_idx. */
int  serial_worker_attach(serial_worker_t *sw, uint8_t radio_idx, sa818_t *dev);

/* Spawn the worker thread (after attaching all devices). */
int  serial_worker_run(serial_worker_t *sw);
void serial_worker_stop(serial_worker_t *sw);   /* joins the thread */

/* Submit a command (non-blocking; returns -1 if the queue is full). */
int  serial_worker_submit(serial_worker_t *sw, const sw_cmd_t *cmd);

/* The eventfd to add to the main poll set (POLLIN when results are pending). */
int  serial_worker_result_fd(const serial_worker_t *sw);

/* Drain one result. Returns 0 if one was returned, -1 if none pending. */
int  serial_worker_get_result(serial_worker_t *sw, sw_result_t *out);

#endif /* SMARTSPOT_SERIAL_WORKER_H */
