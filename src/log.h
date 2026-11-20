/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — leveled logging.
 *
 * Logs to stderr (captured by journald when run under systemd). Levels are
 * filtered at runtime via log_set_level(). Thread-safe enough for our use:
 * each message is formatted into a local buffer and written with a single
 * fputs/fprintf, which glibc serializes per-FILE.
 *
 * Forked verbatim from AetherLink270-Base.
 */
#ifndef SMARTSPOT_LOG_H
#define SMARTSPOT_LOG_H

#include <stdarg.h>

typedef enum {
    LOG_ERROR = 0,
    LOG_WARN  = 1,
    LOG_INFO  = 2,
    LOG_DEBUG = 3,
} log_level_t;

void log_set_level(log_level_t level);
log_level_t log_get_level(void);

/* Parse a level name ("error"|"warn"|"info"|"debug"); returns -1 if unknown. */
int log_level_from_name(const char *name);

void log_msg(log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define log_error(...) log_msg(LOG_ERROR, __VA_ARGS__)
#define log_warn(...)  log_msg(LOG_WARN,  __VA_ARGS__)
#define log_info(...)  log_msg(LOG_INFO,  __VA_ARGS__)
#define log_debug(...) log_msg(LOG_DEBUG, __VA_ARGS__)

#endif /* SMARTSPOT_LOG_H */
