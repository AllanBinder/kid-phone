/*
 * Twilio self-provisioning. Sequence (every step reuses what a previous run
 * created, found by friendly / unique name, so rerunning is safe):
 *
 *   1. GET  Accounts/{SID}.json                      verify SID + token
 *   2. GET  IncomingPhoneNumbers.json                first number with voice = caller ID
 *   3. SIP credential list "kid-phone", credential "kidphone-<last 3 MAC bytes>"
 *      with a fresh random password (an existing credential's password cannot
 *      be read back, so it is updated on every run). Credentials named
 *      "handset1*" (older firmware) are deleted from that list; other
 *      kidphone-* credentials belong to other phones and are kept.
 *   4. SIP domain friendly name "kid-phone" (else kidphone-<6 hex>.sip.twilio.com),
 *      credential list mapped for call authentication (Auth/Calls). Warns if
 *      another list mapped on the domain holds the same username (Twilio may
 *      then check the wrong password and answer 403).
 *   5. Serverless service "kid-phone", environment "prod", function "voice":
 *      upload twilio/voice.js (embedded at build time) as /voice (protected),
 *      build, wait for the build, set CALLER_ID and ALLOWED, deploy
 *   6. SIP domain VoiceUrl = https://<environment domain>/voice
 *   7. Save SIP domain/user/password, caller ID and contacts to NVS
 *
 * Numbers-only run (contacts changed on an already set up phone): 1, find the
 * service and environment, set ALLOWED, redeploy the environment's current
 * build, save the contacts. Falls back to the full run if anything is missing.
 *
 * Not changed by the device: Twilio's per-country Voice geographic permissions
 * (Console > Voice > Settings > Geo permissions). Calls abroad fail until the
 * parent enables the destination country there; the setup page shows that
 * hint when a test call fails.
 *
 * Privacy: only api.twilio.com, serverless.twilio.com and
 * serverless-upload.twilio.com are contacted, over HTTPS with the ESP-IDF
 * certificate bundle. The Auth Token is kept in this task's heap block only,
 * never logged, never stored, and zeroed when the task ends.
 */
#include "twilio_provision.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"

#include "app_config.h"

static const char *TAG = "provision";

#define API      "https://api.twilio.com/2010-04-01/Accounts/"
#define SLS      "https://serverless.twilio.com/v1/Services"
#define SLS_UP   "https://serverless-upload.twilio.com/v1/Services"

/* twilio/voice.js, embedded by src/CMakeLists.txt (EMBED_TXTFILES adds a NUL). */
extern const char voice_js_start[] asm("_binary_voice_js_start");
extern const char voice_js_end[] asm("_binary_voice_js_end");

/* ---------------------------------------------------------------------------
 * Progress (read by the web server)
 * ------------------------------------------------------------------------- */

#define MAX_STEPS 24
#define STEP_LEN  120

typedef enum { ST_IDLE, ST_RUNNING, ST_DONE, ST_FAILED } run_state_t;

static SemaphoreHandle_t s_lock;
static run_state_t s_state = ST_IDLE;
static char s_steps[MAX_STEPS][STEP_LEN];
static int s_nsteps;
static char s_error[200];
static char s_hint[240];

static void ensure_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        assert(s_lock);
    }
}

static void step(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void step(const char *fmt, ...)
{
    char line[STEP_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", line);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_nsteps == MAX_STEPS) {
        memmove(s_steps[0], s_steps[1], (MAX_STEPS - 1) * STEP_LEN);
        s_nsteps--;
    }
    strlcpy(s_steps[s_nsteps++], line, STEP_LEN);
    xSemaphoreGive(s_lock);
}

bool twilio_provision_running(void)
{
    return s_state == ST_RUNNING;
}

void twilio_provision_status_json(cJSON *obj)
{
    ensure_lock();
    static const char *names[] = {"idle", "running", "done", "failed"};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddStringToObject(obj, "state", names[s_state]);
    cJSON *arr = cJSON_AddArrayToObject(obj, "steps");
    for (int i = 0; i < s_nsteps; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(s_steps[i]));
    }
    if (s_state == ST_FAILED) {
        cJSON_AddStringToObject(obj, "error", s_error);
        cJSON_AddStringToObject(obj, "hint", s_hint);
    }
    xSemaphoreGive(s_lock);
}

/* ---------------------------------------------------------------------------
 * Run context. One heap block, zeroed and freed at the end (holds the token).
 * ------------------------------------------------------------------------- */

