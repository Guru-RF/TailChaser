/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static log_level_t g_level = LOG_INFO;

static const char *level_tag(log_level_t l)
{
    switch (l) {
    case LOG_ERROR: return "ERR";
    case LOG_WARN:  return "WRN";
    case LOG_INFO:  return "INF";
    case LOG_DEBUG: return "DBG";
    default:        return "???";
    }
}

void log_set_level(log_level_t level) { g_level = level; }
log_level_t log_get_level(void) { return g_level; }

int log_level_from_name(const char *name)
{
    if (!name) return -1;
    if (!strcasecmp(name, "error") || !strcasecmp(name, "err")) return LOG_ERROR;
    if (!strcasecmp(name, "warn")  || !strcasecmp(name, "warning")) return LOG_WARN;
    if (!strcasecmp(name, "info"))  return LOG_INFO;
    if (!strcasecmp(name, "debug")) return LOG_DEBUG;
    return -1;
}

void log_msg(log_level_t level, const char *fmt, ...)
{
    if (level > g_level)
        return;

    char ts[32];
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct tm tm;
    localtime_r(&now.tv_sec, &tm);
    int n = (int)strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    snprintf(ts + n, sizeof ts - (size_t)n, ".%03ld", now.tv_nsec / 1000000);

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    fprintf(stderr, "%s [%s] %s\n", ts, level_tag(level), buf);
}
