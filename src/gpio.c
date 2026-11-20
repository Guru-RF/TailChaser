/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "gpio.h"
#include "log.h"
#include "config.h"   /* CFG_MAX_RADIOS */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gpiod.h>     /* libgpiod v2 API */

/*
 * THE libgpiod VERSION FIREWALL. This file is written against libgpiod v2
 * (the version on the current Raspberry Pi OS image and Debian Trixie). It is
 * one of the two files that include <gpiod.h>; gpio.h stays version-agnostic. Its
 * twin for an older v1.6 system (Bookworm) is gpio_v1.c — the Makefile builds
 * whichever matches the installed library.
 */

#define CONSUMER "tailchaser"

struct gpio {
    struct gpiod_chip              *chip;
    struct gpiod_line_request      *ptt[CFG_MAX_RADIOS];
    unsigned int                    ptt_off[CFG_MAX_RADIOS];
    struct gpiod_line_request      *cos[CFG_MAX_RADIOS];
    unsigned int                    cos_off[CFG_MAX_RADIOS];
    struct gpiod_edge_event_buffer *evbuf;
};

/* Request a single line with the given settings. Returns NULL on failure. */
static struct gpiod_line_request *
request_line(struct gpiod_chip *chip, unsigned int offset,
             enum gpiod_line_direction dir, bool active_low,
             enum gpiod_line_edge edge, enum gpiod_line_bias bias,
             enum gpiod_line_value out_val)
{
    struct gpiod_line_settings  *settings = gpiod_line_settings_new();
    struct gpiod_line_config    *lcfg     = gpiod_line_config_new();
    struct gpiod_request_config *rcfg     = gpiod_request_config_new();
    struct gpiod_line_request   *req      = NULL;

    if (!settings || !lcfg || !rcfg)
        goto out;

    gpiod_line_settings_set_direction(settings, dir);
    if (active_low)
        gpiod_line_settings_set_active_low(settings, true);
    if (dir == GPIOD_LINE_DIRECTION_OUTPUT)
        gpiod_line_settings_set_output_value(settings, out_val);
    if (edge != GPIOD_LINE_EDGE_NONE)
        gpiod_line_settings_set_edge_detection(settings, edge);
    if (bias != GPIOD_LINE_BIAS_AS_IS)
        gpiod_line_settings_set_bias(settings, bias);

    if (gpiod_line_config_add_line_settings(lcfg, &offset, 1, settings) != 0)
        goto out;
    gpiod_request_config_set_consumer(rcfg, CONSUMER);

    req = gpiod_chip_request_lines(chip, rcfg, lcfg);

out:
    if (rcfg)     gpiod_request_config_free(rcfg);
    if (lcfg)     gpiod_line_config_free(lcfg);
    if (settings) gpiod_line_settings_free(settings);
    return req;
}

/* First /dev/gpiochipN with >= 40 lines whose label contains `needle`. */
static int find_chip_by_label(const char *needle, char *out, size_t cap)
{
    for (int i = 0; i < 16; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/gpiochip%d", i);
        struct gpiod_chip *chip = gpiod_chip_open(path);
        if (!chip)
            continue;
        int matched = 0;
        struct gpiod_chip_info *info = gpiod_chip_get_info(chip);
        if (info) {
            const char *label = gpiod_chip_info_get_label(info);
            size_t lines = gpiod_chip_info_get_num_lines(info);
            if (label && lines >= 40 && strstr(label, needle)) {
                snprintf(out, cap, "%s", path);
                matched = 1;
            }
            gpiod_chip_info_free(info);
        }
        gpiod_chip_close(chip);
        if (matched)
            return 0;
    }
    return -1;
}

int gpio_detect_header_chip(char *out, size_t cap)
{
    /* The 40-pin header controller, in preference order. On a Pi 5 BOTH the
     * RP1 (header) and the SoC (bcm2712) banks have >= 40 lines, so RP1 must
     * be matched FIRST. Zero/3 = bcm2835, Pi 4 = bcm2711 (no rp1 present). */
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
    g->evbuf = gpiod_edge_event_buffer_new(16);
    if (!g->evbuf) {
        log_error("gpio: cannot allocate edge-event buffer");
        gpiod_chip_close(g->chip);
        free(g);
        return NULL;
    }
    log_info("gpio: opened %s", chip_path);
    return g;
}

void gpio_close(gpio_t *g)
{
    if (!g)
        return;
    for (int i = 0; i < CFG_MAX_RADIOS; i++) {
        if (g->ptt[i]) gpiod_line_request_release(g->ptt[i]);
        if (g->cos[i]) gpiod_line_request_release(g->cos[i]);
    }
    if (g->evbuf)
        gpiod_edge_event_buffer_free(g->evbuf);
    if (g->chip)
        gpiod_chip_close(g->chip);
    free(g);
}