typedef struct {
    char sid[40];
    char auth[160];            /* "Basic base64(sid:token)" */
    cfg_contact_t contacts[CFG_MAX_CONTACTS];
    bool numbers_only;
    char sip_user[33];

    char caller_id[20];
    char cl_sid[40];
    char cr_sid[40];
    char sip_pass[33];
    char sd_sid[40];
    char domain[96];
    char svc_sid[40];
    char env_sid[40];
    char env_domain[128];
    char fn_sid[40];
    char fnv_sid[40];
    char build_sid[40];
    char other_lists[4][40];   /* other credential lists mapped on the domain */
    int n_other_lists;

    /* last response */
    int status;
    char *resp;
    size_t resp_len;
    cJSON *json;
    char tw_msg[160];
    int tw_code;

    char url[256];
    char body[512];
} prov_t;

/* Thrown by a failing step: user-facing message + hint. */
static bool fail(prov_t *p, const char *error, const char *hint)
{
    (void)p;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_error, error, sizeof(s_error));
    strlcpy(s_hint, hint ? hint : "", sizeof(s_hint));
    xSemaphoreGive(s_lock);
    ESP_LOGE(TAG, "FAILED: %s", error);
    return false;
}

/* Twilio error, generic wording plus Twilio's own message. */
static bool fail_api(prov_t *p, const char *what)
{
    char e[200];
    if (p->status <= 0) {
        snprintf(e, sizeof(e), "%s: could not reach Twilio (check the internet connection)", what);
    } else if (p->tw_msg[0]) {
        snprintf(e, sizeof(e), "%s: Twilio said \"%.100s\" (%d)", what, p->tw_msg, p->tw_code);
    } else {
        snprintf(e, sizeof(e), "%s: Twilio answered HTTP %d", what, p->status);
    }
    return fail(p, e, "Press Connect again to retry. Nothing is lost by retrying.");
}

/* ---------------------------------------------------------------------------
 * HTTP
 * ------------------------------------------------------------------------- */

static void clear_resp(prov_t *p)
{
    if (p->json) {
        cJSON_Delete(p->json);
        p->json = NULL;
    }
    free(p->resp);
    p->resp = NULL;
    p->resp_len = 0;
    p->status = 0;
    p->tw_msg[0] = '\0';
    p->tw_code = 0;
}

/* One request on a fresh connection. Fills p->status, p->resp, p->json
 * (if the body is JSON) and the Twilio error fields. Returns true on 2xx. */
static bool http(prov_t *p, esp_http_client_method_t method, const char *url, const char *ctype,
                 const char *body, int body_len)
{
    clear_resp(p);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = APP_PROV_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return false;
    }
    esp_http_client_set_header(c, "Authorization", p->auth);
    esp_http_client_set_header(c, "Accept", "application/json");
    esp_http_client_set_header(c, "User-Agent", APP_SIP_USER_AGENT);
    if (ctype) {
        esp_http_client_set_header(c, "Content-Type", ctype);
    }
    esp_err_t err = esp_http_client_open(c, body_len);
    if (err == ESP_OK && body_len > 0) {
        int off = 0;
        while (off < body_len) {
            int w = esp_http_client_write(c, body + off, body_len - off);
            if (w <= 0) {
                err = ESP_FAIL;
                break;
            }
            off += w;
        }
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(c) >= 0) {
        p->status = esp_http_client_get_status_code(c);
        size_t cap = 0;
        while (1) {
            if (p->resp_len + 1025 > cap) {
                size_t ncap = cap ? cap * 2 : 4096;
                if (ncap > APP_PROV_MAX_RESP) {
                    ESP_LOGW(TAG, "response too large, truncated");
                    break;
                }
                char *nb = realloc(p->resp, ncap);
                if (!nb) {
                    break;
                }
                p->resp = nb;
                cap = ncap;
            }
            int r = esp_http_client_read(c, p->resp + p->resp_len, 1024);
            if (r <= 0) {
                break;
            }
            p->resp_len += (size_t)r;
        }
        if (p->resp) {
            p->resp[p->resp_len] = '\0';
            p->json = cJSON_ParseWithLength(p->resp, p->resp_len);
        }
    } else {
        ESP_LOGW(TAG, "request failed: %s", esp_err_to_name(err));
        p->status = -1;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    bool ok = p->status >= 200 && p->status < 300;
    if (!ok && p->json) {
        const cJSON *m = cJSON_GetObjectItem(p->json, "message");
        const cJSON *code = cJSON_GetObjectItem(p->json, "code");
        if (cJSON_IsString(m)) {
            strlcpy(p->tw_msg, m->valuestring, sizeof(p->tw_msg));
        }
        if (cJSON_IsNumber(code)) {
            p->tw_code = code->valueint;
        }
    }
    /* Log the URL path without query; never the body (it can hold the SIP password). */
    const char *path = strstr(url, ".com");
    ESP_LOGI(TAG, "%s %.*s -> %d%s%s",
             method == HTTP_METHOD_GET ? "GET" : (method == HTTP_METHOD_DELETE ? "DELETE" : "POST"),
             path ? (int)strcspn(path + 4, "?") : 0, path ? path + 4 : "", p->status,
             p->tw_msg[0] ? " " : "", p->tw_msg);
    return ok;
}

