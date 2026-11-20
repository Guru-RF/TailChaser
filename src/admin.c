/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "admin.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "admin_page.h"                 /* ADMIN_PAGE[], made from admin.html */

#define USER            "admin"
#define DEFAULT_PASS    "admin"
#define PBKDF2_ITER     100000
#define MAX_KEYS        16
#define MAX_SESSIONS    8
#define SESSION_IDLE_S  (12 * 3600)
#define MAX_CONF        (256 * 1024)

typedef struct {
    bool     used;
    unsigned id;
    bool     control;                   /* may block; otherwise read only */
    time_t   created;
    uint8_t  sha[SHA256_DIGEST_LENGTH];
    char     label[40];
} api_key_t;

typedef struct {
    char   token[65];
    time_t last;
} session_t;

struct admin {
    char   state_file[300], conf_path[300], role[16], exe[300];
    bool   pass_set;                    /* false: still "admin", has to be changed */
    uint8_t salt[16], hash[32];
    api_key_t keys[MAX_KEYS];
    unsigned  next_id;
    session_t sessions[MAX_SESSIONS];
    time_t    fail_until;
    int       fails;
    atomic_bool block_rx, block_tx, restart;
};

/* ---- small things ---- */

static void hex(const uint8_t *in, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[in[i] >> 4];
        out[2 * i + 1] = d[in[i] & 15];
    }
    out[2 * n] = '\0';
}

static bool unhex(const char *in, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (!isxdigit((unsigned char)in[2 * i]) || !isxdigit((unsigned char)in[2 * i + 1]) ||
            sscanf(in + 2 * i, "%2x", &v) != 1)
            return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

static void derive(const char *pass, const uint8_t *salt, uint8_t *out)
{
    PKCS5_PBKDF2_HMAC(pass, (int)strlen(pass), salt, 16, PBKDF2_ITER, EVP_sha256(), 32, out);
}

/* Addresses that are not the internet: the password "admin" is good for
 * those only. */
static bool private_peer(const char *ip)
{
    unsigned a, b;
    if (sscanf(ip, "%u.%u.", &a, &b) != 2)
        return false;
    return a == 127 || a == 10 || (a == 192 && b == 168) || (a == 172 && b >= 16 && b <= 31) ||
           (a == 100 && b >= 64 && b <= 127) || (a == 169 && b == 254);
}

/* One field of an application/x-www-form-urlencoded string. */
static bool form_get(const char *s, size_t len, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    const char *p = s, *end = s + len;
    while (p < end) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        const char *stop = amp ? amp : end;
        if ((size_t)(stop - p) > nl && !memcmp(p, name, nl) && p[nl] == '=') {
            size_t o = 0;
            for (const char *q = p + nl + 1; q < stop && o + 1 < cap; q++) {
                unsigned v;
                if (*q == '+') {
                    out[o++] = ' ';
                } else if (*q == '%' && q + 2 < stop &&
                           isxdigit((unsigned char)q[1]) && isxdigit((unsigned char)q[2]) &&
                           sscanf(q + 1, "%2x", &v) == 1) {
                    out[o++] = (char)v;
                    q += 2;
                } else {
                    out[o++] = *q;
                }
            }
            out[o] = '\0';
            return true;
        }
        if (!amp)
            break;
        p = amp + 1;
    }
    out[0] = '\0';
    return false;
}

/* "rx" / "tx" from a form, a query string or a little JSON: 1, true, on. */
static int flag_of(const http_req_t *in, const char *name)
{
    char v[16];
    if (form_get(in->body, in->body_len, name, v, sizeof v) ||
        form_get(in->query, strlen(in->query), name, v, sizeof v))
        return !strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "on") ? 1 : 0;
    char pat[24];
    snprintf(pat, sizeof pat, "\"%s\"", name);
    const char *p = in->body_len ? strstr(in->body, pat) : NULL;
    if (!p)
        return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    return *p == 't' || *p == '1' ? 1 : *p == 'f' || *p == '0' ? 0 : -1;
}

static void jesc(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 7 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c < 0x20) o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = '\0';
}

