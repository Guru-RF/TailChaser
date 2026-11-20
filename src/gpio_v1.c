/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — GPIO on libgpiod v1.x (Debian Bookworm, libgpiod 1.6).
 *
 * Same interface and behaviour as gpio.c, which is written against libgpiod
 * v2 (Trixie). The Makefile picks one of the two from the installed library
 * version; this is the only other file that includes <gpiod.h>.
 */
#include "gpio.h"
#include "log.h"
#include "config.h"   /* CFG_MAX_RADIOS */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <gpiod.h>     /* libgpiod v1 API */

#define CONSUMER "tailchaser"

struct gpio {
    struct gpiod_chip *chip;
    struct gpiod_line *ptt[CFG_MAX_RADIOS];
    struct gpiod_line *cos[CFG_MAX_RADIOS];
};

/* First /dev/gpiochipN with >= 40 lines whose label contains `needle`. */
static int find_chip_by_label(const char *needle, char *out, size_t cap)
{
    for (int i = 0; i < 16; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/gpiochip%d", i);
        struct gpiod_chip *chip = gpiod_chip_open(path);
        if (!chip)
            continue;
        const char *label = gpiod_chip_label(chip);
        int matched = label && gpiod_chip_num_lines(chip) >= 40 && strstr(label, needle);
        if (matched)
            snprintf(out, cap, "%s", path);
        gpiod_chip_close(chip);
        if (matched)
            return 0;
    }
    return -1;
}

int gpio_detect_header_chip(char *out, size_t cap)
{
    /* Same preference order as the v2 file: Pi 5 RP1 first, then the SoC banks. */
    static const char *const labels[] = { "rp1", "bcm2835", "bcm2711", "bcm2712" };
    for (size_t i = 0; i < sizeof labels / sizeof labels[0]; i++)
        if (find_chip_by_label(labels[i], out, cap) == 0)
            return 0;
    return -1;
}

gpio_t *gpio_open(const char *chip_path)
{
    gpio_t *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    g->chip = gpiod_chip_open(chip_path);
    if (!g->chip) {
        log_error("gpio: cannot open chip %s: %s", chip_path, strerror(errno));
        free(g);
        return NULL;
    }
    log_info("gpio: opened %s (libgpiod %s)", chip_path, gpiod_version_string());
    return g;
}

void gpio_close(gpio_t *g)
{
    if (!g)
        return;
    for (int i = 0; i < CFG_MAX_RADIOS; i++) {
        if (g->ptt[i]) gpiod_line_release(g->ptt[i]);
        if (g->cos[i]) gpiod_line_release(g->cos[i]);
    }
    if (g->chip)
        gpiod_chip_close(g->chip);
    free(g);
}

/* Request one line. Returns NULL on failure. */
static struct gpiod_line *request_line(gpio_t *g, unsigned int offset, int request_type,
                                       bool active_low, bool pull_up, int default_val)
{
    struct gpiod_line *line = gpiod_chip_get_line(g->chip, offset);
    if (!line)
        return NULL;
    struct gpiod_line_request_config rc = {
        .consumer     = CONSUMER,
        .request_type = request_type,
        .flags        = (active_low ? GPIOD_LINE_REQUEST_FLAG_ACTIVE_LOW : 0) |
                        (pull_up ? GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP : 0),
    };
    if (gpiod_line_request(line, &rc, default_val) != 0)
        return NULL;
    return line;
}

int gpio_ptt_init(gpio_t *g, int radio_idx, unsigned int line, bool active_low)
{
    if (radio_idx < 0 || radio_idx >= CFG_MAX_RADIOS)
        return -1;
    /* default value 0 = logically inactive = not transmitting */
    struct gpiod_line *l = request_line(g, line, GPIOD_LINE_REQUEST_DIRECTION_OUTPUT,
                                        active_low, false, 0);
    if (!l) {
        log_error("gpio: ptt request line %u failed: %s", line, strerror(errno));
        return -1;
    }
    g->ptt[radio_idx] = l;
    log_info("gpio: PTT radio %d on line %u (active_%s)", radio_idx, line,
             active_low ? "low" : "high");
    return 0;
}

int gpio_ptt_set(gpio_t *g, int radio_idx, bool transmit)
{
    if (radio_idx < 0 || radio_idx >= CFG_MAX_RADIOS || !g->ptt[radio_idx])
        return -1;
    if (gpiod_line_set_value(g->ptt[radio_idx], transmit ? 1 : 0) != 0) {
        log_error("gpio: ptt set radio %d failed: %s", radio_idx, strerror(errno));
        return -1;
    }
    return 0;
}

int gpio_cos_init(gpio_t *g, int radio_idx, unsigned int line, bool active_low)
{
    if (radio_idx < 0 || radio_idx >= CFG_MAX_RADIOS)
        return -1;
    /* The SA818 SQ output is open-drain: pull it up so idle reads high. */
    struct gpiod_line *l = request_line(g, line, GPIOD_LINE_REQUEST_EVENT_BOTH_EDGES,
                                        active_low, true, 0);
    if (!l) {
        log_error("gpio: cos request line %u failed: %s", line, strerror(errno));
        return -1;
    }
    g->cos[radio_idx] = l;
    log_info("gpio: COS radio %d on line %u (active_%s)", radio_idx, line,
             active_low ? "low" : "high");
    return 0;
}

int gpio_cos_fd(gpio_t *g, int radio_idx)
{
    if (radio_idx < 0 || radio_idx >= CFG_MAX_RADIOS || !g->cos[radio_idx])
        return -1;
    return gpiod_line_event_get_fd(g->cos[radio_idx]);
}

bool gpio_cos_read(gpio_t *g, int radio_idx)
{
    if (radio_idx < 0 || radio_idx >= CFG_MAX_RADIOS || !g->cos[radio_idx])
        return false;
    /* the value honours the active-low flag: 1 means "open" */
    return gpiod_line_get_value(g->cos[radio_idx]) == 1;
}

int gpio_cos_drain(gpio_t *g, int radio_idx)
{
    if (radio_idx < 0 || radio_idx >= CFG_MAX_RADIOS || !g->cos[radio_idx])
        return -1;
    struct gpiod_line *l = g->cos[radio_idx];
    /* Drain queued edge events without blocking, then re-read the level rather
     * than trusting edge direction. */
    for (int guard = 0; guard < 64; guard++) {
        struct timespec zero = { 0, 0 };
        if (gpiod_line_event_wait(l, &zero) != 1)
            break;
        struct gpiod_line_event ev;
        if (gpiod_line_event_read(l, &ev) != 0)
            break;
    }
    return gpio_cos_read(g, radio_idx) ? 1 : 0;
}

int gpio_read_grounded(gpio_t *g, unsigned int line)
{
    struct gpiod_line *l = request_line(g, line, GPIOD_LINE_REQUEST_DIRECTION_INPUT,
                                        false, true, 0);
    if (!l) {
        log_error("gpio: detect request line %u failed: %s", line, strerror(errno));
        return -1;
    }
    int v = gpiod_line_get_value(l);
    gpiod_line_release(l);
    return v == 0 ? 1 : 0;      /* grounded == reads physical low */
}
