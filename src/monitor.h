/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — listening in, from the status page.
 *
 *   ws://<node>:8080/monitor/rx    what the receiver gives
 *   ws://<node>:8080/monitor/tx    what goes to the transmitter
 *
 * Binary websocket messages of 16-bit little-endian mono PCM at the node's
 * sample rate (16 kHz). Off unless [status] monitor = true: whoever can open
 * the status page can then listen. For setting a node up and finding out why
 * something sounds the way it does.
 *
 * The engine hands over the frames (main thread); every listener has a
 * thread and a second or two of store of its own, so a slow browser never
 * holds the node up.
 */
#ifndef TAILCHASER_MONITOR_H
#define TAILCHASER_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct monitor monitor_t;
enum { MONITOR_RX = 0, MONITOR_TX = 1 };

monitor_t *monitor_new(void);
void monitor_free(monitor_t *m);

/* The engine: a frame of one of the two. Costs nothing while nobody listens. */
void monitor_feed(monitor_t *m, int which, const int16_t *pcm, int n);

/* The status server: a GET with a websocket key for /monitor/<which>. Answers
 * the handshake and keeps the socket (true), or leaves it to the caller. */
bool monitor_accept(monitor_t *m, int fd, int which, const char *ws_key);

#endif /* TAILCHASER_MONITOR_H */
