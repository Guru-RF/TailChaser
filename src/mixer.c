/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "mixer.h"
#include "log.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <alsa/asoundlib.h>

/* A percentage as a control value, exactly the way `amixer set NAME N%` works
 * it out (alsa-utils rounds up) — so "77%" here is the very same register
 * value as "77%" in the script the node used before. */
static long pct_to_value(long min, long max, int pct)
{
    long v = (long)ceil(pct * (max - min) * 0.01 + min);
    return v < min ? min : v > max ? max : v;
}

/* Set a simple-mixer volume element (matches `amixer set 'NAME' pct%`).
 * Returns 0 on success, -1 if the control is missing / has no range / the set
 * call failed — so callers (the #09# auto-tune) don't claim a gain change that
 * never happened. */
static int set_simple_volume(snd_mixer_t *mix, const char *name, int pct, int playback)
{
    snd_mixer_selem_id_t *sid;
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, name);

    snd_mixer_elem_t *e = snd_mixer_find_selem(mix, sid);
    if (!e) {
        log_warn("mixer: control '%s' not found", name);
        return -1;
    }
    long min = 0, max = 0;
    int rc;
    if (playback) {
        if (snd_mixer_selem_get_playback_volume_range(e, &min, &max) < 0) {
            log_warn("mixer: '%s' has no playback volume range", name);
            return -1;
        }
        rc = snd_mixer_selem_set_playback_volume_all(e, pct_to_value(min, max, pct));
    } else {
        if (snd_mixer_selem_get_capture_volume_range(e, &min, &max) < 0) {
            log_warn("mixer: '%s' has no capture volume range", name);
            return -1;
        }
        rc = snd_mixer_selem_set_capture_volume_all(e, pct_to_value(min, max, pct));
    }
    if (rc < 0) {
        log_warn("mixer: failed to set '%s' = %d%%", name, pct);
        return -1;
    }
    log_info("mixer: %s -> %d%% (%ld of %ld..%ld)", name, pct, pct_to_value(min, max, pct), min, max);
    return 0;
}

/* Set a raw control by MIXER-interface name (matches
 * `amixer cset iface=MIXER,name='NAME' value`). Handles INTEGER / BOOLEAN /
 * ENUMERATED element types, applied to every channel of the control. */
static void set_named_ctl(snd_ctl_t *ctl, const char *name, long value)
{
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *val;
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_alloca(&val);

    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(id, name);

    snd_ctl_elem_info_set_id(info, id);
    if (snd_ctl_elem_info(ctl, info) < 0) {
        log_warn("mixer: control '%s' not present", name);
        return;
    }

    snd_ctl_elem_value_set_id(val, id);
    unsigned int count = snd_ctl_elem_info_get_count(info);
    snd_ctl_elem_type_t type = snd_ctl_elem_info_get_type(info);

    for (unsigned int i = 0; i < count; i++) {
        switch (type) {
        case SND_CTL_ELEM_TYPE_INTEGER:
            snd_ctl_elem_value_set_integer(val, i, value);
            break;
        case SND_CTL_ELEM_TYPE_BOOLEAN:
            snd_ctl_elem_value_set_boolean(val, i, value ? 1 : 0);
            break;
        case SND_CTL_ELEM_TYPE_ENUMERATED:
            snd_ctl_elem_value_set_enumerated(val, i, (unsigned int)value);
            break;
        default:
            log_warn("mixer: control '%s' has unsupported type %d", name, type);
            return;
        }
    }
    if (snd_ctl_elem_write(ctl, val) < 0)
        log_warn("mixer: failed to set '%s' = %ld", name, value);
    else
        log_debug("mixer: %s = %ld", name, value);
}

int mixer_apply_raw_audio(const char *card, int headphone_pct, int capture_pct)
{
    /* --- volumes via the simple mixer (like `amixer set`) --- */
    snd_mixer_t *mix = NULL;
    if (snd_mixer_open(&mix, 0) == 0 &&
        snd_mixer_attach(mix, card) == 0 &&
        snd_mixer_selem_register(mix, NULL, NULL) == 0 &&
        snd_mixer_load(mix) == 0) {
        if (headphone_pct >= 0)
            set_simple_volume(mix, "Headphone", headphone_pct, 1);  /* DAC -> radio */
        if (capture_pct >= 0)
            set_simple_volume(mix, "Capture",   capture_pct,   0);  /* radio -> ADC */
    } else {
        log_warn("mixer: simple mixer open failed on %s", card);
    }
    if (mix)
        snd_mixer_close(mix);

    /* --- raw controls via snd_ctl (like `amixer cset iface=MIXER,...`) --- */
    snd_ctl_t *ctl = NULL;
    if (snd_ctl_open(&ctl, card, 0) < 0) {
        log_error("mixer: cannot open control for %s", card);
        return -1;
    }

    /* Disable ALC and de-emphasis; flatten the path (from hotspot_volume). */
    set_named_ctl(ctl, "DAC Deemphasis Switch", 0);
    set_named_ctl(ctl, "ALC Function",          0);
    set_named_ctl(ctl, "ALC Target",            0);
    set_named_ctl(ctl, "ALC Mode",              1);
    set_named_ctl(ctl, "ALC Attack",            0);
    set_named_ctl(ctl, "ALC Decay",             0);
    set_named_ctl(ctl, "ALC Hold Time",         0);
    set_named_ctl(ctl, "ALC Max Gain",          0);
    set_named_ctl(ctl, "ALC Min Gain",          0);
    set_named_ctl(ctl, "ADC High Pass Filter Switch", 0);

    snd_ctl_close(ctl);
    log_info("mixer: raw-audio profile applied on %s", card);
    return 0;
}

int mixer_set_capture(const char *card, int capture_pct)
{
    snd_mixer_t *mix = NULL;
    if (snd_mixer_open(&mix, 0) != 0 || !mix)
        return -1;
    int rc = -1;
    if (snd_mixer_attach(mix, card) == 0 &&
        snd_mixer_selem_register(mix, NULL, NULL) == 0 &&
        snd_mixer_load(mix) == 0) {
        rc = set_simple_volume(mix, "Capture", capture_pct, 0);  /* 0 ok, -1 fail */
    } else {
        log_warn("mixer: capture-set open failed on %s", card);
    }
    snd_mixer_close(mix);
    return rc;
}

void mixer_card_from_device(const char *dev, char *out, size_t cap)
{
    const char *colon = dev ? strchr(dev, ':') : NULL;   /* "plughw:0" -> "hw:0" */
    if (colon)
        snprintf(out, cap, "hw:%s", colon + 1);
    else
        snprintf(out, cap, "hw:0");
}

bool mixer_card_is_wm8960(const char *card, char *name_out, size_t cap)
{
    snd_ctl_t *ctl = NULL;
    snd_ctl_card_info_t *info;
    snd_ctl_card_info_alloca(&info);
    if (name_out && cap) snprintf(name_out, cap, "?");
    if (snd_ctl_open(&ctl, card, 0) < 0)
        return false;
    bool is = false;
    if (snd_ctl_card_info(ctl, info) == 0) {
        const char *n[] = { snd_ctl_card_info_get_name(info), snd_ctl_card_info_get_driver(info),
                            snd_ctl_card_info_get_mixername(info), snd_ctl_card_info_get_longname(info) };
        if (name_out && cap) snprintf(name_out, cap, "%s", n[0] ? n[0] : "?");
        for (int i = 0; i < 4; i++)
            if (n[i] && (strcasestr(n[i], "wm8960") != NULL))
                is = true;
    }
    snd_ctl_close(ctl);
    return is;
}
