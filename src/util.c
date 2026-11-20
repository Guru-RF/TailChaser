/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "util.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

int util_primary_ipv4(char *out, size_t cap)
{
    if (!out || cap == 0)
        return -1;
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0)
        return -1;

    int rc = -1;
    for (struct ifaddrs *p = list; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
            continue;
        if ((p->ifa_flags & IFF_LOOPBACK) || !(p->ifa_flags & IFF_UP))
            continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)(void *)p->ifa_addr;
        uint32_t a = ntohl(sin->sin_addr.s_addr);
        if ((a >> 16) == 0xA9FE)         /* skip 169.254/16 link-local */
            continue;
        if (inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)cap)) {
            rc = 0;
            break;                       /* first global IPv4 wins */
        }
    }
    freeifaddrs(list);
    return rc;
}

void util_state_path(const char *state_dir, const char *name, char *out, size_t cap)
{
    char dir[256];
    snprintf(dir, sizeof dir, "%s",
             (state_dir && state_dir[0]) ? state_dir : "/var/lib/tailchaser");
    size_t len = strlen(dir);
    while (len > 1 && dir[len - 1] == '/')      /* strip trailing slashes */
        dir[--len] = '\0';
    char *slash = strrchr(dir, '/');            /* drop the last component (pki) */
    if (slash && slash != dir)
        *slash = '\0';
    snprintf(out, cap, "%s/%s", dir, name);
}

/* Wall clock, shifted by TAILCHASER_FAKE_EPOCH when that is set: the tests
 * start the daemon a few seconds before the hour to see the identification. */
time_t util_wall_time(void)
{
    static int    init;
    static time_t offset;
    if (!init) {
        const char *e = getenv("TAILCHASER_FAKE_EPOCH");
        if (e && *e)
            offset = (time_t)strtoll(e, NULL, 10) - time(NULL);
        init = 1;
    }
    return time(NULL) + offset;
}

uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t mono_ms(void)
{
    return mono_ns() / 1000000ull;
}

int ct_memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a, *pb = b;
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= (unsigned char)(pa[i] ^ pb[i]);
    return diff != 0;
}

void wr_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

void wr_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xff);
}

uint16_t rd_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* Each slot is [uint32 len][payload bytes (max_payload)]. */
struct frame_ring {
    size_t              mask;        /* slots - 1 (slots is power of two) */
    size_t              slot_stride; /* sizeof(uint32) + max_payload      */
    size_t              max_payload;
    unsigned char      *buf;
    _Atomic size_t      head;        /* next write slot (producer)        */
    _Atomic size_t      tail;        /* next read slot  (consumer)        */
};

static size_t next_pow2(size_t v)
{
    size_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

frame_ring_t *frame_ring_new(size_t slots, size_t max_payload)
{
    if (slots < 2)
        slots = 2;
    slots = next_pow2(slots);

    frame_ring_t *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;

    r->mask        = slots - 1;
    r->max_payload = max_payload;
    r->slot_stride = sizeof(uint32_t) + max_payload;
    r->buf         = malloc(slots * r->slot_stride);
    if (!r->buf) {
        free(r);
        return NULL;
    }
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    return r;
}

void frame_ring_free(frame_ring_t *r)
{
    if (!r)
        return;
    free(r->buf);
    free(r);
}

int frame_ring_push(frame_ring_t *r, const void *data, size_t len)
{
    if (len > r->max_payload)
        return -2;

    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (head - tail > r->mask)   /* full */
        return -1;

    unsigned char *slot = r->buf + (head & r->mask) * r->slot_stride;
    uint32_t l = (uint32_t)len;
    memcpy(slot, &l, sizeof l);
    memcpy(slot + sizeof l, data, len);

    atomic_store_explicit(&r->head, head + 1, memory_order_release);
    return 0;
}

ssize_t frame_ring_pop(frame_ring_t *r, void *out, size_t out_cap)
{
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (tail == head)   /* empty */
        return -1;

    unsigned char *slot = r->buf + (tail & r->mask) * r->slot_stride;
    uint32_t l;
    memcpy(&l, slot, sizeof l);
    if (l > out_cap)
        return -2;

    memcpy(out, slot + sizeof l, l);
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    return (ssize_t)l;
}

size_t frame_ring_count(const frame_ring_t *r)
{
    size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    return head - tail;
}
