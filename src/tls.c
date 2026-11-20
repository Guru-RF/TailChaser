/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "tls.h"
#include "pki.h"
#include "log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <openssl/x509v3.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

struct tls {
    SSL_CTX *ctx;
    SSL     *ssl;
    int      fd;
};

static void set_timeout(int fd, int ms)
{
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

/* Nobody to ask for a passphrase: never let OpenSSL prompt on a terminal. */
static int no_passphrase(char *buf, int size, int rwflag, void *u)
{
    (void)buf; (void)size; (void)rwflag; (void)u;
    return -1;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

tls_t *tls_client_start(int fd, const char *state_dir, const char *callsign,
                        const char *const *names, bool verify_host)
{
    tls_t *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fd = fd;

    t->ctx = SSL_CTX_new(TLS_client_method());
    if (!t->ctx) { log_error("tls: SSL_CTX_new failed"); goto fail; }

    SSL_CTX_set_min_proto_version(t->ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(t->ctx, TLS1_2_VERSION);
    /* The reflector has its own CA. Its bundle is kept from the first
     * connection on (pki_store_ca) and from then on the reflector has to show
     * a certificate that chains to it: nobody else can stand in for it. */
    char ca[512];
    pki_ca_path(state_dir, ca, sizeof ca);
    bool pinned = access(ca, R_OK) == 0 && SSL_CTX_load_verify_locations(t->ctx, ca, NULL) == 1;
    if (pinned) {
        SSL_CTX_set_verify(t->ctx, SSL_VERIFY_PEER, NULL);
    } else {
        ERR_clear_error();
        SSL_CTX_set_verify(t->ctx, SSL_VERIFY_NONE, NULL);
        log_warn("tls: no CA bundle of this reflector yet — its certificate is taken on trust this once");
    }

    /* Present the client identity if there is a signed certificate that can
     * still be used. One that has expired (the node was off the air for too
     * long to be sent a renewed one) is NOT presented: the reflector would
     * refuse the handshake, for ever. Without it the reflector asks for the
     * signing request instead, and issues a new certificate for the same key. */
    char note[256];
    if (!pki_have_cert(state_dir, callsign)) {
        log_info("tls: no client certificate yet (will enroll via CSR)");
    } else if (!pki_cert_usable(state_dir, callsign, note, sizeof note)) {
        log_warn("tls: the stored certificate is not used: %s — asking the reflector for a new one",
                 note);
    } else {
        char crt[512], key[512];
        pki_cert_path(state_dir, callsign, crt, sizeof crt);
        pki_key_path(state_dir, callsign, key, sizeof key);
        SSL_CTX_set_default_passwd_cb(t->ctx, no_passphrase);
        if (SSL_CTX_use_certificate_chain_file(t->ctx, crt) != 1 ||
            SSL_CTX_use_PrivateKey_file(t->ctx, key, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(t->ctx) != 1) {
            unsigned long e = ERR_peek_last_error();
            log_error("tls: OpenSSL refuses the client certificate %s: %s", crt,
                      e ? ERR_reason_error_string(e) : "?");
            ERR_clear_error();
            goto fail;
        }
        log_info("tls: client certificate loaded (%s)", note);
    }

    t->ssl = SSL_new(t->ctx);
    if (!t->ssl) { log_error("tls: SSL_new failed"); goto fail; }
    SSL_set_fd(t->ssl, fd);

    /* Generous handshake timeout; steady-state read timeout set afterwards. */
    set_timeout(fd, 10000);

    for (int attempt = 0; attempt < 20; attempt++) {
        int r = SSL_connect(t->ssl);
        if (r == 1) {
            /* Short read timeout: the connection loop wakes this often to send
             * what the main loop queued (talkgroup switches) and to notice a stop. */
            set_timeout(fd, 200);
            if (pinned && verify_host && names) {
                X509 *peer = SSL_get1_peer_certificate(t->ssl);
                bool ok = false;
                for (int i = 0; peer && names[i] && !ok; i++)
                    ok = names[i][0] && (X509_check_host(peer, names[i], 0, 0, NULL) == 1 ||
                                         X509_check_ip_asc(peer, names[i], 0) == 1);
                char subj[256] = "?";
                if (peer)
                    X509_NAME_oneline(X509_get_subject_name(peer), subj, sizeof subj);
                X509_free(peer);
                if (!ok) {
                    log_error("tls: the reflector's certificate (%s) is not made out to %s — not "
                              "connecting. If this is right, set [reflector] verify_host = false",
                              subj, names[0]);
                    goto fail;
                }
            }
            log_info("tls: connected (%s)%s", SSL_get_version(t->ssl),
                     pinned ? ", reflector certificate verified" : "");
            return t;
        }
        int e = SSL_get_error(t->ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
            continue;            /* timed out waiting; retry */
        long vr = SSL_get_verify_result(t->ssl);
        if (pinned && vr != X509_V_OK)
            log_error("tls: the reflector's certificate does not check against the CA bundle kept in %s "
                      "(%s). If the reflector has a new CA, remove that file", ca,
                      X509_verify_cert_error_string(vr));
        log_error("tls: handshake failed (ssl_err=%d)", e);
        goto fail;
    }
    log_error("tls: handshake timed out");

fail:
    tls_free(t);
    return NULL;
}

int tls_write(tls_t *t, const void *buf, int len)
{
    int off = 0;
    const unsigned char *p = buf;
    uint64_t until = now_ms() + 10000;       /* a peer that stopped reading is given up */
    while (off < len) {
        int n = SSL_write(t->ssl, p + off, len - off);
        if (n > 0) { off += n; continue; }
        if (now_ms() > until) {
            log_error("tls: the reflector is not reading — giving up this connection");
            return -1;
        }
        int e = SSL_get_error(t->ssl, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
            continue;
        if (e == SSL_ERROR_SYSCALL && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        log_error("tls: SSL_write error (ssl_err=%d)", e);
        return -1;
    }
    return len;
}

int tls_read(tls_t *t, void *buf, int len)
{
    int n = SSL_read(t->ssl, buf, len);
    if (n > 0) return n;
    int e = SSL_get_error(t->ssl, n);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
        return -2;   /* timeout / retry */
    if (e == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return -2;
    if (e == SSL_ERROR_ZERO_RETURN)
        return -1;   /* clean TLS close */
    return -1;
}

void tls_free(tls_t *t)
{
    if (!t) return;
    if (t->ssl) {
        /* Don't SSL_shutdown — SVXConnect notes close_notify confuses the
         * server; just drop the SSL object and let the caller close the fd. */
        SSL_free(t->ssl);
    }
    if (t->ctx) SSL_CTX_free(t->ctx);
    free(t);
}
