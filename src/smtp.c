#include "smtp.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/ssl.h"
#include "sdkconfig.h"
#ifdef CONFIG_MBEDTLS_SSL_PROTO_TLS1_3
#include "psa/crypto.h"
#endif

#include "app_config.h"

static const char *TAG = "smtp";

/* One connection. Heap (the mbedtls contexts are a few KB), zeroed on free. */
typedef struct {
    mbedtls_net_context net;
    bool tls;          /* reads and writes go through ssl */
    bool tls_init;     /* the mbedtls contexts below need freeing */
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    char rbuf[512];    /* received, not yet parsed */
    size_t rlen;
    char line[256];    /* last reply line, for the error detail */
    bool ehlo;         /* the reply being read is the EHLO list */
    bool ext_starttls, auth_plain, auth_login;
    bool bol;          /* message data: at the start of a line (dot-stuffing) */
} conn_t;

bool smtp_err_permanent(smtp_err_t e)
{
    return e == SMTP_ERR_DNS || e == SMTP_ERR_TLS || e == SMTP_ERR_NO_TLS || e == SMTP_ERR_AUTH ||
           e == SMTP_ERR_NO_AUTH || e == SMTP_ERR_REJECTED;
}

const char *smtp_err_name(smtp_err_t e)
{
    static const char *k[] = {"ok", "dns", "connect", "tls", "no_tls", "auth", "no_auth", "rejected", "temp", "io",
                              "nomem"};
    return (unsigned)e < sizeof(k) / sizeof(k[0]) ? k[e] : "?";
}

static bool conn_write(conn_t *c, const char *d, size_t n)
{
    while (n) {
        int r;
        if (c->tls) {
            r = mbedtls_ssl_write(&c->ssl, (const unsigned char *)d, n);
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
        } else {
            r = mbedtls_net_send(&c->net, (const unsigned char *)d, n);
        }
        if (r <= 0) {
            ESP_LOGW(TAG, "write failed (-0x%04x)", (unsigned)-r);
            return false;
        }
        d += r;
        n -= (size_t)r;
    }
    return true;
}

/* More bytes into rbuf. False on timeout, error or close. */
static bool conn_fill(conn_t *c)
{
    const size_t room = sizeof(c->rbuf) - c->rlen;
    int r;
    do {
        r = c->tls ? mbedtls_ssl_read(&c->ssl, (unsigned char *)c->rbuf + c->rlen, room)
                   : mbedtls_net_recv(&c->net, (unsigned char *)c->rbuf + c->rlen, room);
    } while (c->tls && (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE));
    if (r <= 0) {
        ESP_LOGW(TAG, "read failed (%s-0x%04x)", r == 0 ? "closed, " : "", (unsigned)-r);
        return false;
    }
    c->rlen += (size_t)r;
    return true;
}

/* One reply line into c->line, without the CRLF. A line longer than rbuf
 * is cut (only the code and the first words matter). */
static bool read_line(conn_t *c)
{
    while (1) {
        char *nl = memchr(c->rbuf, '\n', c->rlen);
        if (nl || c->rlen == sizeof(c->rbuf)) {
            size_t n = nl ? (size_t)(nl - c->rbuf) : c->rlen;
            size_t keep = n < sizeof(c->line) - 1 ? n : sizeof(c->line) - 1;
            memcpy(c->line, c->rbuf, keep);
            c->line[keep] = '\0';
            if (keep && c->line[keep - 1] == '\r') {
                c->line[keep - 1] = '\0';
            }
            size_t used = nl ? n + 1 : n;
            memmove(c->rbuf, c->rbuf + used, c->rlen - used);
            c->rlen -= used;
            return true;
        }
        if (!conn_fill(c)) {
            return false;
        }
    }
}

/* EHLO list entries ("STARTTLS", "AUTH LOGIN PLAIN XOAUTH2"). */
static void parse_ext(conn_t *c, const char *ext)
{
    if (strcasecmp(ext, "STARTTLS") == 0) {
        c->ext_starttls = true;
    } else if (strncasecmp(ext, "AUTH", 4) == 0 && (ext[4] == ' ' || ext[4] == '=')) {
        char mech[64];
        const char *p = ext + 5;
        while (*p) {
            size_t n = strcspn(p, " ");
            if (n && n < sizeof(mech)) {
                memcpy(mech, p, n);
                mech[n] = '\0';
                c->auth_plain |= strcasecmp(mech, "PLAIN") == 0;
                c->auth_login |= strcasecmp(mech, "LOGIN") == 0;
            }
            p += n;
            p += strspn(p, " ");
        }
    }
}

