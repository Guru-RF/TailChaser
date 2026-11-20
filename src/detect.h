/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — which Raspberry Pi is this (for the log and the status page). */
#ifndef SVXSR_DETECT_H
#define SVXSR_DETECT_H

#include <stddef.h>

/* Raspberry Pi model from the device tree (e.g. "Raspberry Pi Zero 2 W").
 * Copies into buf (NUL-terminated) and returns buf; "unknown" if unavailable. */
const char *detect_pi_model(char *buf, size_t cap);

#endif /* SVXSR_DETECT_H */