static bool get(prov_t *p, const char *url)
{
    return http(p, HTTP_METHOD_GET, url, NULL, NULL, 0);
}

static bool post_form(prov_t *p, const char *url, const char *form)
{
    return http(p, HTTP_METHOD_POST, url, "application/x-www-form-urlencoded", form, (int)strlen(form));
}

static bool del(prov_t *p, const char *url)
{
    return http(p, HTTP_METHOD_DELETE, url, NULL, NULL, 0);
}

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/* Append key=urlencoded(value) to a form body. */
static void form_add(char *buf, size_t cap, const char *key, const char *val)
{
    size_t n = strlen(buf);
    int w = snprintf(buf + n, cap - n, "%s%s=", n ? "&" : "", key);
    if (w < 0 || (size_t)w >= cap - n) {
        return;
    }
    n += (size_t)w;
    static const char hx[] = "0123456789ABCDEF";
    for (const unsigned char *s = (const unsigned char *)val; *s && n + 4 < cap; s++) {
        if (isalnum(*s) || *s == '-' || *s == '_' || *s == '.' || *s == '~') {
            buf[n++] = (char)*s;
        } else {
            buf[n++] = '%';
            buf[n++] = hx[*s >> 4];
            buf[n++] = hx[*s & 15];
        }
    }
    buf[n] = '\0';
}

/* The list in a Twilio list response: the first top-level array (the key is
 * "credential_lists", "domains", "services", ... depending on the resource). */
static const cJSON *list_of(const prov_t *p)
{
    if (!p->json) {
        return NULL;
    }
    const cJSON *it;
    cJSON_ArrayForEach(it, p->json) {
        if (cJSON_IsArray(it)) {
            return it;
        }
    }
    return NULL;
}

/* Find the element whose string field `key` equals `val`. */
static const cJSON *find_by(const prov_t *p, const char *key, const char *val)
{
    const cJSON *arr = list_of(p);
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        const cJSON *f = cJSON_GetObjectItem(it, key);
        if (cJSON_IsString(f) && strcmp(f->valuestring, val) == 0) {
            return it;
        }
    }
    return NULL;
}

static bool copy_field(const cJSON *obj, const char *key, char *out, size_t len)
{
    const cJSON *f = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsString(f) || !f->valuestring[0]) {
        out[0] = '\0';
        return false;
    }
    strlcpy(out, f->valuestring, len);
    return true;
}

static const char *last4(const char *num)
{
    size_t n = strlen(num);
    return n > 4 ? num + n - 4 : num;
}

/* Twilio credential password rule: >= 12 chars with upper, lower and digit. */
static void gen_password(char *out, size_t len)
{
    static const char alpha[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    const size_t na = sizeof(alpha) - 1;
    const size_t n = len - 1; /* 32 chars */
    bool up, lo, dg;
    do {
        up = lo = dg = false;
        for (size_t i = 0; i < n; i++) {
            uint32_t r;
            do {
                r = esp_random() & 0xff;
            } while (r >= 256 - (256 % na)); /* no modulo bias */
            char ch = alpha[r % na];
            out[i] = ch;
            up |= isupper((unsigned char)ch) != 0;
            lo |= islower((unsigned char)ch) != 0;
            dg |= isdigit((unsigned char)ch) != 0;
        }
        out[n] = '\0';
    } while (!(up && lo && dg));
}

/* ---------------------------------------------------------------------------
 * Steps
 * ------------------------------------------------------------------------- */

static bool step_verify(prov_t *p)
{
    step("Checking your Twilio account...");
    snprintf(p->url, sizeof(p->url), API "%s.json", p->sid);
    if (!get(p, p->url)) {
        if (p->status == 401 || p->status == 404) {
            return fail(p, "Twilio did not accept the Account SID and Auth Token.",
                        "Copy both again from the Twilio Console home page (Account Info). "
                        "The Auth Token is hidden until you click Show.");
        }
        return fail_api(p, "Checking the account");
    }
    char status[24] = "", type[24] = "";
    copy_field(p->json, "status", status, sizeof(status));
    copy_field(p->json, "type", type, sizeof(type));
    if (strcmp(status, "active") != 0) {
        return fail(p, "This Twilio account is not active.",
                    "Log in to the Twilio Console and check the account status and billing.");
    }
    step("Account OK%s", strcmp(type, "Trial") == 0 ? " (trial account: calls only go to verified numbers)" : "");
    return true;
}

static bool step_number(prov_t *p)
{
    step("Looking for your Twilio phone number...");
    snprintf(p->url, sizeof(p->url), API "%s/IncomingPhoneNumbers.json?PageSize=100", p->sid);
    if (!get(p, p->url)) {
        return fail_api(p, "Listing phone numbers");
    }
    const cJSON *arr = list_of(p);
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        const cJSON *caps = cJSON_GetObjectItem(it, "capabilities");
        if (cJSON_IsTrue(cJSON_GetObjectItem(caps, "voice")) &&
            copy_field(it, "phone_number", p->caller_id, sizeof(p->caller_id))) {
            step("Found your number ...%s", last4(p->caller_id));
            return true;
        }
    }
    return fail(p, "Your Twilio account has no phone number that can make calls.",
                "Buy a phone number in Twilio first: Console > Phone Numbers > Manage > Buy a number "
                "(pick one with Voice). Then press Connect again.");
}