/* A whole (possibly multi-line) reply. Returns the code, -1 on I/O failure. */
static int read_reply(conn_t *c)
{
    while (1) {
        if (!read_line(c)) {
            return -1;
        }
        if (strlen(c->line) < 3 || !isdigit((unsigned char)c->line[0]) || !isdigit((unsigned char)c->line[1]) ||
            !isdigit((unsigned char)c->line[2])) {
            ESP_LOGW(TAG, "odd reply line \"%.40s\"", c->line);
            return -1;
        }
        const int code = atoi(c->line);
        if (c->ehlo && c->line[3]) {
            parse_ext(c, c->line + 4);
        }
        if (c->line[3] != '-') {
            ESP_LOGD(TAG, "< %.80s", c->line);
            return code;
        }
    }
}

/* Send one command line (CRLF added). secret: do not log it. */
static int command(conn_t *c, bool secret, const char *fmt, ...)
{
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof(buf) - 2) {
        return -1;
    }
    if (!secret) {
        ESP_LOGD(TAG, "> %s", buf);
    }
    buf[n++] = '\r';
    buf[n++] = '\n';
    bool ok = conn_write(c, buf, (size_t)n);
    mbedtls_platform_zeroize(buf, sizeof(buf));
    return ok ? read_reply(c) : -1;
}

/* The same mbedtls setup esp-tls uses (esp_tls_mbedtls.c): CTR-DRBG seeded
 * from the hardware entropy, the certificate bundle, hostname check. esp-tls
 * itself has no STARTTLS, hence mbedtls directly on the TCP socket. */
static smtp_err_t tls_start(conn_t *c, const char *host, char *detail, size_t dl)
{
#ifdef CONFIG_MBEDTLS_SSL_PROTO_TLS1_3
    if (psa_crypto_init() != PSA_SUCCESS) {
        snprintf(detail, dl, "PSA crypto init failed");
        return SMTP_ERR_TLS;
    }
#endif
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_ctr_drbg_init(&c->drbg);
    mbedtls_entropy_init(&c->ent);
    c->tls_init = true;
    int r = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->ent, (const unsigned char *)"kidphone-smtp", 13);
    if (r == 0) {
        r = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
    }
    if (r != 0) {
        snprintf(detail, dl, "TLS setup failed (-0x%04x)", (unsigned)-r);
        return r == MBEDTLS_ERR_SSL_ALLOC_FAILED ? SMTP_ERR_NOMEM : SMTP_ERR_TLS;
    }
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    if (esp_crt_bundle_attach(&c->conf) != ESP_OK) {
        snprintf(detail, dl, "certificate bundle missing");
        return SMTP_ERR_TLS;
    }
    r = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (r == 0) {
        r = mbedtls_ssl_set_hostname(&c->ssl, host);
    }
    if (r != 0) {
        snprintf(detail, dl, "TLS setup failed (-0x%04x)", (unsigned)-r);
        return r == MBEDTLS_ERR_SSL_ALLOC_FAILED ? SMTP_ERR_NOMEM : SMTP_ERR_TLS;
    }
    mbedtls_ssl_set_bio(&c->ssl, &c->net, mbedtls_net_send, mbedtls_net_recv, NULL);
    while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        const uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
        if (flags && flags != (uint32_t)-1) {
            snprintf(detail, dl, "the server's certificate is not trusted (flags 0x%lx)", (unsigned long)flags);
            return SMTP_ERR_TLS;
        }
        snprintf(detail, dl, "TLS handshake failed (-0x%04x)", (unsigned)-r);
        /* A dropped connection or timeout mid-handshake is worth a retry. */
        return r == MBEDTLS_ERR_NET_RECV_FAILED || r == MBEDTLS_ERR_NET_SEND_FAILED ||
                       r == MBEDTLS_ERR_NET_CONN_RESET || r == MBEDTLS_ERR_SSL_CONN_EOF
                   ? SMTP_ERR_IO
                   : r == MBEDTLS_ERR_SSL_ALLOC_FAILED ? SMTP_ERR_NOMEM : SMTP_ERR_TLS;
    }
    c->tls = true;
    ESP_LOGI(TAG, "TLS up (%s, %s)", mbedtls_ssl_get_version(&c->ssl), mbedtls_ssl_get_ciphersuite(&c->ssl));
    return SMTP_OK;
}

