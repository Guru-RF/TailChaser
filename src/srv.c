/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "srv.h"
#include "log.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>

/* Insertion-sort by priority asc, then weight desc. */
static void sort_records(srv_record_t *r, int n)
{
    for (int i = 1; i < n; i++) {
        srv_record_t k = r[i];
        int j = i - 1;
        while (j >= 0 &&
               (r[j].priority > k.priority ||
                (r[j].priority == k.priority && r[j].weight < k.weight))) {
            r[j + 1] = r[j];
            j--;
        }
        r[j + 1] = k;
    }
}

int srv_resolve(const char *domain, srv_record_t *out, int cap)
{
    if (cap <= 0) return 0;

    char qname[320];
    snprintf(qname, sizeof qname, "_svxreflector._tcp.%s", domain);

    unsigned char answer[4096];
    int len = res_query(qname, ns_c_in, ns_t_srv, answer, sizeof answer);
    if (len < 0) {
        log_info("srv: no SRV for %s — falling back to bare host", qname);
        return 0;
    }

    ns_msg msg;
    if (ns_initparse(answer, len, &msg) < 0)
        return 0;

    int total = ns_msg_count(msg, ns_s_an);
    int n = 0;
    for (int i = 0; i < total && n < cap; i++) {
        ns_rr rr;
        if (ns_parserr(&msg, ns_s_an, i, &rr) < 0)
            continue;
        if (ns_rr_type(rr) != ns_t_srv)
            continue;
        const unsigned char *rd = ns_rr_rdata(rr);
        if (ns_rr_rdlen(rr) < 7)
            continue;

        srv_record_t rec;
        rec.priority = ns_get16(rd);
        rec.weight   = ns_get16(rd + 2);
        rec.port     = ns_get16(rd + 4);

        char host[256];
        int hl = dn_expand(answer, answer + len, rd + 6,
                           host, (int)sizeof host);
        if (hl < 0 || host[0] == '\0')
            continue;
        snprintf(rec.host, sizeof rec.host, "%s", host);
        out[n++] = rec;
    }

    sort_records(out, n);
    if (n > 0)
        log_info("srv: %s -> %s:%u", qname, out[0].host, out[0].port);
    return n;
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

int srv_parse_hostlist(const char *hosts, uint16_t default_port,
                       srv_record_t *out, int cap)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", hosts);

    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok && n < cap;
         tok = strtok_r(NULL, ",", &save)) {
        char *entry = trim(tok);
        if (*entry == '\0') continue;

        srv_record_t rec;
        memset(&rec, 0, sizeof rec);
        rec.port     = default_port;
        rec.priority = (uint16_t)(100 + n);
        rec.weight   = 50;

        char *colon = strchr(entry, ':');
        if (colon) {
            *colon = '\0';
            int p = atoi(colon + 1);
            if (p > 0 && p <= 65535) rec.port = (uint16_t)p;
        }
        snprintf(rec.host, sizeof rec.host, "%s", entry);
        out[n++] = rec;
    }
    return n;
}
