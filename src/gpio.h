/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — GPIO via libgpiod.
 *
 * THE libgpiod VERSION FIREWALL. gpio.c is the only file that includes
 * <gpiod.h>; it targets libgpiod v2 (current Raspberry Pi OS / Trixie). For an
 * older v1.6 system (Bookworm), only gpio.c needs porting — this interface
 * stays the same.
 *
 *  - PTT: one output line per radio (active-low supported, matches "!16").
 *  - COS: one input line per radio with both-edge events; an fd the main
 *    loop can poll(), plus a current-level read. The COS line is wired to the
 *    SA818 SQ pin (hardware squelch + CTCSS gate) — see ARCHITECTURE.md §5.
 *  - Detect pins: one-shot "is this line grounded?" reads for HAT/band autodetect.
 *
 * Forked verbatim from AetherLink270-Base.
 */
#ifndef SMARTSPOT_GPIO_H
#define SMARTSPOT_GPIO_H

#include <stdbool.h>
#include <stddef.h>

typedef struct gpio gpio_t;

gpio_t *gpio_open(const char *chip_path);
void    gpio_close(gpio_t *g);

/* Find the 40-pin header GPIO controller (by libgpiod chip label:
 * bcm2835 = Zero/3, bcm2711 = Pi 4, rp1/bcm2712 = Pi 5). Fills out with the
 * /dev/gpiochipN path. Returns 0 on success, -1 if none matched. */
int gpio_detect_header_chip(char *out, size_t cap);

/* PTT output for radio_idx. Returns 0 on success. */
int  gpio_ptt_init(gpio_t *g, int radio_idx, unsigned int line, bool active_low);
int  gpio_ptt_set(gpio_t *g, int radio_idx, bool transmit);

/* COS input + edge events for radio_idx. cos open == logical "active". */
int  gpio_cos_init(gpio_t *g, int radio_idx, unsigned int line, bool active_low);
int  gpio_cos_fd(gpio_t *g, int radio_idx);     /* pollable fd, -1 if none   */
bool gpio_cos_read(gpio_t *g, int radio_idx);   /* current level: true=open  */
/* Consume pending edge events; returns the current level (1=open,0=closed)
 * or -1 on error. Call after the COS fd becomes readable. */
int  gpio_cos_drain(gpio_t *g, int radio_idx);

/* One-shot: is the given line pulled to ground? (input + pull-up, value==0).
 * Returns 1 if grounded, 0 if not, -1 on error. */
int  gpio_read_grounded(gpio_t *g, unsigned int line);

#endif /* SMARTSPOT_GPIO_H */
