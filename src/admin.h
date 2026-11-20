/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — the admin portal and the remote API.
 *
 *   /admin              a page: log in, edit the configuration, restart the
 *                       node, block the receiver or the transmitter, make API
 *                       keys. One user, "admin"; the password is "admin" until
 *                       the first login, which has to change it.
 *   /admin/api/...      what that page talks to (session cookie)
 *   /api/v1/status      the status, for other programs        (API key)
 *   /api/v1/block       GET: what is blocked; POST rx=0|1 tx=0|1 (control key)
 *
 * What is kept — the password (PBKDF2), the keys (SHA-256, never the key
 * itself) and the two blocks — is in one small file in the state directory,
 * so a block set from far away is still there after a restart.
 *
 * Everything here runs on the status server's thread; the main loop only
 * reads the two blocks and the restart request (atomics).
 */
#ifndef TAILCHASER_ADMIN_H
#define TAILCHASER_ADMIN_H

#include <stdbool.h>
#include <stddef.h>

typedef struct admin admin_t;

typedef struct {
    const char *method, *path, *query;      /* query: after the '?', or "" */
    const char *cookie, *authorization, *api_key, *marker;  /* headers, or "" */
    const char *body;
    size_t      body_len;
    const char *peer;                       /* dotted address of the client */
} http_req_t;

typedef struct {
    const char *status;                     /* "200 OK" */
    const char *type;
    const char *body;                       /* static, or `buf` */
    size_t      len;
    char        header[256];                /* extra header lines, each with \r\n */
    char       *buf;                        /* malloc'd; the server frees it */
} http_resp_t;

/* state_file: where password, keys and blocks are kept. conf_path: the
 * configuration this program was started with. exe: the program itself, to
 * check a new configuration with (-t). */
admin_t *admin_new(const char *state_file, const char *conf_path, const char *role, const char *exe);
void     admin_free(admin_t *a);

/* True when the request was for the portal or the API; `out` is filled in.
 * status_json builds what /status.json shows into a malloc'd string. */
bool admin_handle(admin_t *a, const http_req_t *in, http_resp_t *out,
                  char *(*status_json)(void *ctx, size_t *len), void *ctx);

/* For the main loop. */
void admin_blocks(const admin_t *a, bool *rx, bool *tx);
bool admin_restart_wanted(const admin_t *a);

#endif /* TAILCHASER_ADMIN_H */
