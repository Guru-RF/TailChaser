/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — the five programs built from this one source tree.
 *
 *   tailchaser-simplex      one radio on one frequency, linked to a talkgroup
 *   tailchaser-duplex       a repeater: receiver and transmitter in one box
 *   tailchaser-receiver     the receive site of a split repeater
 *   tailchaser-transmitter  the transmit site of a split repeater: the repeater
 *                           logic, the transmitter and the talkgroup link; its
 *                           receiver is a tailchaser-receiver somewhere else
 *   tailchaser-sdrreceiver  a receiver without a radio: it listens on a web SDR
 *                           and serves what it hears like a tailchaser-receiver
 *
 * A role is a set of capabilities. Everything else — the configuration file,
 * the gate, the reflector client, the status page — is the same code, and asks
 * the role what it has.
 */
#ifndef TAILCHASER_ROLE_H
#define TAILCHASER_ROLE_H

#include <stdbool.h>

typedef enum { ROLE_SIMPLEX = 0, ROLE_DUPLEX, ROLE_RECEIVER, ROLE_TRANSMITTER, ROLE_SDRRECEIVER } role_id_t;

typedef struct {
    role_id_t   id;
    const char *name;       /* "simplex": config file, unit and log name          */
    const char *binary;     /* "tailchaser-simplex"                               */
    const char *summary;    /* one line for --help                                */
    const char *title;      /* on the status page: "simplex node"                 */
    bool rx;                /* a local receiver: sound card in, squelch, the gate */
    bool tx;                /* a local transmitter: PTT, sound card out           */
    bool reflector;         /* a talkgroup link                                   */
    bool uplink;            /* serves its receiver to a repeater (RemoteTrx)      */
    bool remote_rx;         /* its receiver is a tailchaser-receiver elsewhere    */
    bool repeater;          /* what is received is transmitted again              */
    bool sdr;               /* its receiver is a web SDR on the internet          */
} role_t;

const role_t *role_get(role_id_t id);

/* The program: each binary's main() is `return tailchaser_main(ROLE_x, ...)`. */
int tailchaser_main(role_id_t id, int argc, char **argv);

#endif /* TAILCHASER_ROLE_H */
