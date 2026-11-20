/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "detect.h"

#include <stdio.h>

const char *detect_pi_model(char *buf, size_t cap)
{
    if (cap == 0) return buf;
    buf[0] = '\0';
    FILE *f = fopen("/proc/device-tree/model", "rb");
    if (f) {
        size_t n = fread(buf, 1, cap - 1, f);
        fclose(f);
        buf[n] = '\0';   /* DT strings are NUL-terminated; printf stops there */
    }
    if (buf[0] == '\0')
        snprintf(buf, cap, "unknown");
    return buf;
}
