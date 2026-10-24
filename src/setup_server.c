#include "setup_server.h"

#include <ctype.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "mdns.h"

#include "app_config.h"
#include "config_store.h"
#include "modes.h"
#include "schedule.h"
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

static cJSON *contact_json(const cfg_contact_t *ct)
{
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    cJSON *ci = cJSON_CreateObject();
    cJSON_AddStringToObject(ci, "name", ct->name);
    cJSON_AddStringToObject(ci, "number", ct->number);
    const cfg_schedule_t *sc = &ct->sched;
    cJSON *js = cJSON_AddObjectToObject(ci, "schedule");
    cJSON_AddBoolToObject(js, "enabled", sc->enabled);
    cJSON_AddStringToObject(js, "tz", sc->tz);
    cJSON_AddStringToObject(js, "tz_name", sc->tz_name);
    cJSON *wins = cJSON_AddArrayToObject(js, "windows");
    for (int w = 0; w < sc->nwin && w < CFG_MAX_WINDOWS; w++) {
        cJSON *jw = cJSON_CreateObject();
        cJSON_AddNumberToObject(jw, "days", sc->win[w].days);
        cJSON_AddNumberToObject(jw, "start", sc->win[w].start_min);
        cJSON_AddNumberToObject(jw, "end", sc->win[w].end_min);
        cJSON_AddItemToArray(wins, jw);
    }
    sched_state_t st;
    schedule_eval(sc, &st);
    cJSON_AddBoolToObject(ci, "available", ct->number[0] && st.available);
    cJSON_AddBoolToObject(ci, "scheduled", st.scheduled);
    if (st.has_next) {
        char hm[16];
        const unsigned nm = (unsigned)st.next_minute % 1440u;
        snprintf(hm, sizeof(hm), "%02u:%02u", nm / 60u, nm % 60u);
        cJSON *n = cJSON_AddObjectToObject(ci, "next");
        cJSON_AddStringToObject(n, "day", days[st.next_wday]);
        cJSON_AddStringToObject(n, "time", hm);
    }
    return ci;
}

/* WiFi state plus the saved networks (SSIDs only, never a password). */
static void wifi_json(cJSON *w)
{
    char ssid[33], ip[16] = "";
    int rssi = 0;
    bool up = wifi_current(ssid, sizeof(ssid), &rssi);
    cJSON_AddBoolToObject(w, "connected", up);
    cJSON_AddStringToObject(w, "state", wifi_state_name());
    cJSON_AddStringToObject(w, "ssid", ssid);
    if (up) {
        cJSON_AddNumberToObject(w, "rssi", rssi);
    }
    if (wifi_get_ip(ip, sizeof(ip))) {
        cJSON_AddStringToObject(w, "ip", ip);
    }
    cfg_wifi_info_t nets[CFG_MAX_WIFI];
    int n = config_wifi_list(nets, CFG_MAX_WIFI);
    cJSON_AddNumberToObject(w, "max", CFG_MAX_WIFI);
    const uint32_t now = wifi_uptime_s();
    cJSON_AddNumberToObject(w, "uptime_s", now);
    cJSON *arr = cJSON_AddArrayToObject(w, "saved");
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", nets[i].ssid);
        cJSON_AddBoolToObject(e, "secured", nets[i].secured);
        cJSON_AddBoolToObject(e, "connected", up && strcmp(nets[i].ssid, ssid) == 0);
        wifi_net_info_t ni;
        bool scanned = false;
        wifi_net_info(nets[i].ssid, &ni, &scanned);
        if (scanned) {
            cJSON_AddBoolToObject(e, "in_last_scan", ni.in_scan);
        }
        if (ni.ever_seen) {
            cJSON_AddNumberToObject(e, "last_rssi", ni.rssi);
            cJSON_AddNumberToObject(e, "last_seen_ago_s", now - ni.seen_s);
        }
        if (ni.tried) {
            char txt[64];
            wifi_result_text(ni.result, ni.reason, txt, sizeof(txt));
            cJSON_AddStringToObject(e, "last_result", wifi_result_key(ni.result));
            cJSON_AddStringToObject(e, "last_text", txt);
            cJSON_AddNumberToObject(e, "last_reason", ni.reason);
            cJSON_AddNumberToObject(e, "last_try_ago_s", now - ni.try_s);
        }
        cJSON_AddItemToArray(arr, e);
    }
    wifi_attempt_t att[8];
    int na = wifi_attempts(att, 8);
    cJSON *aa = cJSON_AddArrayToObject(w, "attempts");
    for (int i = 0; i < na; i++) {
        char txt[64];
        wifi_result_text(att[i].result, att[i].reason, txt, sizeof(txt));
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", att[i].ssid);
        if (att[i].seen) {
            cJSON_AddNumberToObject(e, "rssi", att[i].rssi);
        }
        cJSON_AddStringToObject(e, "result", wifi_result_key(att[i].result));
        cJSON_AddStringToObject(e, "text", txt);
        cJSON_AddNumberToObject(e, "reason", att[i].reason);
        cJSON_AddNumberToObject(e, "at_s", att[i].at_s);
        cJSON_AddNumberToObject(e, "ago_s", now - att[i].at_s);
        if (att[i].improv) {
            cJSON_AddBoolToObject(e, "improv", true);
        }
        cJSON_AddItemToArray(aa, e);
    }
}

