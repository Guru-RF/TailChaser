/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "role.h"

#include <stddef.h>

static const role_t ROLES[] = {
    [ROLE_SIMPLEX] = {
        ROLE_SIMPLEX, "simplex", "tailchaser-simplex",
        "simplex FM node on a reflector talkgroup", "simplex node",
        .rx = true, .tx = true, .reflector = true,
    },
    [ROLE_DUPLEX] = {
        ROLE_DUPLEX, "duplex", "tailchaser-duplex",
        "FM repeater (receiver and transmitter in one box) on a reflector talkgroup", "repeater",
        .rx = true, .tx = true, .reflector = true, .repeater = true,
    },
    [ROLE_RECEIVER] = {
        ROLE_RECEIVER, "receiver", "tailchaser-receiver",
        "receive site of a split repeater", "repeater receiver",
        .rx = true, .uplink = true,
    },
    [ROLE_TRANSMITTER] = {
        ROLE_TRANSMITTER, "transmitter", "tailchaser-transmitter",
        "transmit site of a split repeater: repeater logic, transmitter, talkgroup link",
        "repeater transmitter",
        .tx = true, .reflector = true, .remote_rx = true, .repeater = true,
    },
    [ROLE_SDRRECEIVER] = {
        ROLE_SDRRECEIVER, "sdrreceiver", "tailchaser-sdrreceiver",
        "a receiver on a web SDR (OpenWebRX, KiwiSDR, Web-888, UberSDR), for a repeater or node to vote with",
        "receiver on a web SDR",
        .rx = true, .uplink = true, .sdr = true,
    },
};

const role_t *role_get(role_id_t id)
{
    if ((size_t)id >= sizeof ROLES / sizeof ROLES[0])
        return NULL;
    return &ROLES[id];
}