void twilio_sip_username(char *out, size_t len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, len, APP_TW_SIP_USER_PREFIX "%02x%02x%02x", mac[3], mac[4], mac[5]);
}

static bool step_credentials(prov_t *p)
{
    step("Creating the phone's private login...");
    snprintf(p->url, sizeof(p->url), API "%s/SIP/CredentialLists.json?PageSize=100", p->sid);
    if (!get(p, p->url)) {
        return fail_api(p, "Listing credential lists");
    }
    const cJSON *cl = find_by(p, "friendly_name", APP_TW_NAME);
    if (cl) {
        copy_field(cl, "sid", p->cl_sid, sizeof(p->cl_sid));
    } else {
        p->body[0] = '\0';
        form_add(p->body, sizeof(p->body), "FriendlyName", APP_TW_NAME);
        if (!post_form(p, p->url, p->body) || !copy_field(p->json, "sid", p->cl_sid, sizeof(p->cl_sid))) {
            return fail_api(p, "Creating the credential list");
        }
    }

    snprintf(p->url, sizeof(p->url), API "%s/SIP/CredentialLists/%s/Credentials.json?PageSize=100", p->sid,
             p->cl_sid);
    if (!get(p, p->url)) {
        return fail_api(p, "Listing credentials");
    }
    /* Our own credential, and logins left behind by older firmware
     * ("handset1"): those are removed so only one password per name exists.
     * Other kidphone-* credentials are other phones on this account. */
    char legacy[4][40];
    int nlegacy = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, list_of(p)) {
        const cJSON *u = cJSON_GetObjectItem(it, "username");
        if (!cJSON_IsString(u)) {
            continue;
        }
        if (strcmp(u->valuestring, p->sip_user) == 0) {
            copy_field(it, "sid", p->cr_sid, sizeof(p->cr_sid));
        } else if (strncmp(u->valuestring, APP_TW_LEGACY_SIP_USER, strlen(APP_TW_LEGACY_SIP_USER)) == 0 &&
                   nlegacy < 4) {
            copy_field(it, "sid", legacy[nlegacy], sizeof(legacy[nlegacy]));
            nlegacy++;
        }
    }
    for (int i = 0; i < nlegacy; i++) {
        if (!legacy[i][0]) {
            continue;
        }
        snprintf(p->url, sizeof(p->url), API "%.34s/SIP/CredentialLists/%.34s/Credentials/%.34s.json", p->sid,
                 p->cl_sid, legacy[i]);
        if (del(p, p->url) || p->status == 404) {
            step("Removed the old login \"" APP_TW_LEGACY_SIP_USER "\" from earlier firmware");
        } else {
            return fail_api(p, "Removing the old login");
        }
    }

    gen_password(p->sip_pass, sizeof(p->sip_pass));
    p->body[0] = '\0';
    form_add(p->body, sizeof(p->body), "Password", p->sip_pass);
    bool ok;
    if (p->cr_sid[0]) {
        /* Existing password cannot be read back: set a fresh one. */
        snprintf(p->url, sizeof(p->url), API "%s/SIP/CredentialLists/%s/Credentials/%s.json", p->sid, p->cl_sid,
                 p->cr_sid);
        ok = post_form(p, p->url, p->body);
    } else {
        form_add(p->body, sizeof(p->body), "Username", p->sip_user);
        snprintf(p->url, sizeof(p->url), API "%s/SIP/CredentialLists/%s/Credentials.json", p->sid, p->cl_sid);
        ok = post_form(p, p->url, p->body);
    }
    mbedtls_platform_zeroize(p->body, sizeof(p->body));
    if (!ok) {
        return fail_api(p, "Saving the phone's login");
    }
    step("Login ready (user %s)", p->sip_user);
    return true;
}