static esp_err_t h_status(httpd_req_t *req)
{
    if (!host_ok(req)) {
        return send_msg(req, "403 Forbidden", false, "bad host");
    }
    /* Setup state before the contacts: a run saves the contacts before it
     * reports done, so "done" here always comes with the new contacts. */
    cJSON *setup = cJSON_CreateObject();
    twilio_provision_status_json(setup);
    app_cfg_t c;
    config_get(&c);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "firmware", esp_app_get_description()->version);

    wifi_json(cJSON_AddObjectToObject(o, "wifi"));

    /* Numbers are the parent's own data on their own network; secrets never. */
    cJSON *t = cJSON_AddObjectToObject(o, "twilio");
    cJSON_AddBoolToObject(t, "provisioned", c.provisioned);
    cJSON_AddStringToObject(t, "caller_id", c.caller_id);
    cJSON_AddStringToObject(t, "sip_domain", c.sip_domain);
    cJSON_AddStringToObject(t, "account_sid", c.account_sid);
    cJSON_AddStringToObject(t, "sip_user", c.sip_user);
    char want_user[33];
    twilio_sip_username(want_user, sizeof(want_user));
    /* Set up by older firmware (shared "handset1" login): a full setup run
     * moves it to its own login. */
    cJSON_AddBoolToObject(t, "login_current", strcmp(c.sip_user, want_user) == 0);
    /* Set up before the current call program (twilio/voice.js and its voice
     * clip): only a full setup run installs the new one. */
    cJSON_AddBoolToObject(t, "program_current", c.call_program >= APP_TW_PROGRAM_VERSION);
    /* Only whether the restricted numbers key exists; never the key itself. */
    cJSON_AddBoolToObject(t, "numbers_key",
                          c.key_sid[0] && c.key_secret[0] && c.svc_sid[0] && c.env_sid[0]);
    cJSON *contacts = cJSON_AddArrayToObject(t, "contacts");
    for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
        cJSON_AddItemToArray(contacts, contact_json(&c.contacts[i]));
    }
    cJSON *set = cJSON_AddObjectToObject(o, "settings");
    cJSON_AddNumberToObject(set, "max_call_minutes", c.max_call_min);
    cJSON_AddNumberToObject(set, "max_call_minutes_lo", APP_CALL_MAX_MIN_LO);
    cJSON_AddNumberToObject(set, "max_call_minutes_hi", APP_CALL_MAX_MIN_HI);
    config_wipe(&c);
    cJSON_AddBoolToObject(o, "time_synced", schedule_time_synced());

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
    cJSON_AddNumberToObject(last, "contact", r.contact);
    cJSON_AddStringToObject(last, "reason", r.reason);
    cJSON_AddStringToObject(last, "kind", r.kind);
    cJSON_AddStringToObject(last, "message", r.message);
    cJSON_AddNumberToObject(call, "contact", sip_call_contact_index());

    cJSON_AddItemToObject(o, "setup", setup);
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

