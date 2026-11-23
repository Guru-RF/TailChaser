/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* The callsign tag: who is talking, as a noise-like signal far under the
 * speech (docs/TAG.md). A listener hears a little more hiss; a phone or a
 * small device next to the radio's speaker reads the callsign. */
#ifndef TAILCHASER_TAG_H
#define TAILCHASER_TAG_H

#include <stdbool.h>
#include <stdint.h>

#define TAG_RATE        16000       /* the only sample rate it is made for    */
#define TAG_SYMBOLS     164         /* per frame: 16 to find it, 148 coded    */
#define TAG_CHIPS       32          /* per symbol                             */
#define TAG_SPC         8           /* samples per chip: 2000 chips/s         */
#define TAG_FRAME       (TAG_SYMBOLS * TAG_CHIPS * TAG_SPC)     /* 41984 = 2.624 s */
#define TAG_CALL_MAX    9

typedef struct tag tag_t;

/* amplitude: RMS of the tag in sample units (32767 = full scale). */
tag_t *tag_new(float rms);
void   tag_free(tag_t *t);

/* Who the next frames are about. Any thread; takes effect at the next frame
 * boundary. `local`: heard on the repeater's own input. */
void   tag_set(tag_t *t, const char *call, bool local);

/* The audio thread: add the tag to one period that goes to the transmitter.
 * `keyed` false: nothing is added, and the frame clock starts again at the
 * next key-up. */
void   tag_mix(tag_t *t, int16_t *pcm, int n, bool keyed);

/* For tests and tools: the 164 symbol signs (+1/-1) of a message. */
void   tag_signs(const char *call, bool local, int8_t signs[TAG_SYMBOLS]);

#endif