static void reply(http_resp_t *out, const char *status, const char *type, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    out->buf = malloc((size_t)n + 1);
    if (out->buf)
        vsnprintf(out->buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    out->status = status;
    out->type = type;
    out->body = out->buf ? out->buf : "";
    out->len = out->buf ? (size_t)n : 0;
}

#define JSON "application/json"
static void fail(http_resp_t *out, const char *status, const char *why)
{
    char e[400];
    jesc(why, e, sizeof e);
    reply(out, status, JSON, "{\"ok\":false,\"error\":\"%s\"}\n", e);
}

/* ---- what is kept ---- */

static void save(admin_t *a)
{
    char tmp[320];
    snprintf(tmp, sizeof tmp, "%s.new", a->state_file);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) {
        if (fd >= 0) close(fd);
        log_error("admin: cannot write %s: %s — what was changed is lost at the next start",
                  tmp, strerror(errno));
        return;
    }
    fprintf(f, "# TailChaser admin: password, API keys, blocks. Written by the program.\n");
    if (a->pass_set) {
        char s[33], h[65];
        hex(a->salt, 16, s);
        hex(a->hash, 32, h);
        fprintf(f, "pass=pbkdf2-sha256$%d$%s$%s\n", PBKDF2_ITER, s, h);
    }
    fprintf(f, "block_rx=%d\nblock_tx=%d\n", (int)atomic_load(&a->block_rx), (int)atomic_load(&a->block_tx));
    for (int i = 0; i < MAX_KEYS; i++) {
        const api_key_t *k = &a->keys[i];
        if (!k->used)
            continue;
        char h[65];
        hex(k->sha, sizeof k->sha, h);
        fprintf(f, "key=%u|%s|%lld|%s|%s\n", k->id, k->control ? "control" : "read",
                (long long)k->created, h, k->label);
    }
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, a->state_file) != 0)
        log_error("admin: cannot keep %s: %s", a->state_file, strerror(errno));
}

static void load(admin_t *a)
{
    FILE *f = fopen(a->state_file, "re");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "pass=pbkdf2-sha256$", 19)) {
            int it;
            char s[40], h[80];
            if (sscanf(line + 19, "%d$%32[0-9a-f]$%64[0-9a-f]", &it, s, h) == 3 && it == PBKDF2_ITER &&
                strlen(s) == 32 && strlen(h) == 64 && unhex(s, a->salt, 16) && unhex(h, a->hash, 32))
                a->pass_set = true;
        } else if (!strncmp(line, "block_rx=", 9)) {
            atomic_store(&a->block_rx, line[9] == '1');
        } else if (!strncmp(line, "block_tx=", 9)) {
            atomic_store(&a->block_tx, line[9] == '1');
        } else if (!strncmp(line, "key=", 4)) {
            unsigned id;
            char scope[12], h[80];
            long long created;
            int used = 0;
            if (sscanf(line + 4, "%u|%11[a-z]|%lld|%64[0-9a-f]|%n", &id, scope, &created, h, &used) >= 4 &&
                strlen(h) == 64) {
                for (int i = 0; i < MAX_KEYS; i++) {
                    api_key_t *k = &a->keys[i];
                    if (k->used)
                        continue;
                    if (!unhex(h, k->sha, sizeof k->sha))
                        break;
                    k->used = true;
                    k->id = id;
                    k->control = !strcmp(scope, "control");
                    k->created = (time_t)created;
                    snprintf(k->label, sizeof k->label, "%s", used ? line + 4 + used : "");
                    if (id >= a->next_id) a->next_id = id + 1;
                    break;
                }
            }
        }
    }
    fclose(f);
}

admin_t *admin_new(const char *state_file, const char *conf_path, const char *role, const char *exe)
{
    admin_t *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    snprintf(a->state_file, sizeof a->state_file, "%s", state_file);
    snprintf(a->conf_path, sizeof a->conf_path, "%s", conf_path);
    snprintf(a->role, sizeof a->role, "%s", role);
    snprintf(a->exe, sizeof a->exe, "%s", exe);
    a->next_id = 1;
    load(a);
    bool rx = atomic_load(&a->block_rx), tx = atomic_load(&a->block_tx);
    log_info("admin: portal on /admin%s%s", a->pass_set ? "" : " — the password is still \"admin\": log in and change it",
             rx && tx ? "; receiver and transmitter are BLOCKED" : rx ? "; the receiver is BLOCKED"
             : tx ? "; the transmitter is BLOCKED" : "");
    return a;
}

void admin_free(admin_t *a) { free(a); }

void admin_blocks(const admin_t *a, bool *rx, bool *tx)
{
    *rx = atomic_load(&a->block_rx);
    *tx = atomic_load(&a->block_tx);
}

bool admin_restart_wanted(const admin_t *a) { return atomic_load(&a->restart); }

/* ---- who is asking ---- */