/* "HH:MM" or a number of minutes. */
static int json_minutes(const cJSON *v)
{
    if (cJSON_IsNumber(v)) {
        return v->valueint;
    }
    if (cJSON_IsString(v)) {
        int h, m;
        if (sscanf(v->valuestring, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
            return h * 60 + m;
        }
    }
    return -1;
}

/*
 * contacts: [{name, number, schedule: {enabled, tz, tz_name, windows: [{days,
 * start, end}]}}, ...], up to CFG_MAX_CONTACTS slots in button order. Slots
 * not given, or with an empty number, are unused. Returns NULL if fine, else
 * a message for the parent.
 */
static const char *parse_contacts(const cJSON *arr, cfg_contact_t *out, char *err, size_t err_len)
{
    memset(out, 0, sizeof(cfg_contact_t) * CFG_MAX_CONTACTS);
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) > CFG_MAX_CONTACTS) {
        return "Bad contact list.";
    }
    int i = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        cfg_contact_t *ct = &out[i];
        const char *num = json_str(it, "number");
        trim_copy(json_str(it, "name"), ct->name, sizeof(ct->name));
        char trimmed[32];
        trim_copy(num, trimmed, sizeof(trimmed));
        if (trimmed[0] && !normalize_number(trimmed, ct->number, sizeof(ct->number))) {
            snprintf(err, err_len,
                     "Button %d: write the phone number in international format, starting with + and the country "
                     "code, like +1 555 123 4567.", i + 1);
            return err;
        }
        if (ct->number[0] && !ct->name[0]) {
            snprintf(ct->name, sizeof(ct->name), "Contact %d", i + 1);
        }
        const cJSON *js = cJSON_GetObjectItem(it, "schedule");
        cfg_schedule_t *sc = &ct->sched;
        if (cJSON_IsObject(js)) {
            sc->enabled = cJSON_IsTrue(cJSON_GetObjectItem(js, "enabled"));
            strlcpy(sc->tz, json_str(js, "tz"), sizeof(sc->tz));
            strlcpy(sc->tz_name, json_str(js, "tz_name"), sizeof(sc->tz_name));
            const cJSON *wins = cJSON_GetObjectItem(js, "windows");
            const cJSON *w;
            cJSON_ArrayForEach(w, wins) {
                if (sc->nwin == CFG_MAX_WINDOWS) {
                    break;
                }
                const cJSON *d = cJSON_GetObjectItem(w, "days");
                int start = json_minutes(cJSON_GetObjectItem(w, "start"));
                int end = json_minutes(cJSON_GetObjectItem(w, "end"));
                if (!cJSON_IsNumber(d) || start < 0 || start >= 1440 || end < 0 || end >= 1440) {
                    snprintf(err, err_len, "Button %d: a time is not valid.", i + 1);
                    return err;
                }
                sc->win[sc->nwin++] = (cfg_window_t){
                    .days = (uint8_t)(d->valueint & 0x7f), .start_min = (uint16_t)start, .end_min = (uint16_t)end};
            }
        }
        const char *why = ct->number[0] ? schedule_validate(sc) : NULL;
        if (why) {
            snprintf(err, err_len, "%s: %s.", ct->name, why);
            return err;
        }
        i++;
    }
    if (!config_any_contact(out)) {
        return "Add at least one person to call, with their phone number.";
    }
    return NULL;
}