/* Collect credential lists mapped on the domain other than ours, from the
 * mapping list in p->json (both the Auth/Calls and the legacy mapping APIs). */
static void collect_other_lists(prov_t *p)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, list_of(p)) {
        char sid[40];
        if (!copy_field(it, "sid", sid, sizeof(sid)) || strcmp(sid, p->cl_sid) == 0) {
            continue;
        }
        bool dup = false;
        for (int i = 0; i < p->n_other_lists; i++) {
            dup |= strcmp(p->other_lists[i], sid) == 0;
        }
        if (!dup && p->n_other_lists < 4) {
            strlcpy(p->other_lists[p->n_other_lists++], sid, sizeof(p->other_lists[0]));
        }
    }
}

/* Twilio checks the password of the first credential it finds with the
 * username, in any list mapped on the domain. A second list holding the same
 * username makes calls fail with 403 after a correct-looking login. */
static void warn_duplicate_logins(prov_t *p)
{
    for (int i = 0; i < p->n_other_lists; i++) {
        snprintf(p->url, sizeof(p->url), API "%.34s/SIP/CredentialLists/%.34s/Credentials.json?PageSize=100",
                 p->sid, p->other_lists[i]);
        if (!get(p, p->url)) {
            continue; /* a warning only: never fail setup over it */
        }
        if (find_by(p, "username", p->sip_user)) {
            step("Warning: another credential list on this SIP domain (%.34s) also has the login %s. Twilio may "
                 "check the wrong password. Remove it there or unmap that list.",
                 p->other_lists[i], p->sip_user);
        }
    }
}

static bool step_domain(prov_t *p)
{
    step("Setting up the phone's calling address...");
    snprintf(p->url, sizeof(p->url), API "%s/SIP/Domains.json?PageSize=100", p->sid);
    if (!get(p, p->url)) {
        return fail_api(p, "Listing SIP domains");
    }
    const cJSON *d = find_by(p, "friendly_name", APP_TW_NAME);
    if (d) {
        copy_field(d, "sid", p->sd_sid, sizeof(p->sd_sid));
        copy_field(d, "domain_name", p->domain, sizeof(p->domain));
    } else {
        /* Domain names are global across Twilio: retry on a name clash. */
        for (int attempt = 0; attempt < 3 && !p->sd_sid[0]; attempt++) {
            snprintf(p->domain, sizeof(p->domain), "kidphone-%06" PRIx32 ".sip.twilio.com",
                     esp_random() & 0xffffff);
            p->body[0] = '\0';
            form_add(p->body, sizeof(p->body), "DomainName", p->domain);
            form_add(p->body, sizeof(p->body), "FriendlyName", APP_TW_NAME);
            if (post_form(p, p->url, p->body)) {
                copy_field(p->json, "sid", p->sd_sid, sizeof(p->sd_sid));
            } else if (p->status != 400 && p->status != 409) {
                break;
            }
        }
        if (!p->sd_sid[0]) {
            return fail_api(p, "Creating the SIP domain");
        }
    }
    if (!p->sd_sid[0] || !p->domain[0]) {
        return fail(p, "Twilio returned an incomplete SIP domain.", "Press Connect again to retry.");
    }

    /* Credential list for call authentication (no registration needed). */
    snprintf(p->url, sizeof(p->url), API "%s/SIP/Domains/%s/Auth/Calls/CredentialListMappings.json", p->sid,
             p->sd_sid);
    if (!get(p, p->url)) {
        return fail_api(p, "Reading the domain's call authentication");
    }
    bool mapped = p->resp && strstr(p->resp, p->cl_sid);
    collect_other_lists(p);
    if (!mapped) {
        p->body[0] = '\0';
        form_add(p->body, sizeof(p->body), "CredentialListSid", p->cl_sid);
        if (!post_form(p, p->url, p->body)) {
            return fail_api(p, "Linking the login to the SIP domain");
        }
    }
    /* Lists mapped through the older (non Auth/Calls) mapping API count too. */
    snprintf(p->url, sizeof(p->url), API "%s/SIP/Domains/%s/CredentialListMappings.json", p->sid, p->sd_sid);
    if (get(p, p->url)) {
        collect_other_lists(p);
    }
    warn_duplicate_logins(p);
    step("Calling address ready: %s", p->domain);
    return true;
}