static session_t *session_of(admin_t *a, const http_req_t *in)
{
    const char *p = strstr(in->cookie, "tc_admin=");
    if (!p)
        return NULL;
    char tok[65];
    size_t n = strspn(p + 9, "0123456789abcdef");
    if (n != 64)
        return NULL;
    memcpy(tok, p + 9, 64);
    tok[64] = '\0';
    time_t now = time(NULL);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        session_t *s = &a->sessions[i];
        if (s->token[0] && CRYPTO_memcmp(s->token, tok, 64) == 0) {
            if (now - s->last > SESSION_IDLE_S) {
                s->token[0] = '\0';
                return NULL;
            }
            s->last = now;
            return s;
        }
    }
    return NULL;
}

static const api_key_t *key_of(const admin_t *a, const http_req_t *in)
{
    const char *k = in->api_key;
    if (!k[0] && !strncasecmp(in->authorization, "Bearer ", 7))
        k = in->authorization + 7;
    while (*k == ' ') k++;
    size_t n = strcspn(k, " \r\n");
    if (n < 16 || n > 128)
        return NULL;
    uint8_t sha[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)k, n, sha);
    const api_key_t *found = NULL;
    for (int i = 0; i < MAX_KEYS; i++)
        if (a->keys[i].used && CRYPTO_memcmp(a->keys[i].sha, sha, sizeof sha) == 0)
            found = &a->keys[i];
    return found;
}

static bool pass_ok(const admin_t *a, const char *pass)
{
    if (!a->pass_set)
        return CRYPTO_memcmp(pass, DEFAULT_PASS, sizeof DEFAULT_PASS) == 0 && strlen(pass) == sizeof DEFAULT_PASS - 1;
    uint8_t h[32];
    derive(pass, a->salt, h);
    return CRYPTO_memcmp(h, a->hash, 32) == 0;
}

/* ---- the configuration ---- */

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "re");
    if (!f)
        return NULL;
    char *buf = malloc(MAX_CONF + 1);
    size_t n = buf ? fread(buf, 1, MAX_CONF, f) : 0;
    fclose(f);
    if (buf) buf[n] = '\0';
    *len = n;
    return buf;
}

static bool write_whole(const char *path, const char *data, size_t len, int flags, mode_t mode)
{
    int fd = open(path, flags | O_WRONLY | O_CLOEXEC, mode);
    if (fd < 0)
        return false;
    bool ok = true;
    for (size_t off = 0; off < len && ok;) {
        ssize_t w = write(fd, data + off, len - off);
        if (w > 0) off += (size_t)w;
        else if (errno != EINTR) ok = false;
    }
    ok = fsync(fd) == 0 && ok;
    return close(fd) == 0 && ok;
}