/* Same set of numbers (order and duplicates ignored)? */
static bool same_numbers(const cfg_contact_t *a, const cfg_contact_t *b)
{
    for (int pass = 0; pass < 2; pass++) {
        const cfg_contact_t *x = pass ? b : a, *y = pass ? a : b;
        for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
            if (!x[i].number[0]) {
                continue;
            }
            bool found = false;
            for (int j = 0; j < CFG_MAX_CONTACTS && !found; j++) {
                found = strcmp(x[i].number, y[j].number) == 0;
            }
            if (!found) {
                return false;
            }
        }
    }
    return true;
}

static cJSON *read_json(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        return NULL;
    }
    size_t body_len = strlen(body);
    cJSON *j = cJSON_ParseWithLength(body, body_len);
    mbedtls_platform_zeroize(body, body_len);
    free(body);
    return j;
}

/*
 * POST /api/provision {account_sid, auth_token, contacts: [...], full}
 * (older pages send contact_name / contact_number instead of contacts).
 * If the phone is already set up with its own login on the same account and
 * full is not true, only the numbers are pushed to Twilio (quick path);
 * otherwise the full setup runs. account_sid may be left out once stored.
 * Answers 202 {ok, message, run}: progress is in /api/status setup.
 */
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
    cJSON *j = read_json(req);
    if (!j) {
        return send_msg(req, "400 Bad Request", false, "Missing, too large or bad request.");
    }

    char sid[40], token[80], errbuf[200];
    cfg_contact_t ct[CFG_MAX_CONTACTS];
    trim_copy(json_str(j, "account_sid"), sid, sizeof(sid));
    trim_copy(json_str(j, "auth_token"), token, sizeof(token));
    const bool force_full = cJSON_IsTrue(cJSON_GetObjectItem(j, "full"));
    const char *err = NULL;
    const cJSON *arr = cJSON_GetObjectItem(j, "contacts");
    if (arr) {
        err = parse_contacts(arr, ct, errbuf, sizeof(errbuf));
    } else {
        memset(ct, 0, sizeof(ct));
        trim_copy(json_str(j, "contact_name"), ct[0].name, sizeof(ct[0].name));
        if (!normalize_number(json_str(j, "contact_number"), ct[0].number, sizeof(ct[0].number))) {
            err = "Write the phone number in international format, starting with + and the country code, "
                  "like +1 555 123 4567.";
        } else if (!ct[0].name[0]) {
            strlcpy(ct[0].name, "Contact", sizeof(ct[0].name));
        }
    }
    /* Wipe the token inside the parsed tree before freeing it. */
    cJSON *tok = cJSON_GetObjectItem(j, "auth_token");
    if (cJSON_IsString(tok)) {
        mbedtls_platform_zeroize(tok->valuestring, strlen(tok->valuestring));
    }
    cJSON_Delete(j);

    if (!sid[0]) {
        app_cfg_t c;
        config_get(&c);
        strlcpy(sid, c.account_sid, sizeof(sid));
        config_wipe(&c);
    }
    bool sid_ok = strlen(sid) == 34 && sid[0] == 'A' && sid[1] == 'C';
    for (int i = 2; sid_ok && i < 34; i++) {
        sid_ok = isxdigit((unsigned char)sid[i]) != 0;
    }
    size_t tl = strlen(token);
    if (!err && !sid_ok) {
        err = "The Account SID should start with AC and be 34 characters long.";
    } else if (!err && (tl < 16 || tl > 64)) {
        err = "The Auth Token looks wrong. Click Show next to Auth Token in the Twilio Console and copy it again.";
    }

    /* Quick path only when the phone already has a working setup with its
     * own login and this is the same account. */
    app_cfg_t c;
    config_get(&c);
    char want_user[33];
    twilio_sip_username(want_user, sizeof(want_user));
    bool quick = !force_full && c.provisioned && strcmp(c.sip_user, want_user) == 0 &&
                 strcmp(c.account_sid, sid) == 0;
    config_wipe(&c);

    bool started = !err && twilio_provision_start(sid, token, ct, quick);
    mbedtls_platform_zeroize(token, sizeof(token));
    if (err) {
        return send_msg(req, "400 Bad Request", false, err);
    }
    if (!started) {
        return send_msg(req, "409 Conflict", false, "Setup could not start (already running?).");
    }
    ESP_LOGI(TAG, "%s started from the web page", quick ? "number update" : "setup");
    /* The run id lets the page tell this run's outcome from older ones. */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "message", "Setup started.");
    cJSON_AddNumberToObject(o, "run", twilio_provision_run_id());
    return send_json(req, "202 Accepted", o);
}