/* multipart/form-data upload of voice.js as a new function version. */
static bool upload_function(prov_t *p)
{
    static const char B[] = "----kidphoneB0undary7c41";
    const size_t js_len = (size_t)(voice_js_end - voice_js_start) - 1; /* minus NUL */
    const size_t cap = js_len + 1024;
    char *body = malloc(cap);
    if (!body) {
        return fail(p, "Out of memory while uploading.", "Restart the phone and try again.");
    }
    int n = snprintf(body, cap,
                     "--%s\r\nContent-Disposition: form-data; name=\"Path\"\r\n\r\n/voice\r\n"
                     "--%s\r\nContent-Disposition: form-data; name=\"Visibility\"\r\n\r\nprotected\r\n"
                     "--%s\r\nContent-Disposition: form-data; name=\"Content\"; filename=\"voice.js\"\r\n"
                     "Content-Type: application/javascript\r\n\r\n",
                     B, B, B);
    memcpy(body + n, voice_js_start, js_len);
    n += (int)js_len;
    n += snprintf(body + n, cap - n, "\r\n--%s--\r\n", B);
    char ctype[80];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", B);
    snprintf(p->url, sizeof(p->url), SLS_UP "/%s/Functions/%s/Versions", p->svc_sid, p->fn_sid);
    bool ok = http(p, HTTP_METHOD_POST, p->url, ctype, body, n) &&
              copy_field(p->json, "sid", p->fnv_sid, sizeof(p->fnv_sid));
    free(body);
    return ok ? true : fail_api(p, "Uploading the call program");
}

/* GET list at p->url, find by key == name, else POST form; sid into out. */
static bool find_or_create(prov_t *p, const char *key, const char *name, const char *form, char *out,
                           size_t out_len, const char *what)
{
    if (!get(p, p->url)) {
        return fail_api(p, what);
    }
    const cJSON *it = find_by(p, key, name);
    if (it && copy_field(it, "sid", out, out_len)) {
        return true;
    }
    if (!post_form(p, p->url, form) || !copy_field(p->json, "sid", out, out_len)) {
        return fail_api(p, what);
    }
    return true;
}

static bool set_variable(prov_t *p, const char *key, const char *value)
{
    snprintf(p->url, sizeof(p->url), SLS "/%s/Environments/%s/Variables?PageSize=100", p->svc_sid, p->env_sid);
    if (!get(p, p->url)) {
        return fail_api(p, "Reading settings");
    }
    char vsid[40] = "";
    const cJSON *it = find_by(p, "key", key);
    if (it) {
        copy_field(it, "sid", vsid, sizeof(vsid));
    }
    p->body[0] = '\0';
    form_add(p->body, sizeof(p->body), "Value", value);
    if (vsid[0]) {
        snprintf(p->url, sizeof(p->url), SLS "/%s/Environments/%s/Variables/%s", p->svc_sid, p->env_sid, vsid);
    } else {
        form_add(p->body, sizeof(p->body), "Key", key);
        snprintf(p->url, sizeof(p->url), SLS "/%s/Environments/%s/Variables", p->svc_sid, p->env_sid);
    }
    if (!post_form(p, p->url, p->body)) {
        return fail_api(p, "Saving settings");
    }
    return true;
}

/* ALLOWED: comma separated numbers of all used contact slots. */
static void allowed_list(const prov_t *p, char *out, size_t len)
{
    out[0] = '\0';
    for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
        if (!p->contacts[i].number[0]) {
            continue;
        }
        if (out[0]) {
            strlcat(out, ",", len);
        }
        strlcat(out, p->contacts[i].number, len);
    }
}

