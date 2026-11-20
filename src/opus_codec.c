/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "opus_codec.h"
#include "log.h"

#include <stdlib.h>
#include <opus/opus.h>

struct opus_chan {
    OpusEncoder *enc;
    OpusDecoder *dec;
};

opus_chan_t *opus_chan_new(int sample_rate, int bitrate, int complexity, int application)
{
    opus_chan_t *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;

    int app = (application == 1) ? OPUS_APPLICATION_AUDIO : OPUS_APPLICATION_VOIP;

    int err = 0;
    c->enc = opus_encoder_create(sample_rate, 1, app, &err);
    if (err != OPUS_OK || !c->enc) {
        log_error("opus: encoder_create: %s", opus_strerror(err));
        free(c);
        return NULL;
    }
    c->dec = opus_decoder_create(sample_rate, 1, &err);
    if (err != OPUS_OK || !c->dec) {
        log_error("opus: decoder_create: %s", opus_strerror(err));
        opus_encoder_destroy(c->enc);
        free(c);
        return NULL;
    }

    opus_encoder_ctl(c->enc, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(c->enc, OPUS_SET_COMPLEXITY(complexity));
    opus_encoder_ctl(c->enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(c->enc, OPUS_SET_VBR(1));
    opus_encoder_ctl(c->enc, OPUS_SET_VBR_CONSTRAINT(1));
    opus_encoder_ctl(c->enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(c->enc, OPUS_SET_PACKET_LOSS_PERC(5));
    opus_encoder_ctl(c->enc, OPUS_SET_DTX(0));

    return c;
}

void opus_chan_free(opus_chan_t *c)
{
    if (!c)
        return;
    if (c->enc) opus_encoder_destroy(c->enc);
    if (c->dec) opus_decoder_destroy(c->dec);
    free(c);
}

int opus_chan_encode(opus_chan_t *c, const int16_t *pcm, int frames,
                     uint8_t *out, int out_cap)
{
    int n = opus_encode(c->enc, pcm, frames, out, out_cap);
    if (n < 0) {
        log_warn("opus: encode: %s", opus_strerror(n));
        return -1;
    }
    return n;
}

int opus_chan_decode(opus_chan_t *c, const uint8_t *pkt, int len,
                     int16_t *pcm, int frames)
{
    /* pkt == NULL -> PLC (conceal a lost/late frame). */
    int n = opus_decode(c->dec, pkt, len, pcm, frames, 0);
    if (n < 0) {
        log_warn("opus: decode: %s", opus_strerror(n));
        return -1;
    }
    return n;
}