/*
 * POST /api/contacts {contacts: [...]}: names and schedules, no Twilio token.
 * Numbers may move between buttons freely. A changed set of numbers has to
 * reach the Twilio Function's ALLOWED list: with the stored restricted key
 * the update starts right away (202 {updating: true, run}, progress in
 * /api/status setup); without a key it answers 409 with needs_token, and the
 * page then sends the same contacts to /api/provision with the Auth Token.
 */
static esp_err_t h_contacts(httpd_req_t *req)
{
    if (!post_ok(req)) {
        return ESP_OK;
    }
    if (twilio_provision_running()) {
        return send_msg(req, "409 Conflict", false, "Setup is running. Try again when it has finished.");
    }
    cJSON *j = read_json(req);
    if (!j) {
        return send_msg(req, "400 Bad Request", false, "Missing, too large or bad request.");
    }
    cfg_contact_t ct[CFG_MAX_CONTACTS];
    char errbuf[200];
    const char *err = parse_contacts(cJSON_GetObjectItem(j, "contacts"), ct, errbuf, sizeof(errbuf));
    cJSON_Delete(j);
    if (err) {
        return send_msg(req, "400 Bad Request", false, err);
    }
    app_cfg_t c;
    config_get(&c);
    bool provisioned = c.provisioned;
    bool same = same_numbers(ct, c.contacts);
    config_wipe(&c);
    if (provisioned && !same && config_has_twilio_key()) {
        if (!wifi_is_connected()) {
            return send_msg(req, "409 Conflict", false, "The phone is not on WiFi.");
        }
        if (!twilio_provision_start_with_key(ct)) {
            return send_msg(req, "409 Conflict", false, "The update could not start. Try again.");
        }
        ESP_LOGI(TAG, "number update with the stored key started from the web page");
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", true);
        cJSON_AddBoolToObject(o, "updating", true);
        cJSON_AddNumberToObject(o, "run", twilio_provision_run_id());
        cJSON_AddStringToObject(o, "message", "Telling Twilio about the new numbers...");
        return send_json(req, "202 Accepted", o);
    }
    if (!provisioned || !same) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddBoolToObject(o, "needs_token", true);
        cJSON_AddStringToObject(o, "error", provisioned
                                                ? "The phone numbers changed, so Twilio has to be told. Enter your "
                                                  "Auth Token below."
                                                : "Connect to Twilio first.");
        return send_json(req, "409 Conflict", o);
    }
    if (config_set_contacts(ct) != ESP_OK) {
        return send_msg(req, "500 Internal Server Error", false, "Could not save. Try again.");
    }
    ESP_LOGI(TAG, "contacts and schedules saved from the web page");
    return send_msg(req, "200 OK", true, "Saved.");
}

/* POST /api/settings {max_call_minutes}: phone-only settings, no Twilio. */
static esp_err_t h_settings(httpd_req_t *req)
{
    if (!post_ok(req)) {
        return ESP_OK;
    }
    cJSON *j = read_json(req);
    if (!j) {
        return send_msg(req, "400 Bad Request", false, "Missing, too large or bad request.");
    }
    const cJSON *v = cJSON_GetObjectItem(j, "max_call_minutes");
    bool num = cJSON_IsNumber(v);
    double m = num ? v->valuedouble : 0;
    cJSON_Delete(j);
    if (!num || m != (double)(int)m || m < APP_CALL_MAX_MIN_LO || m > APP_CALL_MAX_MIN_HI) {
        char e[80];
        snprintf(e, sizeof(e), "Pick a call length from %d to %d minutes.", APP_CALL_MAX_MIN_LO, APP_CALL_MAX_MIN_HI);
        return send_msg(req, "400 Bad Request", false, e);
    }
    if (config_set_max_call_minutes((unsigned)m) != ESP_OK) {
        return send_msg(req, "500 Internal Server Error", false, "Could not save. Try again.");
    }
    ESP_LOGI(TAG, "max call length set to %d min from the web page", (int)m);
    return send_msg(req, "200 OK", true, "Saved.");
}

