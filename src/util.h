/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — small shared utilities.
 *
 *  - frame_ring: a lock-free single-producer/single-consumer ring of
 *    variable-length frames. Used to pass Opus packets / PCM blocks between
 *    the audio thread and the control thread without locks. One writer thread
 *    and one reader thread only.
 *  - monotonic time helpers.
 *  - constant-time byte compare for auth tokens / GCM tags.
 *
 * Forked verbatim from AetherLink270-Base.
 */
#ifndef SMARTSPOT_UTIL_H
#define SMARTSPOT_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ---- monotonic clock ---- */
uint64_t mono_ns(void);
uint64_t mono_ms(void);

/* ---- wall clock (what the hourly identification runs on) ---- */
#include <time.h>
time_t util_wall_time(void);

/* ---- constant-time compare: returns 0 iff the n bytes are equal ---- */
int ct_memcmp(const void *a, const void *b, size_t n);

/* ---- local network ---- */
/* Best-effort: write the primary non-loopback, non-link-local IPv4 address
 * (dotted quad) into out (size cap). Returns 0 on success, -1 if none. */
int util_primary_ipv4(char *out, size_t cap);

/* ---- daemon state files ---- */
/* Build a state-file path in the PARENT of the PKI state_dir, i.e.
 * state_dir="/var/lib/tailchaser/pki" + name="x" ->
 * "/var/lib/tailchaser/x" (state, not PKI material). */
void util_state_path(const char *state_dir, const char *name, char *out, size_t cap);

/* ---- big-endian (network byte order) field helpers ---- */
/* SvxReflector frames every integer big-endian; these read/write into a
 * byte buffer at a fixed offset (caller bounds-checks). */
void     wr_be16(uint8_t *p, uint16_t v);
void     wr_be32(uint8_t *p, uint32_t v);
uint16_t rd_be16(const uint8_t *p);
uint32_t rd_be32(const uint8_t *p);

/* ---- SPSC variable-length frame ring ---- */
typedef struct frame_ring frame_ring_t;

/* Capacity = number of frame slots (rounded up to a power of two).
 * max_payload = largest single frame in bytes. Returns NULL on OOM. */
frame_ring_t *frame_ring_new(size_t slots, size_t max_payload);
void          frame_ring_free(frame_ring_t *r);

/* Producer side. Returns 0 on success, -1 if the ring is full,
 * -2 if len > max_payload. */
int frame_ring_push(frame_ring_t *r, const void *data, size_t len);

/* Consumer side. Copies the next frame into out (capacity out_cap).
 * Returns the frame length on success, -1 if empty, -2 if out_cap too small
 * (frame left in place). */
ssize_t frame_ring_pop(frame_ring_t *r, void *out, size_t out_cap);

/* Approximate number of frames currently queued (consumer-safe estimate). */
size_t frame_ring_count(const frame_ring_t *r);

#endif /* SMARTSPOT_UTIL_H */
