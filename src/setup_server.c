#include "setup_server.h"

#include <ctype.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "mbedtls/platform_util.h"
#include "mdns.h"

#include "app_config.h"
#include "config_store.h"
#include "sip.h"
#include "twilio_provision.h"
#include "usb_audio.h"
#include "wifi.h"

static const char *TAG = "setup";

extern const char page_start[] asm("_binary_setup_page_html_start");
extern const char page_end[] asm("_binary_setup_page_html_end");

/* ---------------------------------------------------------------------------
 * Request guards
 * ------------------------------------------------------------------------- */

/* Host must be our mDNS name or a dotted IPv4 literal (optionally :port). */
static bool host_ok(httpd_req_t *req)
{
    char host[64];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return true; /* HTTP/1.0 client without Host: nothing to rebind */
    }
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
    }
    if (strcasecmp(host, APP_HOSTNAME ".local") == 0 || strcasecmp(host, APP_HOSTNAME) == 0) {
        return true;
    }
    for (const char *p = host; *p; p++) {
        if (!isdigit((unsigned char)*p) && *p != '.') {
            return false;
        }
    }
    return host[0] != '\0';
}

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, s ? s : "{}");
    cJSON_free(s);
    return err;
}

static esp_err_t send_msg(httpd_req_t *req, const char *status, bool ok, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", ok);
    if (msg) {
        cJSON_AddStringToObject(o, ok ? "message" : "error", msg);
    }
    return send_json(req, status, o);
}

/* Guards for POST: Host check, JSON content type. */
static bool post_ok(httpd_req_t *req)
{
    if (!host_ok(req)) {
        send_msg(req, "403 Forbidden", false, "bad host");
        return false;
    }
    char ct[48] = "";
    httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct));
    if (strncasecmp(ct, "application/json", 16) != 0) {
        send_msg(req, "415 Unsupported Media Type", false, "send application/json");
        return false;
    }
    return true;
}

/* Read the whole body (max APP_HTTP_MAX_BODY). Caller frees (and wipes). */
static char *read_body(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > APP_HTTP_MAX_BODY) {
        return NULL;
    }
    char *b = calloc(1, req->content_len + 1);
    if (!b) {
        return NULL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, b + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            free(b);
            return NULL;
        }
        got += (size_t)r;
    }
    return b;
}

/* ---------------------------------------------------------------------------
 * Handlers
 * ------------------------------------------------------------------------- */

static esp_err_t h_page(httpd_req_t *req)
{
    if (!host_ok(req)) {
        return send_msg(req, "403 Forbidden", false, "bad host");
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    return httpd_resp_send(req, page_start, page_end - page_start - 1);
}

static esp_err_t h_status(httpd_req_t *req)
{
    if (!host_ok(req)) {
        return send_msg(req, "403 Forbidden", false, "bad host");
    }
    app_cfg_t c;
    config_get(&c);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "firmware", esp_app_get_description()->version);

    cJSON *w = cJSON_AddObjectToObject(o, "wifi");
    char ip[16] = "";
    cJSON_AddBoolToObject(w, "connected", wifi_is_connected());
    cJSON_AddStringToObject(w, "ssid", c.wifi_ssid);
    if (wifi_get_ip(ip, sizeof(ip))) {
        cJSON_AddStringToObject(w, "ip", ip);
    }

    /* Numbers are the parent's own data on their own network; secrets never. */
    cJSON *t = cJSON_AddObjectToObject(o, "twilio");
    cJSON_AddBoolToObject(t, "provisioned", c.provisioned);
    cJSON_AddStringToObject(t, "caller_id", c.caller_id);
    cJSON_AddStringToObject(t, "sip_domain", c.sip_domain);
    cJSON *contacts = cJSON_AddArrayToObject(t, "contacts");
    for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
        if (!c.contacts[i].number[0]) {
            continue;
        }
        cJSON *ci = cJSON_CreateObject();
        cJSON_AddStringToObject(ci, "name", c.contacts[i].name);
        cJSON_AddStringToObject(ci, "number", c.contacts[i].number);
        cJSON_AddItemToArray(contacts, ci);
    }
    config_wipe(&c);

    cJSON_AddBoolToObject(o, "handset", usb_audio_handset_connected());

    cJSON *call = cJSON_AddObjectToObject(o, "call");
    cJSON_AddStringToObject(call, "state", sip_call_state_name(sip_call_state()));
    sip_result_t r;
    sip_last_result(&r);
    cJSON *last = cJSON_AddObjectToObject(call, "last");
    cJSON_AddNumberToObject(last, "seq", r.seq);
    cJSON_AddBoolToObject(last, "failed", r.failed);
    cJSON_AddNumberToObject(last, "code", r.code);
    cJSON_AddBoolToObject(last, "answered", r.answered);
    cJSON_AddBoolToObject(last, "remote_hangup", r.remote_hangup);
    cJSON_AddNumberToObject(last, "talk_ms", r.talk_ms);
    cJSON_AddStringToObject(last, "reason", r.reason);

    twilio_provision_status_json(cJSON_AddObjectToObject(o, "setup"));
    return send_json(req, "200 OK", o);
}

/* Keep digits; "00" international prefix becomes "+". E.164: + and 8..15 digits. */
static bool normalize_number(const char *in, char *out, size_t len)
{
    size_t n = 0;
    const char *p = in;
    while (*p == ' ') {
        p++;
    }
    if (p[0] == '0' && p[1] == '0') {
        p += 2;
    } else if (*p == '+') {
        p++;
    } else {
        return false; /* national format: ask for the country code */
    }
    out[n++] = '+';
    for (; *p && n + 1 < len; p++) {
        if (isdigit((unsigned char)*p)) {
            out[n++] = *p;
        } else if (!strchr(" -.()/", *p)) {
            return false;
        }
    }
    out[n] = '\0';
    size_t digits = n - 1;
    return digits >= 8 && digits <= 15 && out[1] != '0';
}