/* POST /api/call {contact: i}: test call, same rules as button i. */
static esp_err_t h_call(httpd_req_t *req)
{
    if (!post_ok(req)) {
        return ESP_OK;
    }
    int idx = 0;
    cJSON *j = req->content_len ? read_json(req) : NULL;
    if (j) {
        const cJSON *v = cJSON_GetObjectItem(j, "contact");
        if (cJSON_IsNumber(v)) {
            idx = v->valueint;
        }
        cJSON_Delete(j);
    }
    if (idx < 0 || idx >= CFG_MAX_CONTACTS) {
        return send_msg(req, "400 Bad Request", false, "No such button.");
    }
    if (!wifi_is_connected() || !config_is_provisioned() || twilio_provision_running()) {
        return send_msg(req, "409 Conflict", false, "The phone is not ready to call yet.");
    }
    if (sip_call_state() != CALL_IDLE) {
        return send_msg(req, "409 Conflict", false, "A call is already in progress.");
    }
    cfg_contact_t ct;
    if (!config_get_contact(idx, &ct)) {
        return send_msg(req, "409 Conflict", false, "Nobody is set up on this button.");
    }
    sched_state_t st;
    schedule_eval(&ct.sched, &st);
    if (!st.available) {
        return send_msg(req, "409 Conflict", false,
                        "They are outside their available hours, so the phone would say they can't talk right now.");
    }
    ESP_LOGI(TAG, "test call to contact %d from the web page", idx);
    sip_call_contact(idx);
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
 * WiFi networks
 * ------------------------------------------------------------------------- */

/* GET /api/wifi: what the phone is on, its signal, and the saved list. */
static esp_err_t h_wifi(httpd_req_t *req)
{
    if (!host_ok(req)) {
        return send_msg(req, "403 Forbidden", false, "bad host");
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    wifi_json(o);
    return send_json(req, "200 OK", o);
}

/* 1 to 4 bars from the signal. */
static int signal_bars(int rssi)
{
    return rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
}

/*
 * POST /api/wifi/scan {}: nearby networks, one entry per SSID (its
 * strongest access point), strongest first, hidden ones left out. The radio
 * is 2.4 GHz only. Refused during a call: the scan hops channels and would
 * pause the audio. The connection itself stays up.
 */
static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    if (!post_ok(req)) {
        return ESP_OK;
    }
    if (sip_call_state() != CALL_IDLE) {
        return send_msg(req, "409 Conflict", false, "The phone is in a call. Scan again when the call has ended.");
    }
    static wifi_ap_record_t recs[24]; /* the server runs one handler at a time */
    int n = wifi_scan(recs, 24);
    if (n < 0) {
        return send_msg(req, "503 Service Unavailable", false, "The phone could not scan right now. Try again.");
    }
    char cur[33];
    bool up = wifi_current(cur, sizeof(cur), NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON *arr = cJSON_AddArrayToObject(o, "networks");
    for (int i = 0; i < n; i++) {
        const char *ss = (const char *)recs[i].ssid;
        if (!ss[0]) {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < i && !dup; j++) {
            dup = strcmp((const char *)recs[j].ssid, ss) == 0;
        }
        if (dup) {
            continue; /* sorted strongest first: keep the first */
        }
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", ss);
        cJSON_AddNumberToObject(e, "rssi", recs[i].rssi);
        cJSON_AddNumberToObject(e, "bars", signal_bars(recs[i].rssi));
        cJSON_AddBoolToObject(e, "secured", recs[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddBoolToObject(e, "saved", config_wifi_has(ss));
        cJSON_AddBoolToObject(e, "connected", up && strcmp(ss, cur) == 0);
        cJSON_AddItemToArray(arr, e);
    }
    return send_json(req, "200 OK", o);
}

/*
 * POST /api/wifi/add {ssid, password}: save a network (same SSID = new
 * password). Does not touch the current connection: the phone uses it the
 * next time it (re)connects, or right away if it is offline.
 */
static esp_err_t h_wifi_add(httpd_req_t *req)
{
    if (!post_ok(req)) {
        return ESP_OK;
    }
    cJSON *j = read_json(req);
    if (!j) {
        return send_msg(req, "400 Bad Request", false, "Missing, too large or bad request.");
    }
    char ssid[64], pass[80];
    strlcpy(ssid, json_str(j, "ssid"), sizeof(ssid)); /* not trimmed: spaces can be part of a name */
    strlcpy(pass, json_str(j, "password"), sizeof(pass));
    const size_t sl = strlen(json_str(j, "ssid")), pl = strlen(json_str(j, "password"));
    cJSON *pw = cJSON_GetObjectItem(j, "password");
    if (cJSON_IsString(pw)) {
        mbedtls_platform_zeroize(pw->valuestring, strlen(pw->valuestring));
    }
    cJSON_Delete(j);

    const char *err = NULL;
    if (sl < 1 || sl > 32) {
        err = sl ? "That network name is too long (32 bytes at most)." : "Type the network name.";
    } else if (pl != 0 && (pl < 8 || pl > 63)) {
        err = "A WiFi password has 8 to 63 characters. Leave it empty for an open network.";
    }
    bool updated = false;
    esp_err_t e = ESP_FAIL;
    if (!err) {
        e = config_wifi_add(ssid, pass, NULL, &updated);
    }
    mbedtls_platform_zeroize(pass, sizeof(pass));
    if (err) {
        return send_msg(req, "400 Bad Request", false, err);
    }
    if (e == ESP_ERR_NO_MEM) {
        char m[120];
        snprintf(m, sizeof(m), "The phone keeps %d networks at most. Remove one first.", CFG_MAX_WIFI);
        return send_msg(req, "409 Conflict", false, m);
    }
    if (e != ESP_OK) {
        return send_msg(req, "500 Internal Server Error", false, "Could not save. Try again.");
    }
    ESP_LOGI(TAG, "WiFi network %s from the web page", updated ? "password changed" : "added");
    wifi_history_new_password(ssid);
    wifi_networks_changed();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "updated", updated);
    cJSON_AddStringToObject(o, "message", updated ? "Password updated." : "Saved.");
    return send_json(req, "200 OK", o);
}

/*
 * POST /api/wifi/remove {ssid, confirm}: forget a network. Removing the one
 * the phone is on needs confirm: true (409 needs_confirm otherwise); the
 * phone then leaves it and joins the strongest other saved network. The
 * last saved network is never removed.
 */
static esp_err_t h_wifi_remove(httpd_req_t *req)
{
    if (!post_ok(req)) {
        return ESP_OK;
    }
    cJSON *j = read_json(req);
    if (!j) {
        return send_msg(req, "400 Bad Request", false, "Missing, too large or bad request.");
    }
    char ssid[33];
    strlcpy(ssid, json_str(j, "ssid"), sizeof(ssid));
    const bool confirm = cJSON_IsTrue(cJSON_GetObjectItem(j, "confirm"));
    cJSON_Delete(j);
    if (!ssid[0] || !config_wifi_has(ssid)) {
        return send_msg(req, "404 Not Found", false, "That network is not saved on the phone.");
    }
    if (config_wifi_count() <= 1) {
        return send_msg(req, "409 Conflict", false,
                        "This is the only saved network. Without it the phone could not get back on WiFi and "
                        "this page would be unreachable. Add another network first, or use Factory reset to "
                        "start over.");
    }
    char cur[33];
    if (wifi_current(cur, sizeof(cur), NULL) && strcmp(cur, ssid) == 0 && !confirm) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddBoolToObject(o, "needs_confirm", true);
        cJSON_AddStringToObject(o, "error",
                                "The phone is on this network right now. Removing it disconnects the phone, which "
                                "then joins the strongest other saved network. This page stops working unless "
                                "your computer can reach the phone there.");
        return send_json(req, "409 Conflict", o);
    }
    esp_err_t e = config_wifi_remove(ssid);
    if (e == ESP_ERR_INVALID_STATE) {
        return send_msg(req, "409 Conflict", false, "This is the only saved network.");
    }
    if (e != ESP_OK) {
        return send_msg(req, "500 Internal Server Error", false, "Could not remove it. Try again.");
    }
    ESP_LOGI(TAG, "WiFi network removed from the web page");
    esp_err_t r = send_msg(req, "200 OK", true, "Removed.");
    wifi_networks_changed(); /* after the reply: leaving the current network waits a moment more */
    return r;
}

/* Runs the reset after the reply has had time to leave the socket. */
static void factory_reset_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    app_factory_reset("setup page");
}

/*
 * POST /api/factory_reset {confirm: "RESET"}: erase everything (WiFi,
 * Twilio, numbers key, contacts, call length) and reboot into setup. The
 * confirmation word is checked here too, not only on the page.
 */
static esp_err_t h_factory_reset(httpd_req_t *req)
{
    if (!post_ok(req)) {
        return ESP_OK;
    }
    cJSON *j = read_json(req);
    if (!j) {
        return send_msg(req, "400 Bad Request", false, "Missing, too large or bad request.");
    }
    char word[16];
    trim_copy(json_str(j, "confirm"), word, sizeof(word));
    cJSON_Delete(j);
    if (strcmp(word, "RESET") != 0) {
        return send_msg(req, "400 Bad Request", false, "Type RESET in capital letters to confirm.");
    }
    if (twilio_provision_running()) {
        return send_msg(req, "409 Conflict", false, "Setup is running. Try again when it has finished.");
    }
    static bool s_started;
    if (s_started || xTaskCreate(factory_reset_task, "freset", 4096, NULL, 5, NULL) != pdPASS) {
        return send_msg(req, s_started ? "409 Conflict" : "500 Internal Server Error", false,
                        s_started ? "The phone is already erasing." : "Could not start. Try again.");
    }
    s_started = true;
    ESP_LOGW(TAG, "factory reset requested from the web page");
    return send_msg(req, "202 Accepted", true, "Erasing. The phone restarts in a few seconds.");
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
    cfg.max_uri_handlers = 16; /* 12 registered */
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
        {.uri = "/api/contacts", .method = HTTP_POST, .handler = h_contacts},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = h_settings},
        {.uri = "/api/call", .method = HTTP_POST, .handler = h_call},
        {.uri = "/api/hangup", .method = HTTP_POST, .handler = h_hangup},
        {.uri = "/api/factory_reset", .method = HTTP_POST, .handler = h_factory_reset},
        {.uri = "/api/wifi", .method = HTTP_GET, .handler = h_wifi},
        {.uri = "/api/wifi/scan", .method = HTTP_POST, .handler = h_wifi_scan},
        {.uri = "/api/wifi/add", .method = HTTP_POST, .handler = h_wifi_add},
        {.uri = "/api/wifi/remove", .method = HTTP_POST, .handler = h_wifi_remove},
    };
    _Static_assert(sizeof(uris) / sizeof(uris[0]) <= 16, "raise max_uri_handlers");
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t e = httpd_register_uri_handler(srv, &uris[i]);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(e));
        }
    }
    ESP_LOGI(TAG, "setup page: http://%s.local/ (port %d)", APP_HOSTNAME, APP_HTTP_PORT);
}
