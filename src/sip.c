#include "sip.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/md5.h"

#include "app_config.h"
#include "config_store.h"
#include "rtp.h"
#include "wifi.h"

static const char *TAG = "sip";

#define RX_BUF_BYTES    3072
#define MAX_HDRS        48
#define MAX_ROUTES      4
#define URI_MAX         256
#define ALLOW_METHODS   "INVITE, ACK, CANCEL, BYE, OPTIONS"

/* ---------------------------------------------------------------------------
 * Small string helpers
 * ------------------------------------------------------------------------- */

typedef struct {
    char *b;
    size_t cap;
    size_t len;
    bool overflow;
} sb_t;

static void sb_init(sb_t *s, char *buf, size_t cap)
{
    s->b = buf;
    s->cap = cap;
    s->len = 0;
    s->overflow = false;
    buf[0] = '\0';
}

static void sb_printf(sb_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(sb_t *s, const char *fmt, ...)
{
    if (s->overflow) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->b + s->len, s->cap - s->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= s->cap - s->len) {
        s->overflow = true;
        s->b[s->len] = '\0';
        return;
    }
    s->len += (size_t)n;
}

/* Bounded copy of n bytes (or up to NUL) into out, always terminated. */
static void copy_n(char *out, size_t out_len, const char *src, size_t n)
{
    if (out_len == 0) {
        return;
    }
    size_t i = 0;
    for (; i < n && i + 1 < out_len && src[i]; i++) {
        out[i] = src[i];
    }
    out[i] = '\0';
}

static void rand_hex(char *out, size_t nbytes)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < nbytes; i++) {
        uint8_t v = (uint8_t)esp_random();
        out[2 * i] = hx[v >> 4];
        out[2 * i + 1] = hx[v & 15];
    }
    out[2 * nbytes] = '\0';
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ---------------------------------------------------------------------------
 * Logging: first line always at INFO, full message at INFO or DEBUG.
 * Authorization values are redacted, the destination number is masked.
 * ------------------------------------------------------------------------- */

/* SIP account and destination of the current call, copied from
 * config_store at call start (provisioning may change them at runtime). */
static char s_domain[96];
static char s_user[33];
static char s_pass[65];
static char s_dest[20];

static void mask_line(char *line)
{
    const char *d = s_dest;
    if (d[0] == '+') {
        d++;
    }
    size_t dl = strlen(d);
    if (dl <= 4) {
        return;
    }
    char *p = line;
    while ((p = strstr(p, d)) != NULL) {
        memset(p, '*', dl - 4);
        p += dl;
    }
}

static void redact_line(char *line)
{
    static const char *k_secret_hdrs[] = {"Authorization:", "Proxy-Authorization:"};
    for (size_t i = 0; i < sizeof(k_secret_hdrs) / sizeof(k_secret_hdrs[0]); i++) {
        size_t n = strlen(k_secret_hdrs[i]);
        if (strncasecmp(line, k_secret_hdrs[i], n) == 0) {
            strcpy(line + n, " <redacted>");
            return;
        }
    }
}

static void log_msg(const char *dir, const char *buf, size_t len, bool dump, const char *note)
{
    char line[320];
    size_t pos = 0;
    bool first = true;
    while (pos < len) {
        size_t e = pos;
        while (e < len && buf[e] != '\n') {
            e++;
        }
        size_t l = e - pos;
        if (l > 0 && buf[pos + l - 1] == '\r') {
            l--;
        }
        copy_n(line, sizeof(line), buf + pos, l);
        if (l >= sizeof(line)) {
            strcpy(line + sizeof(line) - 4, "...");
        }
        redact_line(line);
        mask_line(line);
        if (first) {
            ESP_LOGI(TAG, "SIP %s %s%s", dir, line, note ? note : "");
            first = false;
            if (!dump) {
                return;
            }
        } else if (APP_SIP_LOG_FULL) {
            ESP_LOGI(TAG, "    %s", line);
        } else {
            ESP_LOGD(TAG, "    %s", line);
        }
        pos = e + 1;
    }
}

/* ---------------------------------------------------------------------------
 * Message parsing (destructive, in place)
 * ------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *value;
} hdr_t;

typedef struct {
    bool is_req;
    const char *method; /* request */
    const char *ruri;
    int code;           /* response */
    const char *reason;
    hdr_t h[MAX_HDRS];
    int nh;
    const char *body;
    size_t body_len;
} msg_t;

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) {
        *--e = '\0';
    }
    return s;
}

/* Find header #idx by full name or single-letter compact form (0 for none). */
static const char *hdr_get(const msg_t *m, const char *name, char compact, int idx)
{
    for (int i = 0; i < m->nh; i++) {
        const char *n = m->h[i].name;
        bool match = strcasecmp(n, name) == 0 ||
                     (compact && n[0] && !n[1] && tolower((unsigned char)n[0]) == compact);
        if (match && idx-- == 0) {
            return m->h[i].value;
        }
    }
    return NULL;
}

static bool parse_msg(char *buf, size_t len, msg_t *m)
{
    memset(m, 0, sizeof(*m));
    buf[len] = '\0';

    char *hend = strstr(buf, "\r\n\r\n");
    size_t sep = 4;
    if (!hend) {
        hend = strstr(buf, "\n\n");
        sep = 2;
    }
    char *body = NULL;
    if (hend) {
        *hend = '\0';
        body = hend + sep;
    }

    char *save = NULL;
    char *line = strtok_r(buf, "\n", &save);
    if (!line) {
        return false;
    }
    line = trim(line);
    if (strncmp(line, "SIP/2.0 ", 8) == 0) {
        m->is_req = false;
        m->code = atoi(line + 8);
        char *r = strchr(line + 8, ' ');
        m->reason = r ? r + 1 : "";
        if (m->code < 100 || m->code > 699) {
            return false;
        }
    } else {
        m->is_req = true;
        char *sp = strchr(line, ' ');
        if (!sp) {
            return false;
        }
        *sp = '\0';
        m->method = line;
        m->ruri = sp + 1;
        char *sp2 = strchr(sp + 1, ' ');
        if (sp2) {
            *sp2 = '\0';
        }
    }

    while ((line = strtok_r(NULL, "\n", &save)) != NULL && m->nh < MAX_HDRS) {
        if (line[0] == ' ' || line[0] == '\t') {
            continue; /* folded continuation line: not supported, skipped */
        }
        char *colon = strchr(line, ':');
        if (!colon) {
            continue;
        }
        *colon = '\0';
        m->h[m->nh].name = trim(line);
        m->h[m->nh].value = trim(colon + 1);
        m->nh++;
    }

    if (body) {
        size_t avail = len - (size_t)(body - buf);
        const char *cl = hdr_get(m, "Content-Length", 'l', 0);
        size_t want = cl ? (size_t)atoi(cl) : avail;
        m->body = body;
        m->body_len = want < avail ? want : avail;
    }
    return true;
}