static void trim_copy(const char *in, char *out, size_t len)
{
    while (*in && isspace((unsigned char)*in)) {
        in++;
    }
    strlcpy(out, in, len);
    size_t n = strlen(out);
    while (n && isspace((unsigned char)out[n - 1])) {
        out[--n] = '\0';
    }
}

static const char *json_str(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static esp_err_t h_provision(httpd_req_t *req)
{
    if (!post_ok(req)) {
        return ESP_OK;
    }
    if (!wifi_is_connected()) {
        return send_msg(req, "409 Conflict", false, "The phone is not on WiFi.");
    }
    if (twilio_provision_running()) {
        return send_msg(req, "409 Conflict", false, "Setup is already running.");
    }
    char *body = read_body(req);
    if (!body) {
        return send_msg(req, "400 Bad Request", false, "Missing or too large request.");
    }
    size_t body_len = strlen(body);
    cJSON *j = cJSON_ParseWithLength(body, body_len);
    mbedtls_platform_zeroize(body, body_len);
    free(body);
    if (!j) {
        return send_msg(req, "400 Bad Request", false, "Bad JSON.");
    }

    char sid[40], token[80];
    cfg_contact_t ct = {0};
    trim_copy(json_str(j, "account_sid"), sid, sizeof(sid));
    trim_copy(json_str(j, "auth_token"), token, sizeof(token));
    trim_copy(json_str(j, "contact_name"), ct.name, sizeof(ct.name));
    bool num_ok = normalize_number(json_str(j, "contact_number"), ct.number, sizeof(ct.number));
    /* Wipe the token inside the parsed tree before freeing it. */
    cJSON *tok = cJSON_GetObjectItem(j, "auth_token");
    if (cJSON_IsString(tok)) {
        mbedtls_platform_zeroize(tok->valuestring, strlen(tok->valuestring));
    }
    cJSON_Delete(j);

    const char *err = NULL;
    bool sid_ok = strlen(sid) == 34 && sid[0] == 'A' && sid[1] == 'C';
    for (int i = 2; sid_ok && i < 34; i++) {
        sid_ok = isxdigit((unsigned char)sid[i]) != 0;
    }
    size_t tl = strlen(token);
    if (!sid_ok) {
        err = "The Account SID should start with AC and be 34 characters long.";
    } else if (tl < 16 || tl > 64) {
        err = "The Auth Token looks wrong. Click Show next to Auth Token in the Twilio Console and copy it again.";
    } else if (!num_ok) {
        err = "Write the phone number in international format, starting with + and the country code, "
              "like +1 555 123 4567.";
    }
    if (!ct.name[0]) {
        strlcpy(ct.name, "Contact", sizeof(ct.name));
    }
    bool started = !err && twilio_provision_start(sid, token, &ct, 1);
    mbedtls_platform_zeroize(token, sizeof(token));
    if (err) {
        return send_msg(req, "400 Bad Request", false, err);
    }
    if (!started) {
        return send_msg(req, "409 Conflict", false, "Setup could not start (already running?).");
    }
    size_t nl = strlen(ct.number);
    ESP_LOGI(TAG, "setup started from the web page, contact ...%s", nl > 4 ? ct.number + nl - 4 : ct.number);
    return send_msg(req, "202 Accepted", true, "Setup started.");
}

static esp_err_t h_call(httpd_req_t *req)
{
    if (!post_ok(req)) {
        return ESP_OK;
    }
    if (!wifi_is_connected() || !config_is_provisioned() || twilio_provision_running()) {
        return send_msg(req, "409 Conflict", false, "The phone is not ready to call yet.");
    }
    if (sip_call_state() != CALL_IDLE) {
        return send_msg(req, "409 Conflict", false, "A call is already in progress.");
    }
    ESP_LOGI(TAG, "test call from the web page");
    sip_call_start();
    return send_msg(req, "202 Accepted", true, "Calling.");
}

static esp_err_t h_hangup(httpd_req_t *req)
{
    if (!post_ok(req)) {
        return ESP_OK;
    }
    sip_call_hangup();
    return send_msg(req, "200 OK", true, NULL);
}

/* ---------------------------------------------------------------------------
 * Start
 * ------------------------------------------------------------------------- */

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(APP_HOSTNAME);
    mdns_instance_name_set(APP_DEVICE_NAME);
    mdns_service_add(APP_DEVICE_NAME, "_http", "_tcp", APP_HTTP_PORT, NULL, 0);
}

void setup_server_start(void)
{
    start_mdns();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = APP_HTTP_PORT;
    cfg.stack_size = APP_HTTP_STACK;
    cfg.max_open_sockets = 5;
    cfg.lru_purge_enable = true;
    cfg.core_id = 0;
    httpd_handle_t srv = NULL;
    esp_err_t err = httpd_start(&srv, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        return;
    }
    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_page},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/provision", .method = HTTP_POST, .handler = h_provision},
        {.uri = "/api/call", .method = HTTP_POST, .handler = h_call},
        {.uri = "/api/hangup", .method = HTTP_POST, .handler = h_hangup},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }
    ESP_LOGI(TAG, "setup page: http://%s.local/ (port %d)", APP_HOSTNAME, APP_HTTP_PORT);
}
