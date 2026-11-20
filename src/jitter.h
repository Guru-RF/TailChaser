/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — RX UDP sequencing / replay filter.
 *
 * SvxReflector UDP packets carry a per-direction monotonic counter (the first
 * 4 bytes of the datagram, also the GCM AAD / part of the IV). This module
 * tracks the highest counter we have ACCEPTED (after a successful decrypt) and
 * rejects any packet at or below it — dropping replays, duplicates and
 * out-of-order-late frames before they reach the decoder.
 *
 * Two-step use so a forged high counter can't poison the sequence:
 *   1. jitter_is_replay(counter)  — cheap pre-decrypt reject (does NOT advance)
 *   2. jitter_commit(counter)     — call only AFTER the GCM tag verifies
 *
 * Deeper reorder buffering (holding N frames to re-sequence genuine network
 * reordering) is intentionally deferred until the live server's counter
 * cadence is observed — SVXConnect plays frames in arrival order, so a strict
 * reorder window risks adding latency or dropping valid audio. See
 * ARCHITECTURE.md §7. The 250 ms post-TG-switch gate lives in reflector.c.
 */
#ifndef SMARTSPOT_JITTER_H
#define SMARTSPOT_JITTER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     have_hi;
    uint32_t hi;        /* highest accepted (authenticated) counter */
} jitter_t;

void jitter_init(jitter_t *j);
void jitter_reset(jitter_t *j);   /* forget sequencing (new connection) */

/* True if `counter` is a replay / too-old frame that should be dropped
 * WITHOUT decrypting. Never advances state. */
bool jitter_is_replay(const jitter_t *j, uint32_t counter);

/* Record an authenticated frame's counter as accepted (advances the high
 * water mark). Call only after the GCM tag has verified. */
void jitter_commit(jitter_t *j, uint32_t counter);

#endif /* SMARTSPOT_JITTER_H */
