/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — WM8960 mixer setup for "flat/raw" audio.
 *
 * Programmatic equivalent of ../Analog-HotSPOT-SVXLink/hotspot_volume:
 *   - Headphone (DAC -> radio) and Capture (radio -> ADC) levels
 *   - ALC off, DAC de-emphasis off, ADC high-pass off
 * so the audio path is flat/unprocessed and the daemon's own filter chain
 * (ARCHITECTURE.md §6) is the only thing shaping the audio.
 *
 * Forked verbatim from AetherLink270-Base.
 */
#ifndef SMARTSPOT_MIXER_H
#define SMARTSPOT_MIXER_H

#include <stddef.h>

/* card e.g. "hw:0". headphone_pct/capture_pct are 0..100; below zero leaves
 * that level as it is.
 * Returns 0 if the card opened (individual controls are best-effort and
 * logged), -1 if the card could not be opened at all. */
int mixer_apply_raw_audio(const char *card, int headphone_pct, int capture_pct);

/* Set only the WM8960 "Capture" gain at runtime (0..100). Used by the #09#
 * input auto-tune. Returns 0 on success, -1 if the card could not be opened. */
int mixer_set_capture(const char *card, int capture_pct);

/* Derive the control card ("hw:N") from an ALSA device string ("plughw:N"). */
void mixer_card_from_device(const char *dev, char *out, size_t cap);

/* The card's name as ALSA reports it ("?" if it cannot be opened), and whether
 * it is the WM8960 this mixer profile was written for. */
#include <stdbool.h>
bool mixer_card_is_wm8960(const char *card, char *name_out, size_t cap);

#endif /* SMARTSPOT_MIXER_H */
