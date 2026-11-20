/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "jitter.h"

void jitter_init(jitter_t *j)
{
    j->have_hi = false;
    j->hi = 0;
}

void jitter_reset(jitter_t *j)
{
    j->have_hi = false;
    j->hi = 0;
}

bool jitter_is_replay(const jitter_t *j, uint32_t counter)
{
    if (!j->have_hi)
        return false;          /* first frame of the session is always fresh */
    return counter <= j->hi;   /* <= last accepted => replay / late / dup */
}

void jitter_commit(jitter_t *j, uint32_t counter)
{
    if (!j->have_hi || counter > j->hi) {
        j->hi = counter;
        j->have_hi = true;
    }
}
