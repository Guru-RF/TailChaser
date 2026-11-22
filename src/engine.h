/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — the node logic, for all four roles.
 *
 *   receiving     the receiver feeds the squelch gate (gate.c); what the gate
 *                 lets through is an "over". It is levelled (audioproc.c) and
 *                 goes where the role says: to the talkgroup (Opus), to the
 *                 repeater this receiver serves (nettrx.c), to the transmitter.
 *   transmitting  what the talkgroup sends is decoded and queued; the
 *                 transmitter is keyed, the carrier is left alone for the TX
 *                 delay, then the queue is played.
 *
 * A simplex node has one radio: it either listens or transmits, and a station
 * on the frequency wins — the transmitter is never keyed over a voice that is
 * being received, and while it transmits the receiver is ignored.
 *
 * A repeater (duplex, or a transmitter with its remote receiver) listens and
 * transmits at once: what the receiver hears is transmitted again, the carrier
 * stays up for the hang time between overs, and a roger beep marks the end of
 * an over. A station on the input wins over the talkgroup.
 *
 * On top of that: the callsign in Morse on the clock, a transmit time-out,
 * talkgroup following (monitored talkgroups, DTMF selection, back to the home
 * talkgroup after a silence).
 *
 * Everything here runs on the main thread. Its clock is the sound card:
 * engine_on_frame() is called once per 20 ms of audio.
 */
#ifndef SVXSR_ENGINE_H
#define SVXSR_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "audio.h"
#include "config.h"
#include "gpio.h"
#include "netrx.h"
#include "nettrx.h"
#include "reflector.h"
#include "role.h"
#include "status.h"

typedef struct engine engine_t;

/* ref is NULL on a receiver, uplink NULL on everything else. gpio may be NULL
 * (bench test with file audio: PTT only shows in the recording). The engine
 * does not own any of the objects it is given. */
engine_t *engine_new(const config_t *cfg, const role_t *role, audio_t *audio, reflector_t *ref,
                     nettrx_t *uplink, gpio_t *gpio, status_t *status);
/* Drops the carrier if it is up. */
void engine_free(engine_t *e);

/* One captured frame. hw_cos = the squelch line as last read (ignored when
 * the frame carries a simulated squelch). */
void engine_on_frame(engine_t *e, const audio_frame_t *f, bool hw_cos);

/* One frame from the talkgroup: an Opus packet, or len 0 = the talker stopped. */
void engine_on_net_audio(engine_t *e, const uint8_t *opus, int len);

void engine_on_event(engine_t *e, const reflector_event_t *ev);

/* A transmitter: what its receiver reports over the link (netrx.c). */
void engine_remote_squelch(engine_t *e, int link, bool open, float siglev);
void engine_remote_audio(engine_t *e, int link, const int16_t *pcm, int n);
void engine_remote_dtmf(engine_t *e, int link, char digit, int duration_ms);
void engine_remote_siglev(engine_t *e, int link, float siglev);
/* ...and the link itself, for the status page. */
/* A receiver elsewhere; returns its number (the `link` of the calls above). */
struct monitor;
void engine_set_monitor(engine_t *e, struct monitor *m);     /* listening in */
int  engine_add_rxlink(engine_t *e, netrx_t *link, const char *name, float siglev_offset, int lag_ms);

/* A test transmission (tones), as soon as the transmitter is free. */
void engine_test_tx(engine_t *e);
/* From the admin portal or the API. A blocked receiver is a squelch that
 * never opens; a blocked transmitter is never keyed, whatever the logic
 * decides. */
void engine_set_block(engine_t *e, bool rx, bool tx);
/* Nothing on the air, nothing being received: the moment for a restart. */
bool engine_idle(const engine_t *e);
/* Play the next spoken announcement now (SIGUSR2): to hear what it sounds like. */
void engine_announce_now(engine_t *e);
/* Send what a listener elsewhere hears before this repeater comes in from the
 * talkgroup — "ORA" for ON0ORA, at net_call_wpm (SIGRTMIN+1): to hear it. */
void engine_call_test(engine_t *e);

/* Called every time the carrier has dropped. */
void engine_set_unkey_hook(engine_t *e, void (*fn)(void *), void *ctx);

/* Signal strength: the main loop asks the SA818 while this says so. */
bool engine_wants_rssi(const engine_t *e);
void engine_on_rssi(engine_t *e, int rssi);

bool engine_transmitting(const engine_t *e);

/* True once the PTT line refused an order. The caller should stop: the
 * service then releases the line from a fresh process (-u). */
bool engine_ptt_failed(const engine_t *e);

/* Copy the live state into the status page's snapshot. */
void engine_push_status(engine_t *e);

#endif /* SVXSR_ENGINE_H */