/* Length of the first element of a comma-joined header value. */
static size_t elem_len(const char *v)
{
    size_t n = 0;
    bool in_q = false, in_a = false;
    while (v[n]) {
        char c = v[n];
        if (c == '"') {
            in_q = !in_q;
        } else if (!in_q && c == '<') {
            in_a = true;
        } else if (!in_q && c == '>') {
            in_a = false;
        } else if (!in_q && !in_a && c == ',') {
            break;
        }
        n++;
    }
    return n;
}

/* First element of a possibly comma-joined header value (e.g. the top Via). */
static void first_elem(const char *v, char *out, size_t out_len)
{
    copy_n(out, out_len, v, elem_len(v));
}

/*
 * ;name=value parameter of a header (outside any <...>). Returns true if the
 * parameter is present (out gets "" for a flag parameter like ;lr).
 */
static bool param_get(const char *v, const char *name, char *out, size_t out_len)
{
    const char *gt = strchr(v, '>');
    const char *p = gt ? gt : v;
    size_t nl = strlen(name);
    while ((p = strchr(p, ';')) != NULL) {
        p++;
        while (*p == ' ') {
            p++;
        }
        if (strncasecmp(p, name, nl) == 0 && (p[nl] == '=' || p[nl] == ';' || p[nl] == '\0' ||
                                              p[nl] == ',' || p[nl] == ' ')) {
            size_t n = 0;
            if (p[nl] == '=') {
                const char *s = p + nl + 1;
                if (*s == '"') {
                    s++;
                    while (s[n] && s[n] != '"') {
                        n++;
                    }
                } else {
                    while (s[n] && s[n] != ';' && s[n] != ',' && s[n] != ' ' && s[n] != '>') {
                        n++;
                    }
                }
                copy_n(out, out_len, s, n);
            } else if (out_len) {
                out[0] = '\0';
            }
            return true;
        }
    }
    return false;
}

/* URI of a name-addr ("<sip:...>;params") or addr-spec ("sip:...;params"). */
static void uri_of(const char *v, char *out, size_t out_len)
{
    const char *lt = strchr(v, '<');
    if (lt) {
        const char *gt = strchr(lt, '>');
        copy_n(out, out_len, lt + 1, gt ? (size_t)(gt - lt - 1) : strlen(lt + 1));
        return;
    }
    size_t n = 0;
    while (v[n] && v[n] != ';' && v[n] != ' ' && v[n] != ',') {
        n++;
    }
    copy_n(out, out_len, v, n);
}

