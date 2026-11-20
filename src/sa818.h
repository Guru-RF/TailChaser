/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — SA818 / SA818Pro / SA868 FM module driver.
 *
 * Plain-C driver for the NiceRF SA8x8-class FM modules: serial AT-command
 * protocol over a UART, DMOCONNECT handshake, model auto-detection, channel
 * programming (DMOSETGROUP) with the SA818Pro READGROUP-readback verification
 * quirk, filters, volume, tail, and RSSI.
 *
 * The hotspot programs a FIXED channel (freq, bandwidth, TX/RX CTCSS, squelch)
 * once at startup; the chip then performs squelch + (where supported in
 * silicon) CTCSS gating on its SQ pin, which is wired to the COS GPIO. There is
 * no software CTCSS and no CTCSS->TG mapping (ARCHITECTURE.md §5).
 *
 * All calls are blocking and are intended to run on the serial worker thread.
 *
 * Forked verbatim from AetherLink270-Base.
 */
#ifndef SMARTSPOT_SA818_H
#define SMARTSPOT_SA818_H

#include <stdbool.h>
#include <stdint.h>

#define SA818_DEFAULT_BAUD 9600
#define SA818_CTCSS_COUNT  39      /* index 0 ("0.0") = no tone */

typedef struct sa818 sa818_t;

typedef struct {
    int      bw;            /* 0 = narrow, 1 = wide       */
    uint32_t tx_freq_hz;
    uint32_t rx_freq_hz;
    int      tx_ctcss;      /* 4-digit index parsed to int */
    int      squelch;       /* 0..8                        */
    int      rx_ctcss;
} sa818_group_t;

/* Open the serial port and perform the DMOCONNECT handshake + model detect.
 * Returns NULL on error (logged). */
sa818_t *sa818_open(const char *port, int baud);
void     sa818_close(sa818_t *dev);

bool        sa818_is_pro(const sa818_t *dev);
const char *sa818_firmware(const sa818_t *dev);

/* Program a channel. Frequencies in Hz, CTCSS as table index 0..38 (0 = none),
 * squelch 0..8, bw 0/1. Sends DMOSETGROUP and verifies via DMOREADGROUP.
 * Returns 0 on success, -1 on failure (logged). */
int sa818_set_group(sa818_t *dev, int bw, uint32_t tx_hz, uint32_t rx_hz,
                    int ctcss_tx_idx, int ctcss_rx_idx, int squelch);

int sa818_read_group(sa818_t *dev, sa818_group_t *out);

/* 0 = enable, 1 = disable for each filter (matches the SA818 AT semantics). */
int sa818_set_filter(sa818_t *dev, int emphasis, int highpass, int lowpass);

/* level 1..8 */
int sa818_set_volume(sa818_t *dev, int level);

/* open != 0 -> CTCSS tail tone on */
int sa818_set_tail(sa818_t *dev, int open);

/* Reads RSSI 0..255 into *out. Returns 0 on success. */
int sa818_rssi(sa818_t *dev, int *out);

/* ---- CTCSS table helpers ---- */
/* Parse "0"/"none"/"88.5"/"12" (a tone in Hz or a bare index) to an index
 * 0..38. Returns -1 if not recognized. */
int         sa818_ctcss_parse(const char *s);
const char *sa818_ctcss_str(int idx);   /* "88.5"; NULL if out of range */
float       sa818_ctcss_hz(int idx);    /* 0.0 if none/out of range     */

#endif /* SMARTSPOT_SA818_H */