static bool step_serverless(prov_t *p)
{
    step("Installing the call program in your Twilio account...");

    char form[160] = "";
    form_add(form, sizeof(form), "UniqueName", APP_TW_NAME);
    form_add(form, sizeof(form), "FriendlyName", APP_DEVICE_NAME);
    snprintf(p->url, sizeof(p->url), SLS "?PageSize=100");
    if (!find_or_create(p, "unique_name", APP_TW_NAME, form, p->svc_sid, sizeof(p->svc_sid), "Creating the service")) {
        return false;
    }

    form[0] = '\0';
    form_add(form, sizeof(form), "UniqueName", APP_TW_ENV_NAME);
    form_add(form, sizeof(form), "DomainSuffix", APP_TW_ENV_NAME);
    snprintf(p->url, sizeof(p->url), SLS "/%s/Environments?PageSize=100", p->svc_sid);
    if (!get(p, p->url)) {
        return fail_api(p, "Listing environments");
    }
    const cJSON *env = find_by(p, "unique_name", APP_TW_ENV_NAME);
    if (env) {
        copy_field(env, "sid", p->env_sid, sizeof(p->env_sid));
        copy_field(env, "domain_name", p->env_domain, sizeof(p->env_domain));
    } else if (post_form(p, p->url, form)) {
        copy_field(p->json, "sid", p->env_sid, sizeof(p->env_sid));
        copy_field(p->json, "domain_name", p->env_domain, sizeof(p->env_domain));
    }
    if (!p->env_sid[0] || !p->env_domain[0]) {
        return fail_api(p, "Creating the environment");
    }

    form[0] = '\0';
    form_add(form, sizeof(form), "FriendlyName", APP_TW_FN_NAME);
    snprintf(p->url, sizeof(p->url), SLS "/%s/Functions?PageSize=100", p->svc_sid);
    if (!find_or_create(p, "friendly_name", APP_TW_FN_NAME, form, p->fn_sid, sizeof(p->fn_sid),
                        "Creating the function")) {
        return false;
    }

    if (!upload_function(p)) {
        return false;
    }

    step("Building the call program (this can take a minute)...");
    p->body[0] = '\0';
    form_add(p->body, sizeof(p->body), "FunctionVersions", p->fnv_sid);
    form_add(p->body, sizeof(p->body), "Dependencies", APP_PROV_FN_DEPS);
    snprintf(p->url, sizeof(p->url), SLS "/%s/Builds", p->svc_sid);
    if (!post_form(p, p->url, p->body) || !copy_field(p->json, "sid", p->build_sid, sizeof(p->build_sid))) {
        return fail_api(p, "Starting the build");
    }
    snprintf(p->url, sizeof(p->url), SLS "/%s/Builds/%s", p->svc_sid, p->build_sid);
    int waited = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(APP_PROV_BUILD_POLL_MS));
        waited += APP_PROV_BUILD_POLL_MS;
        char status[24] = "";
        if (get(p, p->url)) {
            copy_field(p->json, "status", status, sizeof(status));
        }
        if (strcmp(status, "completed") == 0) {
            break;
        }
        if (strcmp(status, "failed") == 0) {
            return fail(p, "Twilio could not build the call program.",
                        "Press Connect again. If it keeps failing, look at Console > Functions and Assets > "
                        "Services > kid-phone for the build log.");
        }
        if (waited >= APP_PROV_BUILD_TIMEOUT_MS) {
            return fail(p, "The build took too long.", "Twilio may be slow right now. Press Connect again in a few minutes.");
        }
    }
    step("Build finished");

    /* Environment variables: who may be called, and the number calls come from. */
    char allowed[CFG_MAX_CONTACTS * 21];
    allowed_list(p, allowed, sizeof(allowed));
    if (!set_variable(p, "CALLER_ID", p->caller_id) || !set_variable(p, "ALLOWED", allowed)) {
        return false;
    }

    step("Switching it on...");
    p->body[0] = '\0';
    form_add(p->body, sizeof(p->body), "BuildSid", p->build_sid);
    snprintf(p->url, sizeof(p->url), SLS "/%s/Environments/%s/Deployments", p->svc_sid, p->env_sid);
    if (!post_form(p, p->url, p->body)) {
        return fail_api(p, "Deploying the call program");
    }
    step("Call program live at %s", p->env_domain);
    return true;
}

