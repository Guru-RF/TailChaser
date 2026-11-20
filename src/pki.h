/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
/* TailChaser — client PKI for the SvxReflector cert-enrollment flow.
 *
 * Stores, under <state_dir>:
 *   <CALLSIGN>.key   RSA-2048 private key (PEM)
 *   <CALLSIGN>.csr   certificate signing request (PEM, CN=callsign, email SAN)
 *   <CALLSIGN>.crt   sysop-signed client certificate (PEM)
 *   ca-bundle.crt    reflector CA chain (PEM)
 *
 * Mirrors SVXConnect-OSX's libcrypto CSR path (svx_generate_csr). The C TLS
 * layer loads <CALLSIGN>.key + .crt + the CA chain directly into the SSL_CTX
 * (no PKCS#12 step — that was only needed for macOS SecIdentity).
 */
#ifndef SMARTSPOT_PKI_H
#define SMARTSPOT_PKI_H

#include <stddef.h>
#include <stdbool.h>

/* Ensure the key + CSR exist (generate RSA-2048 + CSR if missing) and copy the
 * CSR PEM into out (NUL-terminated). callsign is upper-cased for the filenames
 * and the CN. email may be empty. Returns 0 on success, -1 on failure. */
int pki_ensure_csr(const char *state_dir, const char *callsign, const char *email,
                   char *out_csr, size_t cap);

/* Persist the signed client cert / CA bundle PEM. Returns 0 / -1. */
int pki_store_cert(const char *state_dir, const char *callsign, const char *pem);
int pki_store_ca(const char *state_dir, const char *pem);

/* True if both <CALLSIGN>.key and <CALLSIGN>.crt exist. */
bool pki_have_cert(const char *state_dir, const char *callsign);

/* Can the stored certificate be presented? Not when it has expired, is
 * unreadable, or does not belong to the stored key. `note` gets the reason,
 * or "valid until <date>". */
bool pki_cert_usable(const char *state_dir, const char *callsign, char *note, size_t cap);

/* Does this certificate (PEM, the node's own first) belong to the stored key? */
bool pki_cert_matches_key(const char *state_dir, const char *callsign, const char *pem);

/* Delete the identity files (.key/.csr/.crt) so the next connect re-enrolls.
 * Keeps ca-bundle.crt. Returns count removed, -1 on empty inputs. */
int pki_reset_cert(const char *state_dir, const char *callsign);

/* Fill the standard PEM paths (callsign upper-cased). buf cap should be >= 512. */
void pki_key_path(const char *state_dir, const char *callsign, char *buf, size_t cap);
void pki_cert_path(const char *state_dir, const char *callsign, char *buf, size_t cap);
void pki_ca_path(const char *state_dir, char *buf, size_t cap);

#endif /* SMARTSPOT_PKI_H */
