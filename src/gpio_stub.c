/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — GPIO stand-in for building on a machine without libgpiod
 * (make NOGPIO=1). Only useful with file audio input: there is no squelch line
 * to read, so every call reports "not available". */
#include "gpio.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>

struct gpio { int unused; };

gpio_t *gpio_open(const char *chip_path)
{
    (void)chip_path;
    log_error("gpio: this build has no GPIO support (built with NOGPIO=1)");
    return NULL;
}
void gpio_close(gpio_t *g) { (void)g; }
int gpio_detect_header_chip(char *out, size_t cap) { (void)out; (void)cap; return -1; }
int gpio_ptt_init(gpio_t *g, int r, unsigned int l, bool a) { (void)g; (void)r; (void)l; (void)a; return -1; }
int gpio_ptt_set(gpio_t *g, int r, bool t) { (void)g; (void)r; (void)t; return -1; }
int gpio_cos_init(gpio_t *g, int r, unsigned int l, bool a) { (void)g; (void)r; (void)l; (void)a; return -1; }
int gpio_cos_fd(gpio_t *g, int r) { (void)g; (void)r; return -1; }
bool gpio_cos_read(gpio_t *g, int r) { (void)g; (void)r; return false; }
int gpio_cos_drain(gpio_t *g, int r) { (void)g; (void)r; return -1; }
int gpio_read_grounded(gpio_t *g, unsigned int l) { (void)g; (void)l; return -1; }