/* key="value" or key=token inside a Digest challenge. */
static bool auth_param(const char *v, const char *key, char *out, size_t out_len)
{
    const char *p = v;
    if (strncasecmp(p, "Digest", 6) == 0) {
        p += 6;
    }
    size_t kl = strlen(key);
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t') {
            p++;
        }
        const char *k = p;
        while (*p && *p != '=' && *p != ',') {
            p++;
        }
        size_t this_kl = (size_t)(p - k);
        while (this_kl && k[this_kl - 1] == ' ') {
            this_kl--;
        }
        if (*p != '=') {
            continue;
        }
        p++;
        while (*p == ' ') {
            p++;
        }
        const char *val;
        size_t vl = 0;
        if (*p == '"') {
            val = ++p;
            while (p[vl] && p[vl] != '"') {
                vl++;
            }
            p += vl;
            if (*p == '"') {
                p++;
            }
        } else {
            val = p;
            while (p[vl] && p[vl] != ',' && p[vl] != ' ') {
                vl++;
            }
            p += vl;
        }
        if (this_kl == kl && strncasecmp(k, key, kl) == 0) {
            copy_n(out, out_len, val, vl);
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

/* CMD_START + n = call contact n (0..CFG_MAX_CONTACTS-1). */
typedef enum { CMD_HANGUP = 0, CMD_START = 1 } cmd_t;

typedef struct {
    bool valid;
    bool proxy;          /* 407 (Proxy-Authorization) vs 401 (Authorization) */
    char realm[96];
    char nonce[160];
    char opaque[160];
    bool qop_auth;
    uint32_t nc;
} auth_t;

typedef struct {
    bool active;
    char call_id[48];
    char local_tag[12];
    char from_hdr[160];       /* <sip:user@domain>;tag=local_tag */
    char to_hdr[160];         /* <sip:number@domain>, no tag */
    char ruri[128];
    auth_t auth;
    int auth_tries;
    int bye_auth_tries;
    bool hangup_pending;      /* user hung up: CANCEL or BYE as soon as possible */
    char sdp_sess[16];
    int64_t call_start_ms;

    /* INVITE client transaction */
    uint32_t cseq;
    char inv_branch[40];
    char inv_msg[2048];
    int inv_len;
    int64_t inv_start_ms, inv_next_ms;
    uint32_t inv_interval;
    bool inv_provisional;
    int inv_final;            /* 0 until a final response */
    char ack_msg[1024];       /* last ACK, resent on a retransmitted final */
    int ack_len;

    /* dialog */
    char dlg_to_hdr[256];     /* To of the 200 OK, with the remote tag */
    char remote_target[URI_MAX];
    char route[MAX_ROUTES][URI_MAX];
    int n_route;
    uint32_t local_cseq;
    int64_t keepalive_ms;
    int64_t ending_deadline_ms;

    /* non-INVITE client transaction (BYE or CANCEL) */
    bool nit_active;
    char nit_method[8];
    char nit_branch[40];
    uint32_t nit_cseq;
    char nit_msg[1536];
    int nit_len;
    int64_t nit_start_ms, nit_next_ms;
    uint32_t nit_interval;

    /* outcome, recorded by end_call() */
    bool failed;              /* never connected: error final, Timer B */
    int fail_code;            /* SIP status of the failing final, 0 if none */
    bool remote_bye;
    int64_t answer_ms;        /* 0 until answered */
} call_t;

static call_t C;
static QueueHandle_t s_cmd_q;
static _Atomic int s_state = CALL_IDLE;
static int s_sock = -1;
static struct sockaddr_in s_server;
static char s_local_ip[16];
static char s_pub_ip[16];     /* NAT public address learned from Via received= */
static uint16_t s_pub_port;   /* and rport= */
static char s_rx[RX_BUF_BYTES + 1];

/* Result of the last call, read by main (busy tone) and the setup page. */
static portMUX_TYPE s_res_mux = portMUX_INITIALIZER_UNLOCKED;
static sip_result_t s_result;
static _Atomic int64_t s_busy_until_ms;

static _Atomic int s_contact = -1;

/* Map where and how a call failed to a parent-facing explanation. A 403 is
 * only a login problem if it answers an INVITE that carried our credentials:
 * Twilio rejects a wrong password with 403 after digest auth (seen on
 * 2026-10-10 when two credential lists held the same username). Geo
 * permission and trial-account blocks happen behind the Function, which has
 * already answered the SIP leg, so they show up as a very short call. */
static void classify(sip_result_t *r, bool auth_sent)
{
    const char *kind = "", *msg = "";
    if (r->failed) {
        switch (r->code) {
        case 0:
            if (strstr(r->reason, "not set up")) {
                kind = "setup";
                msg = "The phone is not connected to Twilio yet. Finish setup first.";
            } else {
                kind = "network";
                msg = "The phone could not reach Twilio. Check the WiFi and the internet connection.";
            }
            break;
        case 401:
        case 407:
            kind = "login";
            msg = "Twilio rejected the phone's login. Run setup again.";
            break;
        case 403:
            if (auth_sent) {
                kind = "login";
                msg = "Twilio rejected the phone's login. Run setup again.";
            } else {
                kind = "other";
                msg = "Twilio refused the call.";
            }
            break;
        case 404:
            kind = "domain";
            msg = "Twilio did not find the phone's setup (SIP domain). Run setup again.";
            break;
        case 480:
        case 486:
        case 600:
        case 603:
            kind = "busy";
            msg = "No answer or busy.";
            break;
        default:
            kind = "other";
            msg = "The call did not go through.";
            break;
        }
    } else if (r->answered && r->remote_hangup && r->talk_ms < APP_SHORT_CALL_MS) {
        kind = "unreachable";
        msg = "Twilio could not reach this number. Check Geo permissions for that country, and on a trial "
              "account only verified numbers can be called.";
    }
    strlcpy(r->kind, kind, sizeof(r->kind));
    strlcpy(r->message, msg, sizeof(r->message));
}

static void record_result(bool failed, int code, bool answered, bool remote, uint32_t talk_ms, const char *why,
                          bool auth_sent)
{
    sip_result_t r = {0};
    r.failed = failed;
    r.code = code;
    r.answered = answered;
    r.remote_hangup = remote;
    r.talk_ms = talk_ms;
    r.contact = atomic_load(&s_contact);
    strlcpy(r.reason, why, sizeof(r.reason));
    classify(&r, auth_sent);
    if (r.message[0]) {
        ESP_LOGW(TAG, "call result: %s (%s%s%d)", r.message, why, code ? ", SIP " : ", code ", code);
    }
    portENTER_CRITICAL(&s_res_mux);
    r.seq = s_result.seq + 1;
    s_result = r;
    portEXIT_CRITICAL(&s_res_mux);
    if (failed || strcmp(r.kind, "unreachable") == 0) {
        atomic_store(&s_busy_until_ms, now_ms() + APP_BUSY_TONE_MS);
    }
}

int sip_call_contact_index(void)
{
    return sip_call_state() == CALL_IDLE ? -1 : atomic_load(&s_contact);
}

void sip_last_result(sip_result_t *out)
{
    portENTER_CRITICAL(&s_res_mux);
    *out = s_result;
    portEXIT_CRITICAL(&s_res_mux);
}

bool sip_busy_tone_active(void)
{
    return now_ms() < atomic_load(&s_busy_until_ms);
}

const char *sip_call_state_name(call_state_t s)
{
    switch (s) {
    case CALL_IDLE: return "IDLE";
    case CALL_CALLING: return "CALLING";
    case CALL_RINGING: return "RINGING";
    case CALL_IN_CALL: return "IN_CALL";
    case CALL_ENDING: return "ENDING";
    default: return "?";
    }
}

call_state_t sip_call_state(void)
{
    return (call_state_t)atomic_load(&s_state);
}

static void set_state(call_state_t s, const char *why)
{
    call_state_t old = sip_call_state();
    if (old == s) {
        return;
    }
    atomic_store(&s_state, s);
    ESP_LOGI(TAG, "CALL STATE %s -> %s (%s)", sip_call_state_name(old), sip_call_state_name(s), why);
}

void sip_call_contact(int idx)
{
    if (idx < 0 || idx >= CFG_MAX_CONTACTS) {
        idx = 0;
    }
    cmd_t c = (cmd_t)(CMD_START + idx);
    if (s_cmd_q) {
        xQueueSend(s_cmd_q, &c, 0);
    }
}

void sip_call_hangup(void)
{
    cmd_t c = CMD_HANGUP;
    if (s_cmd_q) {
        xQueueSend(s_cmd_q, &c, 0);
    }
}

/* ---------------------------------------------------------------------------
 * Sending
 * ------------------------------------------------------------------------- */

static void send_to(const char *buf, int len, const struct sockaddr_in *to, bool dump, const char *note)
{
    log_msg(">>", buf, (size_t)len, dump, note);
    if (sendto(s_sock, buf, (size_t)len, 0, (const struct sockaddr *)to, sizeof(*to)) != len) {
        ESP_LOGW(TAG, "sendto failed: errno %d", errno);
    }
}

static void md5_hex(const char *s, char out[33])
{
    static const char hx[] = "0123456789abcdef";
    uint8_t d[16];
    mbedtls_md5((const unsigned char *)s, strlen(s), d);
    for (int i = 0; i < 16; i++) {
        out[2 * i] = hx[d[i] >> 4];
        out[2 * i + 1] = hx[d[i] & 15];
    }
    out[32] = '\0';
}

/* Append an Authorization / Proxy-Authorization line for method + uri. */
static void add_auth(sb_t *s, const char *method, const char *uri)
{
    auth_t *a = &C.auth;
    char tmp[512], ha1[33], ha2[33], resp[33], cnonce[17];

    snprintf(tmp, sizeof(tmp), "%s:%s:%s", s_user, a->realm, s_pass);
    md5_hex(tmp, ha1);
    memset(tmp, 0, sizeof(tmp));
    snprintf(tmp, sizeof(tmp), "%s:%s", method, uri);
    md5_hex(tmp, ha2);
    if (a->qop_auth) {
        a->nc++;
        rand_hex(cnonce, 8);
        snprintf(tmp, sizeof(tmp), "%s:%s:%08" PRIx32 ":%s:auth:%s", ha1, a->nonce, a->nc, cnonce, ha2);
    } else {
        snprintf(tmp, sizeof(tmp), "%s:%s:%s", ha1, a->nonce, ha2);
    }
    md5_hex(tmp, resp);

    sb_printf(s, "%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\", algorithm=MD5",
              a->proxy ? "Proxy-Authorization" : "Authorization", s_user, a->realm, a->nonce, uri, resp);
    if (a->opaque[0]) {
        sb_printf(s, ", opaque=\"%s\"", a->opaque);
    }
    if (a->qop_auth) {
        sb_printf(s, ", qop=auth, nc=%08" PRIx32 ", cnonce=\"%s\"", a->nc, cnonce);
    }
    sb_printf(s, "\r\n");
}

/* Parse the challenge of a 401/407. Returns false if unusable. */
static bool take_challenge(const msg_t *m, bool *stale)
{
    bool proxy = m->code == 407;
    const char *v = hdr_get(m, proxy ? "Proxy-Authenticate" : "WWW-Authenticate", 0, 0);
    *stale = false;
    if (!v || strncasecmp(v, "Digest", 6) != 0) {
        ESP_LOGE(TAG, "%d without a Digest challenge", m->code);
        return false;
    }
    char alg[24] = "", qop[48] = "", st[8] = "", nonce[sizeof(C.auth.nonce)] = "";
    auth_param(v, "algorithm", alg, sizeof(alg));
    if (alg[0] && strcasecmp(alg, "MD5") != 0) {
        ESP_LOGE(TAG, "unsupported digest algorithm %s", alg);
        return false;
    }
    if (!auth_param(v, "nonce", nonce, sizeof(nonce))) {
        ESP_LOGE(TAG, "challenge without nonce");
        return false;
    }
    if (auth_param(v, "stale", st, sizeof(st))) {
        *stale = strcasecmp(st, "true") == 0;
    }
    auth_t *a = &C.auth;
    if (strcmp(nonce, a->nonce) != 0) {
        a->nc = 0;
    }
    strcpy(a->nonce, nonce);
    a->proxy = proxy;
    a->realm[0] = a->opaque[0] = '\0';
    auth_param(v, "realm", a->realm, sizeof(a->realm));
    auth_param(v, "opaque", a->opaque, sizeof(a->opaque));
    a->qop_auth = false;
    if (auth_param(v, "qop", qop, sizeof(qop))) {
        /* qop is a list, e.g. "auth,auth-int": look for the plain "auth" token. */
        char *save = NULL;
        for (char *t = strtok_r(qop, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
            if (strcasecmp(t, "auth") == 0) {
                a->qop_auth = true;
            }
        }
        if (!a->qop_auth) {
            ESP_LOGW(TAG, "challenge offers qop without plain auth, trying without qop");
        }
    }
    a->valid = true;
    ESP_LOGI(TAG, "%s challenge: realm \"%s\", qop %s%s", proxy ? "proxy" : "www", a->realm,
             a->qop_auth ? "auth" : "none", *stale ? ", stale nonce" : "");
    return true;
}

static void build_sdp(char *out, size_t cap)
{
    sb_t s;
    sb_init(&s, out, cap);
    sb_printf(&s,
              "v=0\r\n"
              "o=- %s 1 IN IP4 %s\r\n"
              "s=kid_phone\r\n"
              "c=IN IP4 %s\r\n"
              "t=0 0\r\n"
              "m=audio %u RTP/AVP 0 101\r\n"
              "a=rtpmap:0 PCMU/8000\r\n"
              "a=rtpmap:101 telephone-event/8000\r\n"
              "a=fmtp:101 0-15\r\n"
              "a=ptime:20\r\n"
              "a=sendrecv\r\n",
              C.sdp_sess, s_pub_ip, s_pub_ip, rtp_local_port());
}

static void via_line(sb_t *s, const char *branch)
{
    sb_printf(s, "Via: SIP/2.0/UDP %s:%d;branch=%s;rport\r\n", s_local_ip, APP_SIP_LOCAL_PORT, branch);
}

static void contact_line(sb_t *s)
{
    sb_printf(s, "Contact: <sip:%s@%s:%u>\r\n", s_user, s_pub_ip, s_pub_port);
}

/* New INVITE transaction: fresh branch, current CSeq, auth if challenged. */
static void send_invite(void)
{
    char sdp[512];
    build_sdp(sdp, sizeof(sdp));

    char rnd[17];
    rand_hex(rnd, 8);
    snprintf(C.inv_branch, sizeof(C.inv_branch), "z9hG4bK%s", rnd);

    sb_t s;
    sb_init(&s, C.inv_msg, sizeof(C.inv_msg));
    sb_printf(&s, "INVITE %s SIP/2.0\r\n", C.ruri);
    via_line(&s, C.inv_branch);
    sb_printf(&s, "Max-Forwards: 70\r\n");
    sb_printf(&s, "From: %s\r\n", C.from_hdr);
    sb_printf(&s, "To: %s\r\n", C.to_hdr);
    sb_printf(&s, "Call-ID: %s\r\n", C.call_id);
    sb_printf(&s, "CSeq: %" PRIu32 " INVITE\r\n", C.cseq);
    contact_line(&s);
    if (C.auth.valid) {
        add_auth(&s, "INVITE", C.ruri);
    }
    sb_printf(&s, "Allow: " ALLOW_METHODS "\r\n");
    sb_printf(&s, "User-Agent: " APP_SIP_USER_AGENT "\r\n");
    sb_printf(&s, "Content-Type: application/sdp\r\n");
    sb_printf(&s, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(sdp), sdp);
    if (s.overflow) {
        ESP_LOGE(TAG, "INVITE too large for buffer");
    }
    C.inv_len = (int)s.len;
    C.inv_provisional = false;
    C.inv_final = 0;
    C.inv_start_ms = now_ms();
    C.inv_interval = APP_SIP_T1_MS;
    C.inv_next_ms = C.inv_start_ms + C.inv_interval;
    send_to(C.inv_msg, C.inv_len, &s_server, true, NULL);
}

/* ACK for a non-2xx final: same branch as the INVITE, To from the response. */
static void send_ack_non2xx(const msg_t *m)
{
    const char *to = hdr_get(m, "To", 't', 0);
    sb_t s;
    sb_init(&s, C.ack_msg, sizeof(C.ack_msg));
    sb_printf(&s, "ACK %s SIP/2.0\r\n", C.ruri);
    via_line(&s, C.inv_branch);
    sb_printf(&s, "Max-Forwards: 70\r\n");
    sb_printf(&s, "From: %s\r\n", C.from_hdr);
    sb_printf(&s, "To: %s\r\n", to ? to : C.to_hdr);
    sb_printf(&s, "Call-ID: %s\r\n", C.call_id);
    sb_printf(&s, "CSeq: %" PRIu32 " ACK\r\n", C.cseq);
    sb_printf(&s, "User-Agent: " APP_SIP_USER_AGENT "\r\n");
    sb_printf(&s, "Content-Length: 0\r\n\r\n");
    C.ack_len = (int)s.len;
    send_to(C.ack_msg, C.ack_len, &s_server, true, NULL);
}

static void route_lines(sb_t *s)
{
    for (int i = 0; i < C.n_route; i++) {
        sb_printf(s, "Route: %s\r\n", C.route[i]);
    }
}

/* ACK for a 2xx: its own transaction (new branch), to the remote target. */
static void send_ack_2xx(void)
{
    char rnd[17], branch[40];
    rand_hex(rnd, 8);
    snprintf(branch, sizeof(branch), "z9hG4bK%s", rnd);
    sb_t s;
    sb_init(&s, C.ack_msg, sizeof(C.ack_msg));
    sb_printf(&s, "ACK %s SIP/2.0\r\n", C.remote_target);
    via_line(&s, branch);
    sb_printf(&s, "Max-Forwards: 70\r\n");
    route_lines(&s);
    sb_printf(&s, "From: %s\r\n", C.from_hdr);
    sb_printf(&s, "To: %s\r\n", C.dlg_to_hdr);
    sb_printf(&s, "Call-ID: %s\r\n", C.call_id);
    sb_printf(&s, "CSeq: %" PRIu32 " ACK\r\n", C.cseq);
    contact_line(&s);
    sb_printf(&s, "User-Agent: " APP_SIP_USER_AGENT "\r\n");
    sb_printf(&s, "Content-Length: 0\r\n\r\n");
    C.ack_len = (int)s.len;
    send_to(C.ack_msg, C.ack_len, &s_server, true, NULL);
}

static void nit_begin(const char *method)
{
    copy_n(C.nit_method, sizeof(C.nit_method), method, strlen(method));
    C.nit_active = true;
    C.nit_start_ms = now_ms();
    C.nit_interval = APP_SIP_T1_MS;
    C.nit_next_ms = C.nit_start_ms + C.nit_interval;
    send_to(C.nit_msg, C.nit_len, &s_server, true, NULL);
}

/* CANCEL: same branch, Request-URI, From, To and CSeq number as the INVITE. */
static void start_cancel(void)
{
    C.hangup_pending = true;
    copy_n(C.nit_branch, sizeof(C.nit_branch), C.inv_branch, sizeof(C.inv_branch));
    C.nit_cseq = C.cseq;
    sb_t s;
    sb_init(&s, C.nit_msg, sizeof(C.nit_msg));
    sb_printf(&s, "CANCEL %s SIP/2.0\r\n", C.ruri);
    via_line(&s, C.nit_branch);
    sb_printf(&s, "Max-Forwards: 70\r\n");
    sb_printf(&s, "From: %s\r\n", C.from_hdr);
    sb_printf(&s, "To: %s\r\n", C.to_hdr);
    sb_printf(&s, "Call-ID: %s\r\n", C.call_id);
    sb_printf(&s, "CSeq: %" PRIu32 " CANCEL\r\n", C.cseq);
    sb_printf(&s, "User-Agent: " APP_SIP_USER_AGENT "\r\n");
    sb_printf(&s, "Content-Length: 0\r\n\r\n");
    C.nit_len = (int)s.len;
    C.ending_deadline_ms = now_ms() + APP_SIP_TIMEOUT_MS;
    nit_begin("CANCEL");
}

/* BYE: in-dialog, next CSeq, auth only after the BYE itself was challenged. */
static void start_bye(void)
{
    C.hangup_pending = true;
    char rnd[17];
    rand_hex(rnd, 8);
    snprintf(C.nit_branch, sizeof(C.nit_branch), "z9hG4bK%s", rnd);
    C.nit_cseq = ++C.local_cseq;
    sb_t s;
    sb_init(&s, C.nit_msg, sizeof(C.nit_msg));
    sb_printf(&s, "BYE %s SIP/2.0\r\n", C.remote_target);
    via_line(&s, C.nit_branch);
    sb_printf(&s, "Max-Forwards: 70\r\n");
    route_lines(&s);
    sb_printf(&s, "From: %s\r\n", C.from_hdr);
    sb_printf(&s, "To: %s\r\n", C.dlg_to_hdr);
    sb_printf(&s, "Call-ID: %s\r\n", C.call_id);
    sb_printf(&s, "CSeq: %" PRIu32 " BYE\r\n", C.nit_cseq);
    if (C.bye_auth_tries > 0 && C.auth.valid) {
        add_auth(&s, "BYE", C.remote_target);
    }
    sb_printf(&s, "User-Agent: " APP_SIP_USER_AGENT "\r\n");
    sb_printf(&s, "Content-Length: 0\r\n\r\n");
    C.nit_len = (int)s.len;
    nit_begin("BYE");
}

/* Response to an incoming request: echo Via (all), From, To, Call-ID, CSeq. */
static void respond(const msg_t *m, int code, const char *reason, const char *extra, const char *body,
                    const struct sockaddr_in *to)
{
    char out[2048];
    sb_t s;
    sb_init(&s, out, sizeof(out));
    sb_printf(&s, "SIP/2.0 %d %s\r\n", code, reason);
    const char *v;
    for (int i = 0; (v = hdr_get(m, "Via", 'v', i)) != NULL; i++) {
        sb_printf(&s, "Via: %s\r\n", v);
    }
    if ((v = hdr_get(m, "From", 'f', 0)) != NULL) {
        sb_printf(&s, "From: %s\r\n", v);
    }
    if ((v = hdr_get(m, "To", 't', 0)) != NULL) {
        char tag[64];
        if (param_get(v, "tag", tag, sizeof(tag))) {
            sb_printf(&s, "To: %s\r\n", v);
        } else {
            /* Out-of-dialog request: add a To tag of our own. */
            rand_hex(tag, 4);
            sb_printf(&s, "To: %s;tag=%s\r\n", v, tag);
        }
    }
    if ((v = hdr_get(m, "Call-ID", 'i', 0)) != NULL) {
        sb_printf(&s, "Call-ID: %s\r\n", v);
    }
    if ((v = hdr_get(m, "CSeq", 0, 0)) != NULL) {
        sb_printf(&s, "CSeq: %s\r\n", v);
    }
    if (extra) {
        sb_printf(&s, "%s", extra);
    }
    sb_printf(&s, "User-Agent: " APP_SIP_USER_AGENT "\r\n");
    if (body) {
        sb_printf(&s, "Content-Type: application/sdp\r\nContent-Length: %u\r\n\r\n%s", (unsigned)strlen(body), body);
    } else {
        sb_printf(&s, "Content-Length: 0\r\n\r\n");
    }
    send_to(out, (int)s.len, to, true, NULL);
}

/* ---------------------------------------------------------------------------
 * Call control
 * ------------------------------------------------------------------------- */

static void end_call(const char *why)
{
    int64_t now = now_ms();
    record_result(C.failed, C.fail_code, C.answer_ms != 0, C.remote_bye,
                  C.answer_ms ? (uint32_t)(now - C.answer_ms) : 0, why, C.auth_tries > 0);
    rtp_session_end();
    C.active = false;
    C.nit_active = false;
    C.hangup_pending = false;
    set_state(CALL_IDLE, why);
}

/* A call that could not even start (no WiFi, not set up, DNS). */
static void start_failed(const char *why)
{
    ESP_LOGW(TAG, "%s", why);
    record_result(true, 0, false, false, 0, why, false);
}

static void start_call(int contact)
{
    app_cfg_t cfg;
    config_get(&cfg);
    strlcpy(s_domain, cfg.sip_domain, sizeof(s_domain));
    strlcpy(s_user, cfg.sip_user, sizeof(s_user));
    strlcpy(s_pass, cfg.sip_pass, sizeof(s_pass));
    strlcpy(s_dest, cfg.contacts[contact].number, sizeof(s_dest));
    bool ready = cfg.provisioned;
    config_wipe(&cfg);
    atomic_store(&s_contact, contact);

    if (!ready || !s_dest[0]) {
        start_failed(!ready ? "not set up yet, cannot call" : "no number for this contact");
        return;
    }
    const char *num = s_dest;
    size_t nl = strlen(num);
    if (!wifi_is_connected() || !wifi_get_ip(s_local_ip, sizeof(s_local_ip))) {
        start_failed("no WiFi, cannot call");
        return;
    }
    ESP_LOGI(TAG, "calling ...%s as %s@%s", nl > 4 ? num + nl - 4 : num, s_user, s_domain);

    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
    struct addrinfo *res = NULL;
    char port[8];
    snprintf(port, sizeof(port), "%d", APP_SIP_SERVER_PORT);
    int err = getaddrinfo(s_domain, port, &hints, &res);
    if (err != 0 || !res) {
        ESP_LOGE(TAG, "DNS lookup of %s failed (%d)", s_domain, err);
        if (res) {
            freeaddrinfo(res);
        }
        start_failed("DNS lookup of the SIP domain failed");
        return;
    }
    memcpy(&s_server, res->ai_addr, sizeof(s_server));
    freeaddrinfo(res);
    char sip_ip[16];
    inet_ntoa_r(s_server.sin_addr, sip_ip, sizeof(sip_ip));
    ESP_LOGI(TAG, "SIP server %s -> %s:%d, local %s:%d", s_domain, sip_ip, APP_SIP_SERVER_PORT,
             s_local_ip, APP_SIP_LOCAL_PORT);

    memset(&C, 0, sizeof(C));
    char rnd[17];
    rand_hex(rnd, 8);
    snprintf(C.call_id, sizeof(C.call_id), "%s@%s", rnd, s_local_ip);
    rand_hex(C.local_tag, 4);
    snprintf(C.from_hdr, sizeof(C.from_hdr), "<sip:%s@%s>;tag=%s", s_user, s_domain, C.local_tag);
    snprintf(C.ruri, sizeof(C.ruri), "sip:%s@%s", s_dest, s_domain);
    snprintf(C.to_hdr, sizeof(C.to_hdr), "<%s>", C.ruri);
    snprintf(C.sdp_sess, sizeof(C.sdp_sess), "%" PRIu32, esp_random() % 1000000000u);
    C.cseq = 1 + esp_random() % 1000;
    C.active = true;
    C.call_start_ms = now_ms();

    /* Until a response tells us otherwise, assume no NAT. */
    strcpy(s_pub_ip, s_local_ip);
    s_pub_port = APP_SIP_LOCAL_PORT;

    rtp_session_begin();
    char why[24];
    snprintf(why, sizeof(why), "contact %d", contact);
    set_state(CALL_CALLING, why);
    send_invite();
}

static void hangup(const char *why)
{
    switch (sip_call_state()) {
    case CALL_CALLING:
        if (C.inv_provisional) {
            start_cancel();
        } else {
            /* RFC 3261: no CANCEL before a provisional response. Send it when
             * one arrives; Timer B ends the call if nothing ever does. */
            C.hangup_pending = true;
            ESP_LOGI(TAG, "no response yet, CANCEL will follow the first provisional");
        }
        break;
    case CALL_RINGING:
        start_cancel();
        break;
    case CALL_IN_CALL:
        start_bye();
        break;
    default:
        return;
    }
    rtp_session_end();
    set_state(CALL_ENDING, why);
}

/* Remote media address from an SDP body, if any. */
static void apply_sdp(const msg_t *m, const char *what)
{
    const char *ct = hdr_get(m, "Content-Type", 'c', 0);
    if (!m->body || m->body_len == 0 || !ct || strncasecmp(ct, "application/sdp", 15) != 0) {
        if (C.inv_final >= 200) {
            ESP_LOGW(TAG, "%s without SDP", what);
        }
        return;
    }
    char body[1024];
    copy_n(body, sizeof(body), m->body, m->body_len);
    char ip[16] = "";
    unsigned port = 0;
    bool pcmu = false;
    char *save = NULL;
    for (char *l = strtok_r(body, "\r\n", &save); l; l = strtok_r(NULL, "\r\n", &save)) {
        if (strncmp(l, "c=IN IP4 ", 9) == 0) {
            copy_n(ip, sizeof(ip), l + 9, strcspn(l + 9, " /"));
        } else if (strncmp(l, "m=audio ", 8) == 0) {
            char *p = l + 8;
            port = (unsigned)strtoul(p, &p, 10);
            p = strstr(p, "RTP/AVP");
            if (p) {
                p += 7;
                while (*p) {
                    char *e = NULL;
                    long pt = strtol(p, &e, 10);
                    if (e == p) {
                        break;
                    }
                    p = e;
                    if (pt == 0) {
                        pcmu = true;
                    }
                }
            }
        }
    }
    if (!ip[0] || port == 0) {
        ESP_LOGW(TAG, "%s: SDP has no usable c=/m= line", what);
        return;
    }
    ESP_LOGI(TAG, "%s: remote media %s:%u%s", what, ip, port, pcmu ? ", PCMU" : ", WARNING: PCMU not in answer");
    if (strcmp(ip, "0.0.0.0") == 0) {
        return; /* on hold */
    }
    rtp_set_remote(ip, (uint16_t)port);
}

/* Learn our NAT public address from received= / rport= of the top Via. */
static void learn_nat(const msg_t *m)
{
    const char *via = hdr_get(m, "Via", 'v', 0);
    if (!via) {
        return;
    }
    char top[256], rcv[24] = "", rport[8] = "";
    first_elem(via, top, sizeof(top));
    param_get(top, "received", rcv, sizeof(rcv));
    param_get(top, "rport", rport, sizeof(rport));
    struct in_addr tmp;
    bool changed = false;
    if (rcv[0] && inet_aton(rcv, &tmp) && strcmp(rcv, s_pub_ip) != 0) {
        strcpy(s_pub_ip, rcv);
        changed = true;
    }
    int rp = atoi(rport);
    if (rp > 0 && rp < 65536 && rp != s_pub_port) {
        s_pub_port = (uint16_t)rp;
        changed = true;
    }
    if (changed) {
        ESP_LOGI(TAG, "NAT: public address %s, SIP port %u (local %s:%d); used in Contact and SDP c=",
                 s_pub_ip, s_pub_port, s_local_ip, APP_SIP_LOCAL_PORT);
    }
}

/* Capture the dialog from a 2xx: remote tag (To), remote target, route set. */
static void take_dialog(const msg_t *m)
{
    const char *to = hdr_get(m, "To", 't', 0);
    copy_n(C.dlg_to_hdr, sizeof(C.dlg_to_hdr), to ? to : C.to_hdr, sizeof(C.dlg_to_hdr));
    const char *ct = hdr_get(m, "Contact", 'm', 0);
    if (ct) {
        uri_of(ct, C.remote_target, sizeof(C.remote_target));
    } else {
        copy_n(C.remote_target, sizeof(C.remote_target), C.ruri, sizeof(C.ruri));
    }

    /* Record-Route entries in order, then reversed for the UAC route set. */
    char rr[MAX_ROUTES][URI_MAX];
    int n = 0;
    const char *v;
    for (int i = 0; (v = hdr_get(m, "Record-Route", 0, i)) != NULL; i++) {
        const char *p = v;
        while (*p) {
            while (*p == ' ' || *p == ',') {
                p++;
            }
            if (!*p) {
                break;
            }
            size_t el = elem_len(p);
            if (n == MAX_ROUTES || el >= URI_MAX) {
                ESP_LOGW(TAG, "Record-Route entry dropped (too many or too long), in-dialog requests may fail");
            } else {
                copy_n(rr[n], sizeof(rr[n]), p, el);
                n++;
            }
            p += el;
        }
    }
    C.n_route = n;
    for (int i = 0; i < n; i++) {
        strcpy(C.route[i], rr[n - 1 - i]);
    }
    C.local_cseq = C.cseq;
    ESP_LOGI(TAG, "dialog: remote target %s, %d route(s)", C.remote_target, C.n_route);
}

static void on_invite_response(const msg_t *m)
{
    const int code = m->code;
    if (!C.active) {
        if (code >= 200 && C.ack_len) {
            send_to(C.ack_msg, C.ack_len, &s_server, false, " (re-ACK, retransmitted final)");
        }
        return;
    }

    if (code < 200) {
        C.inv_provisional = true;
        if (code > 100 && sip_call_state() == CALL_CALLING && !C.hangup_pending) {
            set_state(CALL_RINGING, code == 183 ? "183 session progress" : "ringing");
        }
        if (code > 100 && sip_call_state() == CALL_RINGING) {
            apply_sdp(m, "early media");
        }
        if (C.hangup_pending && !C.nit_active) {
            start_cancel();
        }
        return;
    }

    if (code < 300) {
        if (C.inv_final >= 200 && C.inv_final < 300) {
            send_to(C.ack_msg, C.ack_len, &s_server, false, " (re-ACK, retransmitted 200)");
            return;
        }
        C.inv_final = code;
        C.inv_provisional = true;
        take_dialog(m);
        send_ack_2xx();
        if (C.hangup_pending) {
            /* Answered while we were hanging up (200 crossed the CANCEL). */
            C.nit_active = false;
            start_bye();
            set_state(CALL_ENDING, "answered after hang up, BYE");
            return;
        }
        apply_sdp(m, "answer");
        C.answer_ms = now_ms();
        C.keepalive_ms = now_ms() + APP_SIP_KEEPALIVE_MS;
        set_state(CALL_IN_CALL, "200 OK");
        return;
    }

    if (C.inv_final >= 300) {
        send_to(C.ack_msg, C.ack_len, &s_server, false, " (re-ACK, retransmitted final)");
        return;
    }
    C.inv_final = code;
    send_ack_non2xx(m);
    if ((code == 401 || code == 407) && !C.hangup_pending) {
        bool stale = false;
        if (take_challenge(m, &stale) && (C.auth_tries == 0 || (stale && C.auth_tries < 3))) {
            C.auth_tries++;
            C.cseq++;
            send_invite();
            return;
        }
        ESP_LOGE(TAG, "authentication failed: SIP user / password do not match the Twilio credential list (run setup again)");
    } else if (code == 403 && C.auth_tries > 0) {
        ESP_LOGE(TAG, "403 after digest auth: Twilio rejected the login (wrong password, or another credential "
                      "list on the domain holds the same username)");
    }
    char why[80];
    if (C.hangup_pending) {
        snprintf(why, sizeof(why), "cancelled: %d %.40s", code, m->reason);
    } else {
        snprintf(why, sizeof(why), "call failed: %d %.40s", code, m->reason);
        C.failed = true;
        C.fail_code = code;
    }
    end_call(why);
}

static void on_nit_response(const msg_t *m)
{
    const int code = m->code;
    if (code < 200) {
        return;
    }
    C.nit_active = false;
    if (strcmp(C.nit_method, "BYE") == 0) {
        bool stale = false;
        if ((code == 401 || code == 407) && C.bye_auth_tries < 2 && take_challenge(m, &stale)) {
            C.bye_auth_tries++;
            start_bye();
            return;
        }
        char why[48];
        if (code < 300) {
            snprintf(why, sizeof(why), "BYE accepted");
        } else {
            snprintf(why, sizeof(why), "BYE answered %d", code);
        }
        end_call(why);
    } else {
        /* CANCEL: on 200 the INVITE finishes with 487, wait for it. */
        if (code >= 300) {
            ESP_LOGW(TAG, "CANCEL rejected (%d), ending locally", code);
            end_call("CANCEL rejected");
        }
    }
}

static void on_response(const msg_t *m)
{
    const char *via = hdr_get(m, "Via", 'v', 0);
    const char *cid = hdr_get(m, "Call-ID", 'i', 0);
    const char *cseq = hdr_get(m, "CSeq", 0, 0);
    if (!via || !cid || !cseq || strcmp(cid, C.call_id) != 0) {
        ESP_LOGW(TAG, "response for an unknown call, ignored");
        return;
    }
    char top[256], branch[64] = "";
    first_elem(via, top, sizeof(top));
    param_get(top, "branch", branch, sizeof(branch));
    const char *method = strchr(cseq, ' ');
    method = method ? method + 1 : "";
    while (*method == ' ') {
        method++;
    }

    if (C.active) {
        learn_nat(m);
    }
    if (strcmp(branch, C.inv_branch) == 0 && strcasecmp(method, "INVITE") == 0) {
        on_invite_response(m);
    } else if (C.nit_active && strcmp(branch, C.nit_branch) == 0 && strcasecmp(method, C.nit_method) == 0) {
        on_nit_response(m);
    } else {
        ESP_LOGD(TAG, "stray response (%s), ignored", method);
    }
}

static void on_request(const msg_t *m, const struct sockaddr_in *from)
{
    const char *cid = hdr_get(m, "Call-ID", 'i', 0);
    bool ours = C.active && cid && strcmp(cid, C.call_id) == 0;
    const call_state_t st = sip_call_state();

    if (strcmp(m->method, "ACK") == 0) {
        return;
    }
    if (strcmp(m->method, "OPTIONS") == 0) {
        respond(m, 200, "OK", "Allow: " ALLOW_METHODS "\r\nAccept: application/sdp\r\n", NULL, from);
    } else if (strcmp(m->method, "BYE") == 0) {
        if (ours) {
            respond(m, 200, "OK", NULL, NULL, from);
            C.remote_bye = true;
            end_call(st == CALL_IN_CALL ? "remote hung up" : "remote BYE");
        } else {
            respond(m, 481, "Call/Transaction Does Not Exist", NULL, NULL, from);
        }
    } else if (strcmp(m->method, "INVITE") == 0) {
        if (ours && st == CALL_IN_CALL) {
            /* Re-INVITE (session refresh or media change): answer with our SDP. */
            apply_sdp(m, "re-INVITE");
            char sdp[512], extra[160];
            build_sdp(sdp, sizeof(sdp));
            sb_t s;
            sb_init(&s, extra, sizeof(extra));
            contact_line(&s);
            sb_printf(&s, "Allow: " ALLOW_METHODS "\r\n");
            respond(m, 200, "OK", extra, sdp, from);
        } else {
            respond(m, 486, "Busy Here", NULL, NULL, from);
        }
    } else if (strcmp(m->method, "CANCEL") == 0) {
        respond(m, 481, "Call/Transaction Does Not Exist", NULL, NULL, from);
    } else {
        respond(m, 501, "Not Implemented", "Allow: " ALLOW_METHODS "\r\n", NULL, from);
    }
}

static void run_timers(void)
{
    if (!C.active) {
        return;
    }
    const int64_t now = now_ms();
    const call_state_t st = sip_call_state();

    /* INVITE: Timer A retransmits until any response, Timer B gives up. */
    if (!C.inv_provisional && C.inv_final == 0) {
        if (now - C.inv_start_ms >= APP_SIP_TIMEOUT_MS) {
            C.failed = true;
            end_call("no response to INVITE (Timer B)");
            return;
        }
        if (now >= C.inv_next_ms) {
            send_to(C.inv_msg, C.inv_len, &s_server, false, " (retransmit)");
            C.inv_interval *= 2;
            C.inv_next_ms = now + C.inv_interval;
        }
    }

    if ((st == CALL_CALLING || st == CALL_RINGING) && now - C.call_start_ms >= APP_SIP_RING_TIMEOUT_MS) {
        hangup("no answer, ring timeout");
        return;
    }

    /* BYE / CANCEL: Timer E retransmits (capped at T2), Timer F gives up. */
    if (C.nit_active) {
        if (now - C.nit_start_ms >= APP_SIP_TIMEOUT_MS) {
            ESP_LOGW(TAG, "no final response to %s", C.nit_method);
            end_call("transaction timeout");
            return;
        }
        if (now >= C.nit_next_ms) {
            send_to(C.nit_msg, C.nit_len, &s_server, false, " (retransmit)");
            C.nit_interval = C.nit_interval * 2 > APP_SIP_T2_MS ? APP_SIP_T2_MS : C.nit_interval * 2;
            C.nit_next_ms = now + C.nit_interval;
        }
    }

    if (st == CALL_ENDING && C.ending_deadline_ms && now >= C.ending_deadline_ms) {
        end_call("no final response after CANCEL");
        return;
    }

    if (st == CALL_IN_CALL && now >= C.keepalive_ms) {
        C.keepalive_ms = now + APP_SIP_KEEPALIVE_MS;
        static const char crlf[] = "\r\n\r\n";
        sendto(s_sock, crlf, 4, 0, (const struct sockaddr *)&s_server, sizeof(s_server));
        ESP_LOGD(TAG, "CRLF keepalive");
    }
}

static void sip_task(void *arg)
{
    (void)arg;
    while (1) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s_sock, s_rx, RX_BUF_BYTES, 0, (struct sockaddr *)&from, &fl);
        if (n > 0) {
            bool blank = true;
            for (int i = 0; i < n && blank; i++) {
                blank = s_rx[i] == '\r' || s_rx[i] == '\n' || s_rx[i] == ' ';
            }
            if (!blank) {
                log_msg("<<", s_rx, (size_t)n, true, NULL);
                msg_t m;
                if (!parse_msg(s_rx, (size_t)n, &m)) {
                    ESP_LOGW(TAG, "unparseable SIP message (%d bytes), ignored", n);
                } else if (m.is_req) {
                    on_request(&m, &from);
                } else {
                    on_response(&m);
                }
            }
        }

        cmd_t cmd;
        while (xQueueReceive(s_cmd_q, &cmd, 0) == pdTRUE) {
            if (cmd >= CMD_START) {
                if (sip_call_state() == CALL_IDLE) {
                    start_call((int)(cmd - CMD_START));
                } else {
                    ESP_LOGW(TAG, "call start ignored, state %s", sip_call_state_name(sip_call_state()));
                }
            } else {
                hangup("button");
            }
        }

        run_timers();
    }
}

void sip_start(void)
{
    s_cmd_q = xQueueCreate(4, sizeof(cmd_t));
    assert(s_cmd_q);

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket failed: errno %d", errno);
        return;
    }
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(APP_SIP_LOCAL_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
        ESP_LOGE(TAG, "bind to port %d failed: errno %d", APP_SIP_LOCAL_PORT, errno);
        close(s_sock);
        s_sock = -1;
        return;
    }
    /* Short receive timeout: the same loop also runs commands and timers. */
    struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (!rtp_start()) {
        ESP_LOGE(TAG, "RTP init failed, calls will have no audio");
    }
    xTaskCreatePinnedToCore(sip_task, "sip", 10240, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "SIP UA ready: UDP port %d (account read from config at each call)", APP_SIP_LOCAL_PORT);
}
