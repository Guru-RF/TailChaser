/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "pki.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

/* uppercase copy of callsign into buf */
static void upper(const char *s, char *buf, size_t cap)
{
    size_t i = 0;
    for (; s[i] && i + 1 < cap; i++)
        buf[i] = (char)toupper((unsigned char)s[i]);
    buf[i] = '\0';
}

/* mkdir -p */
static int mkdir_p(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t n = strlen(tmp);
    if (n == 0) return -1;
    if (tmp[n - 1] == '/') tmp[n - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}

void pki_key_path(const char *state_dir, const char *callsign, char *buf, size_t cap)
{
    char cs[64]; upper(callsign, cs, sizeof cs);
    snprintf(buf, cap, "%s/%s.key", state_dir, cs);
}
void pki_cert_path(const char *state_dir, const char *callsign, char *buf, size_t cap)
{
    char cs[64]; upper(callsign, cs, sizeof cs);
    snprintf(buf, cap, "%s/%s.crt", state_dir, cs);
}
void pki_ca_path(const char *state_dir, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s/ca-bundle.crt", state_dir);
}
static void csr_path(const char *state_dir, const char *callsign, char *buf, size_t cap)
{
    char cs[64]; upper(callsign, cs, sizeof cs);
    snprintf(buf, cap, "%s/%s.csr", state_dir, cs);
}

static bool file_exists_nonempty(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

static int read_file(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
    return (int)n;
}

/* Written next to the file and renamed over it: a power cut leaves the old
 * file or the new one, never half of one. */
static int write_file(const char *path, const char *data, size_t len)
{
    char tmp[540];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { log_error("pki: cannot write %s: %s", tmp, strerror(errno)); return -1; }
    bool bad = fwrite(data, 1, len, f) != len;
    bad = fflush(f) != 0 || bad;
    bad = fsync(fileno(f)) != 0 || bad;
    bad = fclose(f) != 0 || bad;
    if (bad || rename(tmp, path) != 0) {
        log_error("pki: cannot write %s: %s", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* A daemon has nobody to ask for a passphrase: an encrypted key is unusable. */
static int no_passphrase(char *buf, int size, int rwflag, void *u)
{
    (void)buf; (void)size; (void)rwflag; (void)u;
    return -1;
}

static EVP_PKEY *load_key(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    EVP_PKEY *k = PEM_read_PrivateKey(f, NULL, no_passphrase, NULL);
    fclose(f);
    return k;
}

/* The first certificate in the file: the node's own, ahead of its chain. */
static X509 *load_leaf(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    X509 *x = PEM_read_X509(f, NULL, NULL, NULL);
    fclose(f);
    return x;
}

bool pki_have_cert(const char *state_dir, const char *callsign)
{
    char key[512], crt[512];
    pki_key_path(state_dir, callsign, key, sizeof key);
    pki_cert_path(state_dir, callsign, crt, sizeof crt);
    return file_exists_nonempty(key) && file_exists_nonempty(crt);
}

/* Delete the identity files (.key/.csr/.crt) so the next connect re-enrolls with
 * a fresh keypair + CSR (new CN/email SAN). Keeps ca-bundle.crt (re-fetched on
 * connect). Returns the count removed, or -1 for empty inputs. The CALLER must
 * have joined the reflector's net thread first (no enrollment racing the unlink). */
int pki_reset_cert(const char *state_dir, const char *callsign)
{
    if (!state_dir || !state_dir[0] || !callsign || !callsign[0]) return -1;
    char key[512], crt[512], csr[512];
    pki_key_path(state_dir, callsign, key, sizeof key);
    pki_cert_path(state_dir, callsign, crt, sizeof crt);
    snprintf(csr, sizeof csr, "%s", key);              /* .csr sits next to .key */
    char *dot = strrchr(csr, '.');
    if (dot) snprintf(dot, sizeof csr - (size_t)(dot - csr), ".csr");
    int removed = 0;
    const char *paths[3] = { key, csr, crt };
    for (int i = 0; i < 3; i++) {
        if (unlink(paths[i]) == 0) { log_info("pki: reset — removed %s", paths[i]); removed++; }
        else if (errno != ENOENT) log_warn("pki: reset — unlink %s: %s", paths[i], strerror(errno));
    }
    return removed;
}

bool pki_cert_usable(const char *state_dir, const char *callsign, char *note, size_t cap)
{
    char keyp[512], crtp[512];
    pki_key_path(state_dir, callsign, keyp, sizeof keyp);
    pki_cert_path(state_dir, callsign, crtp, sizeof crtp);
    EVP_PKEY *key = load_key(keyp);
    X509 *crt = load_leaf(crtp);
    bool ok = false;
    if (!key)
        snprintf(note, cap, "the private key %s cannot be read", keyp);
    else if (!crt)
        snprintf(note, cap, "%s holds no certificate", crtp);
    else if (X509_check_private_key(crt, key) != 1)
        snprintf(note, cap, "it does not belong to the private key");
    else {
        char when[64] = "?";
        BIO *b = BIO_new(BIO_s_mem());
        if (b) {
            if (ASN1_TIME_print(b, X509_get0_notAfter(crt)) == 1) {
                int n = BIO_read(b, when, (int)sizeof when - 1);
                when[n > 0 ? n : 0] = '\0';
            }
            BIO_free(b);
        }
        /* Only the end of its life is checked here. "Not valid yet" is left to
         * the reflector: a Pi that has not synchronised its clock yet must not
         * throw a good certificate away. */
        if (X509_cmp_current_time(X509_get0_notAfter(crt)) <= 0)
            snprintf(note, cap, "it expired on %s", when);
        else {
            snprintf(note, cap, "valid until %s", when);
            ok = true;
        }
    }
    EVP_PKEY_free(key);
    X509_free(crt);
    ERR_clear_error();
    return ok;
}

bool pki_cert_matches_key(const char *state_dir, const char *callsign, const char *pem)
{
    char keyp[512];
    pki_key_path(state_dir, callsign, keyp, sizeof keyp);
    EVP_PKEY *key = load_key(keyp);
    BIO *b = BIO_new_mem_buf(pem, -1);
    X509 *crt = b ? PEM_read_bio_X509(b, NULL, NULL, NULL) : NULL;
    bool ok = key && crt && X509_check_private_key(crt, key) == 1;
    BIO_free(b);
    X509_free(crt);
    EVP_PKEY_free(key);
    ERR_clear_error();
    return ok;
}

int pki_store_cert(const char *state_dir, const char *callsign, const char *pem)
{
    if (mkdir_p(state_dir) != 0) return -1;
    char crt[512];
    pki_cert_path(state_dir, callsign, crt, sizeof crt);
    if (write_file(crt, pem, strlen(pem)) != 0) return -1;
    log_info("pki: stored client certificate %s", crt);
    return 0;
}

int pki_store_ca(const char *state_dir, const char *pem)
{
    if (mkdir_p(state_dir) != 0) return -1;
    char ca[512];
    pki_ca_path(state_dir, ca, sizeof ca);
    /* It arrives at every connect: only touch the SD card when it changed. */
    static char have[64 * 1024];
    int n = read_file(ca, have, sizeof have);
    if (n >= 0 && (size_t)n == strlen(pem) && memcmp(have, pem, (size_t)n) == 0)
        return 0;
    if (n >= 0) {
        /* It comes in before the connection is protected, so a different one
         * is not believed: the one from the first connection stays. */
        char offered[540];
        snprintf(offered, sizeof offered, "%s.offered", ca);
        write_file(offered, pem, strlen(pem));
        log_warn("pki: the reflector offers another CA bundle than the one kept in %s — keeping ours. "
                 "If the reflector really has a new CA: mv %s %s", ca, offered, ca);
        return 0;
    }
    if (write_file(ca, pem, strlen(pem)) != 0) return -1;
    log_info("pki: stored CA bundle %s (%zu bytes)", ca, strlen(pem));
    return 0;
}

/* Generate RSA-2048 + write key PEM. Returns the EVP_PKEY (caller frees) or NULL. */
static EVP_PKEY *gen_rsa_key(const char *key_path)
{
    EVP_PKEY *pkey = EVP_RSA_gen(2048);
    if (!pkey) { log_error("pki: RSA keygen failed"); return NULL; }

    /* created 0600 whatever the umask: the private key is nobody else's business */
    int kfd = open(key_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    FILE *kf = kfd >= 0 ? fdopen(kfd, "w") : NULL;
    BIO *bio = kf ? BIO_new_fp(kf, BIO_CLOSE) : NULL;
    if (!bio) {
        log_error("pki: cannot open %s: %s", key_path, strerror(errno));
        if (kf) fclose(kf); else if (kfd >= 0) close(kfd);
        EVP_PKEY_free(pkey);
        return NULL;
    }
    int ok = PEM_write_bio_PrivateKey(bio, pkey, NULL, NULL, 0, NULL, NULL);
    BIO_free(bio);
    if (!ok) { log_error("pki: write key PEM failed"); EVP_PKEY_free(pkey); return NULL; }
    return pkey;
}

/* Build + sign a CSR with CN=callsign and (optional) email SAN; write PEM. */
static int gen_csr(EVP_PKEY *pkey, const char *callsign, const char *email,
                   const char *csr_path_str)
{
    int rc = -1;
    X509_REQ *req = X509_REQ_new();
    if (!req) return -1;

    X509_REQ_set_version(req, 0L);   /* v1 */

    X509_NAME *name = X509_REQ_get_subject_name(req);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char *)callsign, -1, -1, 0);

    if (X509_REQ_set_pubkey(req, pkey) != 1) goto done;

    /* Mirror SVXConnect's svx_generate_csr: always request a client-auth leaf
     * (basicConstraints/keyUsage/extendedKeyUsage), plus the email SAN when an
     * address is configured. A sysop CA that copies or validates CSR
     * extensions then issues a cert equivalent to the SVXConnect-enrolled one. */
    {
        X509V3_CTX v3ctx;
        X509V3_set_ctx_nodb(&v3ctx);
        X509V3_set_ctx(&v3ctx, NULL, NULL, req, NULL, 0);

        STACK_OF(X509_EXTENSION) *exts = sk_X509_EXTENSION_new_null();
        if (exts) {
            static const struct { int nid; const char *val; } base[] = {
                { NID_basic_constraints, "critical,CA:FALSE" },
                { NID_key_usage,         "critical,digitalSignature,keyEncipherment,keyAgreement" },
                { NID_ext_key_usage,     "clientAuth" },
            };
            for (size_t i = 0; i < sizeof base / sizeof base[0]; i++) {
                X509_EXTENSION *x = X509V3_EXT_conf_nid(NULL, &v3ctx, base[i].nid, base[i].val);
                if (x) sk_X509_EXTENSION_push(exts, x);
            }
            if (email && email[0]) {
                char san[384];
                snprintf(san, sizeof san, "email:%s", email);
                X509_EXTENSION *x = X509V3_EXT_conf_nid(NULL, &v3ctx, NID_subject_alt_name, san);
                if (x) sk_X509_EXTENSION_push(exts, x);
            }
            X509_REQ_add_extensions(req, exts);
            sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
        }
    }

    if (X509_REQ_sign(req, pkey, EVP_sha256()) == 0) { log_error("pki: CSR sign failed"); goto done; }

    BIO *bio = BIO_new_file(csr_path_str, "w");
    if (!bio) { log_error("pki: cannot open %s", csr_path_str); goto done; }
    int ok = PEM_write_bio_X509_REQ(bio, req);
    BIO_free(bio);
    if (!ok) { log_error("pki: write CSR PEM failed"); goto done; }
    rc = 0;

done:
    X509_REQ_free(req);
    return rc;
}

int pki_ensure_csr(const char *state_dir, const char *callsign, const char *email,
                   char *out_csr, size_t cap)
{
    if (mkdir_p(state_dir) != 0) return -1;

    char cs[64]; upper(callsign, cs, sizeof cs);
    char key[512], csr[512];
    pki_key_path(state_dir, callsign, key, sizeof key);
    csr_path(state_dir, callsign, csr, sizeof csr);

    /* Reuse an existing key+CSR (don't regenerate — the sysop may already be
     * signing the original). */
    if (file_exists_nonempty(key) && file_exists_nonempty(csr)) {
        if (read_file(csr, out_csr, cap) > 0) {
            log_info("pki: reusing existing CSR for %s", cs);
            return 0;
        }
    }

    /* A key without a request (the request file was lost, or never copied):
     * ask again with the SAME key — the reflector re-issues for a key it
     * knows, a new key would have to be approved by its sysop. */
    EVP_PKEY *pkey = file_exists_nonempty(key) ? load_key(key) : NULL;
    if (pkey) {
        log_info("pki: new signing request for %s with the existing key", cs);
    } else {
        log_info("pki: generating RSA-2048 key + CSR for %s", cs);
        pkey = gen_rsa_key(key);
    }
    if (!pkey) return -1;
    int rc = gen_csr(pkey, cs, email, csr);
    EVP_PKEY_free(pkey);
    if (rc != 0) return -1;

    if (read_file(csr, out_csr, cap) <= 0) { log_error("pki: cannot read back CSR"); return -1; }
    return 0;
}