/* Let the program itself say whether it would start with this file. */
static bool check_conf(const admin_t *a, const char *path, char *said, size_t cap)
{
    int pfd[2];
    said[0] = '\0';
    if (pipe2(pfd, O_CLOEXEC) != 0)
        return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return false;
    }
    if (pid == 0) {
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        execl(a->exe, a->exe, "-t", "-c", path, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    size_t got = 0;
    for (;;) {
        ssize_t r = read(pfd[0], said + got, cap - 1 - got);
        if (r > 0) got += (size_t)r;
        else if (r == 0 || errno != EINTR) break;
        if (got >= cap - 1) break;
    }
    said[got] = '\0';
    close(pfd[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static void conf_save(admin_t *a, const http_req_t *in, http_resp_t *out)
{
    char *conf = malloc(MAX_CONF + 1);
    if (!conf || !form_get(in->body, in->body_len, "conf", conf, MAX_CONF + 1) || strlen(conf) < 20) {
        free(conf);
        fail(out, "400 Bad Request", "no configuration in the request");
        return;
    }
    char cand[340], bak[340], said[3000];
    snprintf(cand, sizeof cand, "%s.candidate.conf", a->state_file);
    snprintf(bak, sizeof bak, "%s.previous.conf", a->state_file);
    if (!write_whole(cand, conf, strlen(conf), O_CREAT | O_TRUNC, 0600)) {
        free(conf);
        fail(out, "500 Internal Server Error", "cannot write next to the state file");
        return;
    }
    bool ok = check_conf(a, cand, said, sizeof said);
    char esc[6100];
    jesc(said, esc, sizeof esc);
    if (!ok) {
        unlink(cand);
        free(conf);
        reply(out, "200 OK", JSON, "{\"ok\":false,\"error\":\"the program refuses this configuration — nothing was saved\","
              "\"said\":\"%s\"}\n", esc);
        return;
    }
    size_t oldn = 0;
    char *old = read_file(a->conf_path, &oldn);
    if (old)
        write_whole(bak, old, oldn, O_CREAT | O_TRUNC, 0600);
    free(old);
    /* in place: the directory is not ours to make files in */
    if (!write_whole(a->conf_path, conf, strlen(conf), O_TRUNC, 0)) {
        char why[400];
        snprintf(why, sizeof why, "cannot write %s (%s) — the service has to be allowed to: "
                 "run bootstrap.sh again, it sets that up", a->conf_path, strerror(errno));
        fail(out, "200 OK", why);
    } else {
        log_info("admin: configuration saved from %s (the one before is in %s) — takes effect at the next restart",
                 in->peer, bak);
        reply(out, "200 OK", JSON, "{\"ok\":true,\"said\":\"%s\"}\n", esc);
    }
    unlink(cand);
    free(conf);
}

/* ---- the answers ---- */

static void blocks_json(const admin_t *a, char *out, size_t cap)
{
    snprintf(out, cap, "{\"rx\":%s,\"tx\":%s}", atomic_load(&a->block_rx) ? "true" : "false",
             atomic_load(&a->block_tx) ? "true" : "false");
}

static void set_blocks(admin_t *a, const http_req_t *in, const char *who)
{
    int rx = flag_of(in, "rx"), tx = flag_of(in, "tx");
    bool was_rx = atomic_load(&a->block_rx), was_tx = atomic_load(&a->block_tx);
    if (rx >= 0) atomic_store(&a->block_rx, rx == 1);
    if (tx >= 0) atomic_store(&a->block_tx, tx == 1);
    bool now_rx = atomic_load(&a->block_rx), now_tx = atomic_load(&a->block_tx);
    if (now_rx != was_rx)
        log_warn("admin: receiver %s by %s (%s)", now_rx ? "BLOCKED" : "released", who, in->peer);
    if (now_tx != was_tx)
        log_warn("admin: transmitter %s by %s (%s)", now_tx ? "BLOCKED" : "released", who, in->peer);
    if (now_rx != was_rx || now_tx != was_tx)
        save(a);
}

static void api(admin_t *a, const http_req_t *in, http_resp_t *out,
                char *(*status_json)(void *, size_t *), void *ctx)
{
    const api_key_t *k = key_of(a, in);
    bool get = !strcmp(in->method, "GET"), post = !strcmp(in->method, "POST");
    if (!k) {
        snprintf(out->header, sizeof out->header, "WWW-Authenticate: Bearer\r\n");
        fail(out, "401 Unauthorized", "an API key is needed: Authorization: Bearer <key>, or X-API-Key: <key>");
    } else if (!strcmp(in->path, "/api/v1/status") && get) {
        size_t n = 0;
        out->buf = status_json(ctx, &n);
        out->status = "200 OK";
        out->type = JSON;
        out->body = out->buf ? out->buf : "{}";
        out->len = out->buf ? n : 2;
    } else if (!strcmp(in->path, "/api/v1/block") && get) {
        char b[48];
        blocks_json(a, b, sizeof b);
        reply(out, "200 OK", JSON, "%s\n", b);
    } else if (!strcmp(in->path, "/api/v1/block") && post) {
        if (!k->control) {
            fail(out, "403 Forbidden", "this key may only read");
            return;
        }
        char who[64], b[48];
        snprintf(who, sizeof who, "API key \"%s\"", k->label);
        set_blocks(a, in, who);
        blocks_json(a, b, sizeof b);
        reply(out, "200 OK", JSON, "%s\n", b);
    } else {
        fail(out, "404 Not Found", "there is /api/v1/status and /api/v1/block");
    }
}

static void login(admin_t *a, const http_req_t *in, http_resp_t *out)
{
    time_t now = time(NULL);
    if (now < a->fail_until) {
        fail(out, "429 Too Many Requests", "too many wrong passwords — wait a little");
        return;
    }
    char user[32], pass[128];
    form_get(in->body, in->body_len, "user", user, sizeof user);
    form_get(in->body, in->body_len, "pass", pass, sizeof pass);
    bool ok = !strcmp(user, USER) && pass_ok(a, pass);
    if (ok && !a->pass_set && !private_peer(in->peer)) {
        log_warn("admin: login with the first password from %s refused — only from a private address", in->peer);
        fail(out, "403 Forbidden", "the first password works from the local network only — "
             "log in from there and change it");
        return;
    }
    if (!ok) {
        a->fails++;
        int wait = a->fails < 3 ? 1 : a->fails < 6 ? 5 : 30;
        a->fail_until = now + wait;
        log_warn("admin: wrong password from %s", in->peer);
        fail(out, "401 Unauthorized", "wrong name or password");
        return;
    }
    a->fails = 0;
    session_t *s = &a->sessions[0];
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!a->sessions[i].token[0] || now - a->sessions[i].last > SESSION_IDLE_S) { s = &a->sessions[i]; break; }
        if (a->sessions[i].last < s->last) s = &a->sessions[i];
    }
    uint8_t raw[32];
    if (RAND_bytes(raw, sizeof raw) != 1) {
        fail(out, "500 Internal Server Error", "no random numbers");
        return;
    }
    hex(raw, sizeof raw, s->token);
    s->last = now;
    snprintf(out->header, sizeof out->header,
             "Set-Cookie: tc_admin=%s; Path=/admin; HttpOnly; SameSite=Strict; Max-Age=%d\r\n",
             s->token, SESSION_IDLE_S);
    log_info("admin: login from %s", in->peer);
    reply(out, "200 OK", JSON, "{\"ok\":true,\"must_change\":%s}\n", a->pass_set ? "false" : "true");
}

static void portal(admin_t *a, const http_req_t *in, http_resp_t *out)
{
    const char *p = in->path + 6;                       /* after "/admin" */
    bool get = !strcmp(in->method, "GET"), post = !strcmp(in->method, "POST");
    if (get && (!*p || !strcmp(p, "/"))) {
        out->status = "200 OK";
        out->type = "text/html; charset=utf-8";
        out->body = (const char *)ADMIN_PAGE;
        out->len = sizeof ADMIN_PAGE;
        return;
    }
    if (strncmp(p, "/api/", 5) != 0 || (!get && !post)) {
        fail(out, "404 Not Found", "not found");
        return;
    }
    p += 5;
    /* A page from somewhere else cannot set this header: no request forgery. */
    if (post && strcmp(in->marker, "1") != 0) {
        fail(out, "403 Forbidden", "not from the admin page");
        return;
    }
    if (post && !strcmp(p, "login")) {
        login(a, in, out);
        return;
    }
    session_t *s = session_of(a, in);
    if (get && !strcmp(p, "session")) {
        char b[48];
        blocks_json(a, b, sizeof b);
        reply(out, "200 OK", JSON, "{\"auth\":%s,\"must_change\":%s,\"role\":\"%s\",\"conf\":\"%s\",\"block\":%s}\n",
              s ? "true" : "false", a->pass_set ? "false" : "true", a->role, s ? a->conf_path : "", b);
        return;
    }
    if (!s) {
        fail(out, "401 Unauthorized", "log in first");
        return;
    }
    if (post && !strcmp(p, "logout")) {
        s->token[0] = '\0';
        snprintf(out->header, sizeof out->header, "Set-Cookie: tc_admin=; Path=/admin; Max-Age=0\r\n");
        reply(out, "200 OK", JSON, "{\"ok\":true}\n");
        return;
    }
    if (post && !strcmp(p, "password")) {
        char old[128], neu[128];
        form_get(in->body, in->body_len, "old", old, sizeof old);
        form_get(in->body, in->body_len, "new", neu, sizeof neu);
        if (!pass_ok(a, old)) {
            fail(out, "200 OK", "the present password is not right");
        } else if (strlen(neu) < 8 || !strcmp(neu, DEFAULT_PASS) || !strcmp(neu, old)) {
            fail(out, "200 OK", "the new password needs 8 characters or more, and to be a new one");
        } else if (RAND_bytes(a->salt, sizeof a->salt) != 1) {
            fail(out, "500 Internal Server Error", "no random numbers");
        } else {
            derive(neu, a->salt, a->hash);
            a->pass_set = true;
            for (int i = 0; i < MAX_SESSIONS; i++)      /* everybody else logs in again */
                if (&a->sessions[i] != s) a->sessions[i].token[0] = '\0';
            save(a);
            log_info("admin: password changed from %s", in->peer);
            reply(out, "200 OK", JSON, "{\"ok\":true}\n");
        }
        return;
    }
    /* With the first password nothing else is open. */
    if (!a->pass_set) {
        fail(out, "403 Forbidden", "change the password first");
        return;
    }
    if (get && !strcmp(p, "config")) {
        size_t n = 0;
        out->buf = read_file(a->conf_path, &n);
        if (!out->buf) {
            fail(out, "500 Internal Server Error", "cannot read the configuration");
            return;
        }
        out->status = "200 OK";
        out->type = "text/plain; charset=utf-8";
        out->body = out->buf;
        out->len = n;
    } else if (post && !strcmp(p, "config")) {
        conf_save(a, in, out);
    } else if (post && !strcmp(p, "restart")) {
        atomic_store(&a->restart, true);
        log_info("admin: restart asked for from %s — at the next quiet moment", in->peer);
        reply(out, "200 OK", JSON, "{\"ok\":true}\n");
    } else if (post && !strcmp(p, "block")) {
        char b[48];
        set_blocks(a, in, "the admin");
        blocks_json(a, b, sizeof b);
        reply(out, "200 OK", JSON, "{\"ok\":true,\"block\":%s}\n", b);
    } else if (get && !strcmp(p, "keys")) {
        char *buf = malloc(MAX_KEYS * 200 + 32);
        if (!buf) {
            fail(out, "500 Internal Server Error", "out of memory");
            return;
        }
        size_t o = (size_t)sprintf(buf, "{\"keys\":[");
        bool first = true;
        for (int i = 0; i < MAX_KEYS; i++) {
            const api_key_t *k = &a->keys[i];
            if (!k->used)
                continue;
            char l[100];
            jesc(k->label, l, sizeof l);
            o += (size_t)sprintf(buf + o, "%s{\"id\":%u,\"label\":\"%s\",\"scope\":\"%s\",\"created\":%lld}",
                                 first ? "" : ",", k->id, l, k->control ? "control" : "read", (long long)k->created);
            first = false;
        }
        o += (size_t)sprintf(buf + o, "]}\n");
        out->buf = buf;
        out->status = "200 OK";
        out->type = JSON;
        out->body = buf;
        out->len = o;
    } else if (post && !strcmp(p, "keys")) {
        char label[40], scope[12];
        form_get(in->body, in->body_len, "label", label, sizeof label);
        form_get(in->body, in->body_len, "scope", scope, sizeof scope);
        for (char *c = label; *c; c++)
            if ((unsigned char)*c < 0x20 || *c == '|') *c = ' ';
        api_key_t *k = NULL;
        for (int i = 0; i < MAX_KEYS && !k; i++)
            if (!a->keys[i].used) k = &a->keys[i];
        uint8_t raw[24];
        if (!label[0]) {
            fail(out, "200 OK", "give the key a name: what will use it");
        } else if (!k) {
            fail(out, "200 OK", "that is the most keys there can be — delete one first");
        } else if (RAND_bytes(raw, sizeof raw) != 1) {
            fail(out, "500 Internal Server Error", "no random numbers");
        } else {
            char key[64] = "tc_";
            hex(raw, sizeof raw, key + 3);
            SHA256((const unsigned char *)key, strlen(key), k->sha);
            k->used = true;
            k->id = a->next_id++;
            k->control = !strcmp(scope, "control");
            k->created = time(NULL);
            snprintf(k->label, sizeof k->label, "%s", label);
            save(a);
            log_info("admin: API key \"%s\" (%s) made from %s", k->label, k->control ? "control" : "read", in->peer);
            reply(out, "200 OK", JSON, "{\"ok\":true,\"id\":%u,\"key\":\"%s\"}\n", k->id, key);
        }
    } else if (post && !strcmp(p, "keys/delete")) {
        char id[16];
        form_get(in->body, in->body_len, "id", id, sizeof id);
        bool done = false;
        for (int i = 0; i < MAX_KEYS; i++)
            if (a->keys[i].used && a->keys[i].id == (unsigned)strtoul(id, NULL, 10)) {
                log_info("admin: API key \"%s\" deleted from %s", a->keys[i].label, in->peer);
                memset(&a->keys[i], 0, sizeof a->keys[i]);
                done = true;
            }
        if (done) save(a);
        reply(out, "200 OK", JSON, "{\"ok\":%s}\n", done ? "true" : "false");
    } else {
        fail(out, "404 Not Found", "not found");
    }
}

bool admin_handle(admin_t *a, const http_req_t *in, http_resp_t *out,
                  char *(*status_json)(void *, size_t *), void *ctx)
{
    if (!strncmp(in->path, "/api/", 5)) {
        api(a, in, out, status_json, ctx);
        return true;
    }
    if (!strcmp(in->path, "/admin") || !strncmp(in->path, "/admin/", 7)) {
        portal(a, in, out);
        return true;
    }
    return false;
}
