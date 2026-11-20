/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — DNS SRV resolution for the reflector host.
 *
 * Looks up _svxreflector._tcp.<domain> (matches SVXConnect-OSX SRVResolver),
 * sorted by priority ascending then weight descending. Falls back to a static
 * "host:port,host:port" list.
 */
#ifndef SMARTSPOT_SRV_H
#define SMARTSPOT_SRV_H

#include <stdint.h>

#define SRV_DEFAULT_PORT 5300

typedef struct {
    char     host[256];
    uint16_t port;
    uint16_t priority;
    uint16_t weight;
} srv_record_t;

/* Resolve _svxreflector._tcp.<domain>; fill out[] (up to cap), sorted.
 * Returns the number of records (0 on no-SRV / error). */
int srv_resolve(const char *domain, srv_record_t *out, int cap);

/* Parse "host1:port,host2" (missing port => default_port). Returns count. */
int srv_parse_hostlist(const char *hosts, uint16_t default_port,
                       srv_record_t *out, int cap);

#endif /* SMARTSPOT_SRV_H */