static bool step_voice_url(prov_t *p)
{
    step("Connecting the calling address to the call program...");
    char vurl[160];
    snprintf(vurl, sizeof(vurl), "https://%s/voice", p->env_domain);
    p->body[0] = '\0';
    form_add(p->body, sizeof(p->body), "VoiceUrl", vurl);
    form_add(p->body, sizeof(p->body), "VoiceMethod", "POST");
    snprintf(p->url, sizeof(p->url), API "%s/SIP/Domains/%s.json", p->sid, p->sd_sid);
    if (!post_form(p, p->url, p->body)) {
        return fail_api(p, "Setting the SIP domain voice URL");
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Task
 * ------------------------------------------------------------------------- */

/*
 * Numbers-only update: find service and environment, set ALLOWED, redeploy
 * the build that is live now. Returns 1 done, 0 failed (error set), -1 fall
 * back to the full setup (something the quick path needs is missing).
 */
static int quick_numbers(prov_t *p)
{
    if (!step_verify(p)) {
        return 0;
    }
    step("Updating who the phone may call...");
    snprintf(p->url, sizeof(p->url), SLS "?PageSize=100");
    if (!get(p, p->url)) {
        (void)fail_api(p, "Finding the call program");
        return 0;
    }
    const cJSON *svc = find_by(p, "unique_name", APP_TW_NAME);
    if (!svc || !copy_field(svc, "sid", p->svc_sid, sizeof(p->svc_sid))) {
        return -1;
    }
    snprintf(p->url, sizeof(p->url), SLS "/%s/Environments?PageSize=100", p->svc_sid);
    if (!get(p, p->url)) {
        (void)fail_api(p, "Finding the call program");
        return 0;
    }
    const cJSON *env = find_by(p, "unique_name", APP_TW_ENV_NAME);
    if (!env || !copy_field(env, "sid", p->env_sid, sizeof(p->env_sid)) ||
        !copy_field(env, "build_sid", p->build_sid, sizeof(p->build_sid))) {
        return -1;
    }
    char allowed[CFG_MAX_CONTACTS * 21];
    allowed_list(p, allowed, sizeof(allowed));
    if (!set_variable(p, "ALLOWED", allowed)) {
        return 0;
    }
    /* Variables only take effect with a new deployment. */
    p->body[0] = '\0';
    form_add(p->body, sizeof(p->body), "BuildSid", p->build_sid);
    snprintf(p->url, sizeof(p->url), SLS "/%s/Environments/%s/Deployments", p->svc_sid, p->env_sid);
    if (!post_form(p, p->url, p->body)) {
        (void)fail_api(p, "Deploying the call program");
        return 0;
    }
    if (config_set_contacts(p->contacts) != ESP_OK) {
        (void)fail(p, "Could not save the settings on the phone.", "Restart the phone and try again.");
        return 0;
    }
    step("Numbers updated. Twilio takes a few seconds to switch over.");
    return 1;
}

static bool full_setup(prov_t *p)
{
    bool ok = step_verify(p) && step_number(p) && step_credentials(p) && step_domain(p) && step_serverless(p) &&
              step_voice_url(p);
    clear_resp(p);
    if (!ok) {
        return false;
    }
    if (config_set_twilio(p->sid, p->domain, p->sip_user, p->sip_pass, p->caller_id, p->contacts) != ESP_OK) {
        return fail(p, "Could not save the settings on the phone.", "Restart the phone and run setup again.");
    }
    step("All set! Pick up the handset: you should hear a dial tone.");
    return true;
}

static void prov_task(void *arg)
{
    prov_t *p = arg;
    bool ok;
    int q = p->numbers_only ? quick_numbers(p) : -1;
    if (q < 0) {
        if (p->numbers_only) {
            step("Running the full setup instead...");
        }
        ok = full_setup(p);
    } else {
        ok = q == 1;
    }
    clear_resp(p);
    /* The token (inside p->auth) and the SIP password go away here. */
    mbedtls_platform_zeroize(p, sizeof(*p));
    free(p);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = ok ? ST_DONE : ST_FAILED;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "provisioning %s, free heap %u", ok ? "done" : "failed", (unsigned)esp_get_free_heap_size());
    vTaskDelete(NULL);
}

bool twilio_provision_start(const char *account_sid, const char *auth_token, const cfg_contact_t *contacts,
                            bool numbers_only)
{
    ensure_lock();
    if (!contacts || !config_any_contact(contacts) || !account_sid || !auth_token) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state == ST_RUNNING) {
        xSemaphoreGive(s_lock);
        return false;
    }
    s_state = ST_RUNNING;
    s_nsteps = 0;
    s_error[0] = s_hint[0] = '\0';
    xSemaphoreGive(s_lock);

    prov_t *p = calloc(1, sizeof(prov_t));
    if (!p) {
        s_state = ST_FAILED;
        return false;
    }
    strlcpy(p->sid, account_sid, sizeof(p->sid));
    char up[128];
    int ul = snprintf(up, sizeof(up), "%s:%s", account_sid, auth_token);
    size_t olen = 0;
    memcpy(p->auth, "Basic ", 6);
    mbedtls_base64_encode((unsigned char *)p->auth + 6, sizeof(p->auth) - 7, &olen, (const unsigned char *)up,
                          (size_t)ul);
    p->auth[6 + olen] = '\0';
    mbedtls_platform_zeroize(up, sizeof(up));
    memcpy(p->contacts, contacts, sizeof(p->contacts));
    p->numbers_only = numbers_only;
    twilio_sip_username(p->sip_user, sizeof(p->sip_user));

    if (xTaskCreatePinnedToCore(prov_task, "provision", APP_PROV_STACK, p, 4, NULL, 0) != pdPASS) {
        mbedtls_platform_zeroize(p, sizeof(*p));
        free(p);
        s_state = ST_FAILED;
        return false;
    }
    return true;
}
