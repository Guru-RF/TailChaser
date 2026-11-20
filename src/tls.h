/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — STARTTLS over an already-connected socket (OpenSSL).
 *
 * Matches SVXConnect-OSX's SecureTransport setup: TLS 1.2 forced, the server
 * certificate is accepted without chain verification (the reflector uses its
 * own CA; SVXConnect uses .breakOnServerAuth then proceeds), and the client
 * certificate chain + key are presented if present (mutual TLS for cert auth).
 */
#ifndef SMARTSPOT_TLS_H
#define SMARTSPOT_TLS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct tls tls_t;

/* Upgrade a connected blocking socket to TLS. Loads <CALLSIGN>.crt (chain) +
 * <CALLSIGN>.key from state_dir as the client identity when they exist.
 * Returns a handle on success, NULL on failure. Does NOT take ownership of fd
 * (tls_free leaves the fd open; the caller closes it). */
/* `names` are the names the reflector may carry in its certificate (NULL-
 * terminated list; host names or addresses). With a CA bundle kept from an
 * earlier connection the certificate must chain to it; with verify_host it
 * must also be made out to one of the names. */
tls_t *tls_client_start(int fd, const char *state_dir, const char *callsign,
                        const char *const *names, bool verify_host);

/* Blocking write of the whole buffer. Returns len, or -1 on error. */
int  tls_write(tls_t *t, const void *buf, int len);

/* Read up to len bytes. Returns >0 bytes, -2 on timeout (retry), -1 on
 * close/error. The underlying socket carries a receive timeout so this
 * returns -2 periodically and the caller can re-check its run flag. */
int  tls_read(tls_t *t, void *buf, int len);

void tls_free(tls_t *t);

#endif /* SMARTSPOT_TLS_H */