static void conn_close(conn_t *c)
{
    if (c->tls) {
        mbedtls_ssl_close_notify(&c->ssl);
    }
    if (c->tls_init) {
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_ctr_drbg_free(&c->drbg);
        mbedtls_entropy_free(&c->ent);
    }
    if (c->net.fd >= 0) {
        close(c->net.fd);
    }
    mbedtls_platform_zeroize(c, sizeof(*c));
    free(c);
}

/* A reply code that is not the wanted one: 4xx = temporary, else permanent
 * (with the server's words as the detail). */
static smtp_err_t bad_reply(conn_t *c, int code, const char *step, char *detail, size_t dl)
{
    if (code < 0) {
        snprintf(detail, dl, "connection lost at %s", step);
        return SMTP_ERR_IO;
    }
    snprintf(detail, dl, "%s: %.180s", step, c->line);
    return code >= 400 && code < 500 ? SMTP_ERR_TEMP : SMTP_ERR_REJECTED;
}

static bool data_sink(void *ctx, const char *d, size_t n)
{
    conn_t *c = ctx;
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (c->bol && d[i] == '.') {
            /* Dot-stuffing (RFC 5321 4.5.2): a line starting with "." gets
             * one more, so it cannot end the message early. */
            if (!conn_write(c, d + start, i - start) || !conn_write(c, ".", 1)) {
                return false;
            }
            start = i;
        }
        c->bol = d[i] == '\n';
    }
    return conn_write(c, d + start, n - start);
}

static smtp_err_t do_auth(conn_t *c, const smtp_account_t *a, char *detail, size_t dl)
{
    const size_t ul = strlen(a->user), pl = strlen(a->pass);
    /* PLAIN: base64("\0user\0pass"). Both fit: user < 96, pass < 72. */
    uint8_t raw[192];
    char b64[260];
    int code;
    if (c->auth_plain || !c->auth_login) {
        if (ul + pl + 2 > sizeof(raw)) {
            return SMTP_ERR_AUTH;
        }
        raw[0] = 0;
        memcpy(raw + 1, a->user, ul);
        raw[1 + ul] = 0;
        memcpy(raw + 2 + ul, a->pass, pl);
        mime_b64(raw, ul + pl + 2, b64);
        code = command(c, true, "AUTH PLAIN %s", b64);
    } else {
        code = command(c, false, "AUTH LOGIN");
        if (code == 334) {
            mime_b64((const uint8_t *)a->user, ul, b64);
            code = command(c, true, "%s", b64);
        }
        if (code == 334) {
            mime_b64((const uint8_t *)a->pass, pl, b64);
            code = command(c, true, "%s", b64);
        }
    }
    mbedtls_platform_zeroize(raw, sizeof(raw));
    mbedtls_platform_zeroize(b64, sizeof(b64));
    if (code == 235) {
        return SMTP_OK;
    }
    if (code < 0) {
        snprintf(detail, dl, "connection lost at login");
        return SMTP_ERR_IO;
    }
    snprintf(detail, dl, "%.200s", c->line);
    /* 454 "temporary authentication failure" is worth a retry; 535 and
     * friends mean the user or password is wrong. */
    return code >= 400 && code < 500 ? SMTP_ERR_TEMP : SMTP_ERR_AUTH;
}

static smtp_err_t ehlo(conn_t *c, const smtp_account_t *a, char *detail, size_t dl)
{
    c->ehlo = true;
    c->ext_starttls = c->auth_plain = c->auth_login = false;
    int code = command(c, false, "EHLO %s", a->helo);
    c->ehlo = false;
    return code == 250 ? SMTP_OK : bad_reply(c, code, "EHLO", detail, dl);
}

