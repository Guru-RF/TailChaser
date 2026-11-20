/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — Opus encode/decode for one mono radio channel.
 *
 * 16 kHz mono, tuned for narrowband FM voice, to match the SvxReflector codec
 * negotiation. application = 0 (VOIP) or 1 (AUDIO) — the reflector's negotiated
 * codec[] list decides which; see ARCHITECTURE.md §6 / §15 Q3.
 *
 * Forked from AetherLink270-Base (retuned 48k->16k; application now a param).
 */
#ifndef SMARTSPOT_OPUS_CODEC_H
#define SMARTSPOT_OPUS_CODEC_H

#include <stdint.h>

typedef struct opus_chan opus_chan_t;

/* application: 0 = OPUS_APPLICATION_VOIP, 1 = OPUS_APPLICATION_AUDIO. */
opus_chan_t *opus_chan_new(int sample_rate, int bitrate, int complexity, int application);
void         opus_chan_free(opus_chan_t *c);

/* Encode `frames` mono S16 samples. Returns encoded byte count, or -1. */
int opus_chan_encode(opus_chan_t *c, const int16_t *pcm, int frames,
                     uint8_t *out, int out_cap);

/* Decode one packet into `frames` mono S16 samples. pkt == NULL (len 0) runs
 * packet-loss concealment. Returns samples decoded, or -1. */
int opus_chan_decode(opus_chan_t *c, const uint8_t *pkt, int len,
                     int16_t *pcm, int frames);

#endif /* SMARTSPOT_OPUS_CODEC_H */