int gpio_ptt_init(gpio_t *g, int radio_idx, unsigned int line, bool active_low)
{
    if (radio_idx < 0 || radio_idx >= CFG_MAX_RADIOS)
        return -1;
    struct gpiod_line_request *req =
        request_line(g->chip, line, GPIOD_LINE_DIRECTION_OUTPUT, active_low,
                     GPIOD_LINE_EDGE_NONE, GPIOD_LINE_BIAS_AS_IS,
                     GPIOD_LINE_VALUE_INACTIVE);   /* idle = not transmitting */
    if (!req) {
        log_error("gpio: ptt request line %u failed: %s", line, strerror(errno));
        return -1;
    }
    g->ptt[radio_idx] = req;
    g->ptt_off[radio_idx] = line;
    log_info("gpio: PTT radio %d on line %u (active_%s)", radio_idx, line,
             active_low ? "low" : "high");
    return 0;
}

int gpio_ptt_set(gpio_t *g, int radio_idx, bool transmit)
{
    if (radio_idx < 0 || radio_idx >= CFG_MAX_RADIOS || !g->ptt[radio_idx])
        return -1;
    enum gpiod_line_value v = transmit ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
    if (gpiod_line_request_set_value(g->ptt[radio_idx], g->ptt_off[radio_idx], v) != 0) {
        log_error("gpio: ptt set radio %d failed: %s", radio_idx, strerror(errno));
        return -1;
    }
    return 0;
}

int gpio_cos_init(gpio_t *g, int radio_idx, unsigned int line, bool active_low)
{
    if (radio_idx < 0 || radio_idx >= CFG_MAX_RADIOS)
        return -1;
    /* The SA818/868 SQ (COS) output is open-drain (high-Z when idle, driven
     * low when squelch opens), so enable an internal pull-up: idle reads high,
     * an asserted signal pulls it low. Combined with cos_active_low this gives
     * a clean both-edge transition. */
    struct gpiod_line_request *req =
        request_line(g->chip, line, GPIOD_LINE_DIRECTION_INPUT, active_low,
                     GPIOD_LINE_EDGE_BOTH, GPIOD_LINE_BIAS_PULL_UP,
                     GPIOD_LINE_VALUE_INACTIVE);
    if (!req) {
        log_error("gpio: cos request line %u failed: %s", line, strerror(errno));
        return -1;
    }
    g->cos[radio_idx] = req;
    g->cos_off[radio_idx] = line;
    log_info("gpio: COS radio %d on line %u (active_%s)", radio_idx, line,
             active_low ? "low" : "high");
    return 0;
}

int gpio_cos_fd(gpio_t *g, int radio_idx)
{
    if (radio_idx < 0 || radio_idx >= CFG_MAX_RADIOS || !g->cos[radio_idx])
        return -1;
    return gpiod_line_request_get_fd(g->cos[radio_idx]);
}

bool gpio_cos_read(gpio_t *g, int radio_idx)
{
    if (radio_idx < 0 || radio_idx >= CFG_MAX_RADIOS || !g->cos[radio_idx])
        return false;
    /* value honours the active-low setting → ACTIVE means "open". */
    return gpiod_line_request_get_value(g->cos[radio_idx], g->cos_off[radio_idx])
           == GPIOD_LINE_VALUE_ACTIVE;
}

int gpio_cos_drain(gpio_t *g, int radio_idx)
{
    if (radio_idx < 0 || radio_idx >= CFG_MAX_RADIOS || !g->cos[radio_idx])
        return -1;
    struct gpiod_line_request *req = g->cos[radio_idx];
    /* Drain queued edge events without blocking (wait with a 0 ns timeout),
     * then re-read the level rather than trusting edge direction. */
    for (int guard = 0; guard < 64; guard++) {
        int pending = gpiod_line_request_wait_edge_events(req, 0);
        if (pending != 1)
            break;
        gpiod_line_request_read_edge_events(req, g->evbuf, 16);
    }
    return gpio_cos_read(g, radio_idx) ? 1 : 0;
}

int gpio_read_grounded(gpio_t *g, unsigned int line)
{
    struct gpiod_line_request *req =
        request_line(g->chip, line, GPIOD_LINE_DIRECTION_INPUT, false,
                     GPIOD_LINE_EDGE_NONE, GPIOD_LINE_BIAS_PULL_UP,
                     GPIOD_LINE_VALUE_INACTIVE);
    if (!req) {
        log_error("gpio: detect request line %u failed: %s", line, strerror(errno));
        return -1;
    }
    enum gpiod_line_value v = gpiod_line_request_get_value(req, line);
    gpiod_line_request_release(req);
    /* grounded == reads physical low == INACTIVE (active_low not set) */
    return (v == GPIOD_LINE_VALUE_INACTIVE) ? 1 : 0;
}