static smtp_err_t session(conn_t *c, const smtp_account_t *a, const mime_msg_t *m, char *detail, size_t dl)
{
    smtp_err_t e;
    if (a->security == SMTP_TLS && (e = tls_start(c, a->host, detail, dl)) != SMTP_OK) {
        return e;
    }
    int code = read_reply(c);
    if (code != 220) {
        return bad_reply(c, code, "greeting", detail, dl);
    }
    if ((e = ehlo(c, a, detail, dl)) != SMTP_OK) {
        return e;
    }
    if (a->security == SMTP_STARTTLS) {
        if (!c->ext_starttls) {
            snprintf(detail, dl, "the server does not offer STARTTLS on this port");
            return SMTP_ERR_NO_TLS;
        }
        code = command(c, false, "STARTTLS");
        if (code != 220) {
            return bad_reply(c, code, "STARTTLS", detail, dl);
        }
        c->rlen = 0; /* nothing sent before the handshake may be trusted (RFC 3207) */
        if ((e = tls_start(c, a->host, detail, dl)) != SMTP_OK || (e = ehlo(c, a, detail, dl)) != SMTP_OK) {
            return e;
        }
    }
    if (!c->auth_plain && !c->auth_login) {
        snprintf(detail, dl, "the server does not offer a password login");
        return SMTP_ERR_NO_AUTH;
    }
    if ((e = do_auth(c, a, detail, dl)) != SMTP_OK) {
        return e;
    }
    code = command(c, false, "MAIL FROM:<%s>", m->from_addr);
    if (code != 250) {
        return bad_reply(c, code, "sender", detail, dl);
    }
    code = command(c, false, "RCPT TO:<%s>", m->to_addr);
    if (code != 250 && code != 251) {
        return bad_reply(c, code, "recipient", detail, dl);
    }
    code = command(c, false, "DATA");
    if (code != 354) {
        return bad_reply(c, code, "DATA", detail, dl);
    }
    c->bol = true;
    if (!mime_write(m, data_sink, c)) {
        snprintf(detail, dl, "connection lost while sending the message");
        return SMTP_ERR_IO;
    }
    if (!(c->bol ? true : conn_write(c, "\r\n", 2)) || !conn_write(c, ".\r\n", 3)) {
        snprintf(detail, dl, "connection lost while sending the message");
        return SMTP_ERR_IO;
    }
    code = read_reply(c);
    if (code != 250) {
        return bad_reply(c, code, "message", detail, dl);
    }
    command(c, false, "QUIT"); /* the message is accepted: the answer does not matter */
    return SMTP_OK;
}

smtp_err_t smtp_send(const smtp_account_t *a, const mime_msg_t *m, char *detail, size_t detail_len)
{
    detail[0] = '\0';
    conn_t *c = calloc(1, sizeof(conn_t));
    if (!c) {
        snprintf(detail, detail_len, "out of memory");
        return SMTP_ERR_NOMEM;
    }
    c->net.fd = -1;
    const esp_tls_cfg_t cfg = {.timeout_ms = APP_SECRET_SMTP_TIMEOUT_MS};
    esp_tls_last_error_t last = {0};
    esp_err_t err = esp_tls_plain_tcp_connect(a->host, (int)strlen(a->host), a->port, &cfg, &last, &c->net.fd);
    if (err != ESP_OK) {
        c->net.fd = -1;
        conn_close(c);
        if (err == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) {
            snprintf(detail, detail_len, "server name %.64s not found", a->host);
            return SMTP_ERR_DNS;
        }
        snprintf(detail, detail_len, "could not connect to %.64s port %u", a->host, a->port);
        return SMTP_ERR_CONNECT;
    }
    ESP_LOGI(TAG, "connected to %s:%u (%s)", a->host, a->port, a->security == SMTP_TLS ? "TLS" : "STARTTLS");
    smtp_err_t e = session(c, a, m, detail, detail_len);
    conn_close(c);
    if (e != SMTP_OK) {
        ESP_LOGW(TAG, "send failed (%s): %s", smtp_err_name(e), detail);
    }
    return e;
}
