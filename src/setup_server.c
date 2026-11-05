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
#include "pins.h"
#include "prompts.h"
#include "schedule.h"
#include "secret.h"
#include "sip.h"
#include "smtp.h"
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

static void add_hhmm(cJSON *o, const char *key, unsigned minute)
{
    char hm[16];
    minute %= 1440u;
    snprintf(hm, sizeof(hm), "%02u:%02u", minute / 60u, minute % 60u);
    cJSON_AddStringToObject(o, key, hm);
}

/* Bedtime settings plus the live state (asleep now or not). */
static void bedtime_json(cJSON *o, const cfg_bedtime_t *b)
{
    cJSON_AddBoolToObject(o, "enabled", b->enabled);
    add_hhmm(o, "start", b->start_min);
    add_hhmm(o, "end", b->end_min);
    cJSON_AddStringToObject(o, "tz", b->tz);
    cJSON_AddStringToObject(o, "tz_name", b->tz_name);
    bedtime_state_t bs;
    schedule_bedtime_eval(b, &bs);
    cJSON_AddBoolToObject(o, "scheduled", bs.scheduled);
    cJSON_AddBoolToObject(o, "active", bs.active);
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
    static const char *modes[] = {"off", "hold", "dedicated"};
    cJSON_AddStringToObject(ci, "secret", ct->secret <= CFG_SECRET_DEDICATED ? modes[ct->secret] : "off");
    cJSON_AddStringToObject(ci, "email", ct->email);
    sched_state_t st;
    schedule_eval(sc, &st);
    cJSON_AddBoolToObject(ci, "available", ct->number[0] && st.available);
    cJSON_AddBoolToObject(ci, "scheduled", st.scheduled);
    if (st.has_next) {
        cJSON *n = cJSON_AddObjectToObject(ci, "next");
        cJSON_AddStringToObject(n, "day", days[st.next_wday]);
        add_hhmm(n, "time", (unsigned)st.next_minute);
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

/* Button and light pins: {max, assignable: [gpio...], buttons: [{button,
 * lamp}]}, lamp -1 = no light. */
static void pins_json(cJSON *o, const cfg_pins_t *p)
{
    int all[APP_NUM_ASSIGNABLE];
    const int n = pins_assignable(all, APP_NUM_ASSIGNABLE);
    cJSON_AddNumberToObject(o, "max", CFG_MAX_CONTACTS);
    cJSON_AddItemToObject(o, "assignable", cJSON_CreateIntArray(all, n));
    cJSON *arr = cJSON_AddArrayToObject(o, "buttons");
    for (int i = 0; i < p->count && i < CFG_MAX_CONTACTS; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "button", p->button[i]);
        cJSON_AddNumberToObject(e, "lamp", p->lamp[i]);
        cJSON_AddItemToArray(arr, e);
    }
}

/* Secret message settings (never the password, only whether one is saved)
 * and the last secret / test email. */
static void secret_json(cJSON *o, const cfg_secret_t *s)
{
    cJSON_AddBoolToObject(o, "configured", s->host[0] && s->user[0] && s->pass[0]);
    cJSON_AddStringToObject(o, "smtp_host", s->host);
    cJSON_AddNumberToObject(o, "smtp_port", s->port);
    cJSON_AddStringToObject(o, "smtp_security", s->security ? "starttls" : "tls");
    cJSON_AddStringToObject(o, "smtp_user", s->user);
    cJSON_AddBoolToObject(o, "has_password", s->pass[0] != '\0');
    cJSON_AddStringToObject(o, "from_name", s->from_name);
    cJSON_AddStringToObject(o, "kid_name", s->kid_name);
    cJSON_AddNumberToObject(o, "max_seconds", s->max_s);
    cJSON_AddNumberToObject(o, "max_seconds_lo", APP_SECRET_MAX_S_LO);
    cJSON_AddNumberToObject(o, "max_seconds_hi", APP_SECRET_MAX_S_HI);
    cJSON_AddNumberToObject(o, "hourly_cap", s->cap_hr);
    cJSON_AddNumberToObject(o, "hourly_cap_lo", APP_SECRET_CAP_LO);
    cJSON_AddNumberToObject(o, "hourly_cap_hi", APP_SECRET_CAP_HI);
    cJSON_AddBoolToObject(o, "bedtime_ok", s->bed_ok);
    secret_status_json(o);
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
    /* Several KB with CFG_MAX_CONTACTS slots: heap, not the server stack. */
    app_cfg_t *c = malloc(sizeof(app_cfg_t));
    if (!c) {
        cJSON_Delete(setup);
        return send_msg(req, "503 Service Unavailable", false, "The phone is busy. Try again.");
    }
    config_get(c);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "firmware", esp_app_get_description()->version);

    wifi_json(cJSON_AddObjectToObject(o, "wifi"));

    /* Numbers are the parent's own data on their own network; secrets never. */
    cJSON *t = cJSON_AddObjectToObject(o, "twilio");
    cJSON_AddBoolToObject(t, "provisioned", c->provisioned);
    cJSON_AddStringToObject(t, "caller_id", c->caller_id);
    cJSON_AddStringToObject(t, "sip_domain", c->sip_domain);
    cJSON_AddStringToObject(t, "account_sid", c->account_sid);
    cJSON_AddStringToObject(t, "sip_user", c->sip_user);
    char want_user[33];
    twilio_sip_username(want_user, sizeof(want_user));
    /* Set up by older firmware (shared "handset1" login): a full setup run
     * moves it to its own login. */
    cJSON_AddBoolToObject(t, "login_current", strcmp(c->sip_user, want_user) == 0);
    /* Set up before the current call program (twilio/voice.js and its voice
     * clip): only a full setup run installs the new one. */
    cJSON_AddBoolToObject(t, "program_current", c->call_program >= APP_TW_PROGRAM_VERSION);
    /* Only whether the restricted numbers key exists; never the key itself. */
    cJSON_AddBoolToObject(t, "numbers_key",
                          c->key_sid[0] && c->key_secret[0] && c->svc_sid[0] && c->env_sid[0]);
    /* One slot per wired button (and any slot further on that still has a
     * number or a secret mode, which config_set_pins never leaves behind). */
    int nslots = c->pins.count;
    for (int i = nslots; i < CFG_MAX_CONTACTS; i++) {
        if (c->contacts[i].number[0] || c->contacts[i].secret != CFG_SECRET_OFF) {
            nslots = i + 1;
        }
    }
    cJSON *contacts = cJSON_AddArrayToObject(t, "contacts");
    for (int i = 0; i < nslots; i++) {
        cJSON_AddItemToArray(contacts, contact_json(&c->contacts[i]));
    }
    cJSON *set = cJSON_AddObjectToObject(o, "settings");
    cJSON_AddNumberToObject(set, "max_call_minutes", c->max_call_min);
    cJSON_AddNumberToObject(set, "max_call_minutes_lo", APP_CALL_MAX_MIN_LO);
    cJSON_AddNumberToObject(set, "max_call_minutes_hi", APP_CALL_MAX_MIN_HI);
    cJSON_AddStringToObject(set, "lang", prompt_lang_code((prompt_lang_t)c->lang));
    cJSON_AddNumberToObject(set, "emergency_taps", c->emerg_taps);
    cJSON_AddNumberToObject(set, "emergency_taps_lo", APP_EMERGENCY_TAPS_LO);
    cJSON_AddNumberToObject(set, "emergency_taps_hi", APP_EMERGENCY_TAPS_HI);
    cJSON_AddNumberToObject(set, "emergency_gap_ms", APP_EMERGENCY_GAP_MS);
    int em_slot;
    int64_t em_ago;
    if (app_emergency_last(&em_slot, &em_ago)) {
        cJSON *em = cJSON_AddObjectToObject(set, "emergency_last");
        cJSON_AddNumberToObject(em, "slot", em_slot);
        cJSON_AddNumberToObject(em, "ago_s", (double)(em_ago / 1000));
    }
    bedtime_json(cJSON_AddObjectToObject(o, "bedtime"), &c->bedtime);
    pins_json(cJSON_AddObjectToObject(o, "pins"), &c->pins);
    secret_json(cJSON_AddObjectToObject(o, "secret"), &c->secret);
    config_wipe(c);
    free(c);
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

/* Plain email address: one "@", a dot after it, no spaces, quotes, angle
 * brackets or control characters (it goes into SMTP commands and headers). */
static bool email_ok(const char *e)
{
    const size_t n = strlen(e);
    const char *at = strchr(e, '@');
    if (n < 6 || n > 63 || !at || at == e || strchr(at + 1, '@') || !strchr(at + 1, '.') || e[n - 1] == '.') {
        return false;
    }
    for (const char *p = e; *p; p++) {
        if ((unsigned char)*p <= ' ' || (unsigned char)*p >= 0x7f || strchr("<>()[]\\,;:\"", *p)) {
            return false;
        }
    }
    return true;
}

/*
 * contacts: [{name, number, schedule: {enabled, tz, tz_name, windows: [{days,
 * start, end}]}, secret, email}, ...], up to CFG_MAX_CONTACTS slots in button
 * order. Slots not given, or with an empty number and secret "off", are
 * unused; a number or a secret mode on a slot with no button pin (pins.h) is
 * refused. secret: "off" (or missing), "hold" (long press records, needs a
 * number) or "dedicated" (no number); either needs an email. Returns NULL if
 * fine, else a message for the parent.
 */
static const char *parse_contacts(const cJSON *arr, cfg_contact_t *out, char *err, size_t err_len)
{
    memset(out, 0, sizeof(cfg_contact_t) * CFG_MAX_CONTACTS);
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) > CFG_MAX_CONTACTS) {
        return "Bad contact list.";
    }
    cfg_pins_t pins;
    config_get_pins(&pins);
    const int nbuttons = pins.count;
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
        if (ct->number[0] && i >= nbuttons) {
            snprintf(err, err_len,
                     "Button %d has no pin on the phone. Add it under Buttons and lights first, or leave it empty.",
                     i + 1);
            return err;
        }
        const char *mode = json_str(it, "secret");
        if (!mode[0] || strcmp(mode, "off") == 0) {
            ct->secret = CFG_SECRET_OFF;
        } else if (strcmp(mode, "hold") == 0) {
            ct->secret = CFG_SECRET_HOLD;
        } else if (strcmp(mode, "dedicated") == 0) {
            ct->secret = CFG_SECRET_DEDICATED;
        } else {
            snprintf(err, err_len, "Button %d: unknown secret message setting.", i + 1);
            return err;
        }
        if (ct->secret != CFG_SECRET_OFF) {
            char em[128]; /* longer than ct->email, so a long address is refused, not cut */
            trim_copy(json_str(it, "email"), em, sizeof(em));
            if (!email_ok(em)) {
                snprintf(err, err_len, "Button %d: %s", i + 1,
                         em[0] ? "that email address doesn't look right."
                               : "add the email address the secret messages go to.");
                return err;
            }
            strlcpy(ct->email, em, sizeof(ct->email));
            if (i >= nbuttons) {
                snprintf(err, err_len,
                         "Button %d has no pin on the phone. Add it under Buttons and lights first, or turn its "
                         "secret messages off.", i + 1);
                return err;
            }
        }
        if (ct->secret == CFG_SECRET_HOLD && !ct->number[0]) {
            snprintf(err, err_len,
                     "Button %d: with Long press, a short press still calls, so it needs a phone number. Pick "
                     "Secret button only for a button that never calls.", i + 1);
            return err;
        }
        if (ct->secret == CFG_SECRET_DEDICATED && ct->number[0]) {
            snprintf(err, err_len,
                     "Button %d is a secret button only, so it doesn't call anyone. Clear its phone number, or "
                     "pick Long press.", i + 1);
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
        if (!ct->number[0]) {
            memset(sc, 0, sizeof(*sc)); /* nobody to call: no hours to keep */
        }
        i++;
    }
    if (!config_any_contact(out) && !config_any_dedicated(out)) {
        return "Add at least one person to call, with their phone number, or a secret button.";
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
    /* Contacts and config are several KB each: heap, not the server stack. */
    cfg_contact_t *ct = calloc(CFG_MAX_CONTACTS, sizeof(cfg_contact_t));
    app_cfg_t *c = malloc(sizeof(app_cfg_t));
    const bool mem = ct && c;
    cJSON *j = mem ? read_json(req) : NULL;
    if (!j) {
        free(ct);
        free(c);
        return mem ? send_msg(req, "400 Bad Request", false, "Missing, too large or bad request.")
                   : send_msg(req, "503 Service Unavailable", false, "The phone is busy. Try again.");
    }

    char sid[40], token[80], errbuf[200];
    trim_copy(json_str(j, "account_sid"), sid, sizeof(sid));
    trim_copy(json_str(j, "auth_token"), token, sizeof(token));
    const bool force_full = cJSON_IsTrue(cJSON_GetObjectItem(j, "full"));
    const char *err = NULL;
    const cJSON *arr = cJSON_GetObjectItem(j, "contacts");
    if (arr) {
        err = parse_contacts(arr, ct, errbuf, sizeof(errbuf));
    } else {
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

    config_get(c);
    if (!sid[0]) {
        strlcpy(sid, c->account_sid, sizeof(sid));
    }
    bool sid_ok = strlen(sid) == 34 && sid[0] == 'A' && sid[1] == 'C';
    for (int i = 2; sid_ok && i < 34; i++) {
        sid_ok = isxdigit((unsigned char)sid[i]) != 0;
    }
    size_t tl = strlen(token);
    if (!err && !config_any_contact(ct)) {
        err = "Add at least one person to call, with their phone number.";
    } else if (!err && !sid_ok) {
        err = "The Account SID should start with AC and be 34 characters long.";
    } else if (!err && (tl < 16 || tl > 64)) {
        err = "The Auth Token looks wrong. Click Show next to Auth Token in the Twilio Console and copy it again.";
    }

    /* Quick path only when the phone already has a working setup with its
     * own login and this is the same account. */
    char want_user[33];
    twilio_sip_username(want_user, sizeof(want_user));
    bool quick = !force_full && c->provisioned && strcmp(c->sip_user, want_user) == 0 &&
                 strcmp(c->account_sid, sid) == 0;
    config_wipe(c);
    free(c);

    bool started = !err && twilio_provision_start(sid, token, ct, quick);
    mbedtls_platform_zeroize(token, sizeof(token));
    free(ct);
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
 * POST /api/contacts {contacts: [...]}: names, schedules and secret message
 * modes, no Twilio token. Numbers may move between buttons freely. A phone
 * not connected to Twilio can save secret buttons only (no numbers). A changed set of numbers has to
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
    /* Contacts and config are several KB each: heap, not the server stack. */
    cfg_contact_t *ct = calloc(CFG_MAX_CONTACTS, sizeof(cfg_contact_t));
    app_cfg_t *c = malloc(sizeof(app_cfg_t));
    if (!ct || !c) {
        free(ct);
        free(c);
        cJSON_Delete(j);
        return send_msg(req, "503 Service Unavailable", false, "The phone is busy. Try again.");
    }
    char errbuf[200];
    const char *err = parse_contacts(cJSON_GetObjectItem(j, "contacts"), ct, errbuf, sizeof(errbuf));
    cJSON_Delete(j);
    if (err) {
        free(ct);
        free(c);
        return send_msg(req, "400 Bad Request", false, err);
    }
    config_get(c);
    bool provisioned = c->provisioned;
    bool same = same_numbers(ct, c->contacts);
    config_wipe(c);
    free(c);
    /* Once connected, Twilio setup always keeps at least one number. */
    if (provisioned && !config_any_contact(ct)) {
        free(ct);
        return send_msg(req, "400 Bad Request", false,
                        "Keep at least one person to call while the phone is connected to Twilio.");
    }
    if (provisioned && !same && config_has_twilio_key()) {
        bool started = wifi_is_connected() && twilio_provision_start_with_key(ct);
        free(ct);
        if (!wifi_is_connected()) {
            return send_msg(req, "409 Conflict", false, "The phone is not on WiFi.");
        }
        if (!started) {
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
    /* Not connected to Twilio and no numbers at all (secret buttons only):
     * nothing for Twilio to allow, save right away. */
    const bool secrets_only = !provisioned && !config_any_contact(ct);
    if (!secrets_only && (!provisioned || !same)) {
        free(ct);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddBoolToObject(o, "needs_token", true);
        cJSON_AddStringToObject(o, "error", provisioned
                                                ? "The phone numbers changed, so Twilio has to be told. Enter your "
                                                  "Auth Token below."
                                                : "Connect to Twilio first.");
        return send_json(req, "409 Conflict", o);
    }
    esp_err_t saved = config_set_contacts(ct);
    free(ct);
    if (saved != ESP_OK) {
        return send_msg(req, "500 Internal Server Error", false, "Could not save. Try again.");
    }
    ESP_LOGI(TAG, "contacts and schedules saved from the web page");
    return send_msg(req, "200 OK", true, "Saved.");
}

/* POST /api/settings {max_call_minutes, lang, emergency_taps}: phone-only
 * settings, no Twilio. Any field may be left out; all are checked before any
 * is saved. emergency_taps 0 or null = off. */
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
    const cJSON *l = cJSON_GetObjectItem(j, "lang");
    const cJSON *e = cJSON_GetObjectItem(j, "emergency_taps");
    bool num = cJSON_IsNumber(v);
    double m = num ? v->valuedouble : 0;
    prompt_lang_t lang = cJSON_IsString(l) ? prompt_lang_from_code(l->valuestring) : PROMPT_LANG_COUNT;
    const bool e_ok = cJSON_IsNull(e) || cJSON_IsNumber(e);
    const double et = cJSON_IsNumber(e) ? e->valuedouble : 0;
    cJSON_Delete(j);
    if (!v && !l && !e) {
        return send_msg(req, "400 Bad Request", false, "Nothing to save.");
    }
    if (v && (!num || m != (double)(int)m || m < APP_CALL_MAX_MIN_LO || m > APP_CALL_MAX_MIN_HI)) {
        char e[80];
        snprintf(e, sizeof(e), "Pick a call length from %d to %d minutes.", APP_CALL_MAX_MIN_LO, APP_CALL_MAX_MIN_HI);
        return send_msg(req, "400 Bad Request", false, e);
    }
    if (l && lang == PROMPT_LANG_COUNT) {
        return send_msg(req, "400 Bad Request", false, "Pick English, Catalan or Spanish.");
    }
    if (e && (!e_ok || et != (double)(int)et ||
              (et != 0 && (et < APP_EMERGENCY_TAPS_LO || et > APP_EMERGENCY_TAPS_HI)))) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Leave it empty to turn it off, or pick %d to %d presses.", APP_EMERGENCY_TAPS_LO,
                 APP_EMERGENCY_TAPS_HI);
        return send_msg(req, "400 Bad Request", false, msg);
    }
    if (v) {
        if (config_set_max_call_minutes((unsigned)m) != ESP_OK) {
            return send_msg(req, "500 Internal Server Error", false, "Could not save. Try again.");
        }
        ESP_LOGI(TAG, "max call length set to %d min from the web page", (int)m);
    }
    if (l) {
        if (config_set_lang((uint8_t)lang) != ESP_OK) {
            return send_msg(req, "500 Internal Server Error", false, "Could not save. Try again.");
        }
        prompt_set_lang(lang);
        ESP_LOGI(TAG, "prompt language set to %s from the web page", prompt_lang_code(lang));
    }
    if (e) {
        if (config_set_emergency_taps((unsigned)et) != ESP_OK) {
            return send_msg(req, "500 Internal Server Error", false, "Could not save. Try again.");
        }
        ESP_LOGI(TAG, "emergency override set to %d presses (0 = off) from the web page", (int)et);
    }
    return send_msg(req, "200 OK", true, "Saved.");
}

/* POST /api/prompt/sample {}: play one prompt ("They didn't answer the
 * phone.") on the handset in the saved language. Only while idle with the
 * handset plugged in: a prompt would end a secret recording's audio. */
static esp_err_t h_prompt_sample(httpd_req_t *req)
{
    if (!post_ok(req)) {
        return ESP_OK;
    }
    if (!usb_audio_handset_connected()) {
        return send_msg(req, "409 Conflict", false, "Plug in the handset first.");
    }
    if (sip_call_state() != CALL_IDLE || secret_active()) {
        return send_msg(req, "409 Conflict", false, "The phone is busy. Try again when it is idle.");
    }
    ESP_LOGI(TAG, "sample prompt from the web page");
    prompt_play(PROMPT_NO_ANSWER);
    return send_msg(req, "200 OK", true, "Playing on the handset.");
}

/*
 * POST /api/bedtime {enabled, start, end, tz, tz_name}: the nightly bedtime
 * window ("HH:MM" or minutes, home time) and the home time zone. Device
 * only, no Twilio and no Auth Token.
 */
static esp_err_t h_bedtime(httpd_req_t *req)
{
    if (!post_ok(req)) {
        return ESP_OK;
    }
    cJSON *j = read_json(req);
    if (!j) {
        return send_msg(req, "400 Bad Request", false, "Missing, too large or bad request.");
    }
    cfg_bedtime_t b = {0};
    b.enabled = cJSON_IsTrue(cJSON_GetObjectItem(j, "enabled"));
    const int start = json_minutes(cJSON_GetObjectItem(j, "start"));
    const int end = json_minutes(cJSON_GetObjectItem(j, "end"));
    trim_copy(json_str(j, "tz"), b.tz, sizeof(b.tz));
    trim_copy(json_str(j, "tz_name"), b.tz_name, sizeof(b.tz_name));
    cJSON_Delete(j);
    if (start < 0 || start >= 1440 || end < 0 || end >= 1440) {
        return send_msg(req, "400 Bad Request", false, "Fill in both bedtime times.");
    }
    b.start_min = (uint16_t)start;
    b.end_min = (uint16_t)end;
    const char *why = schedule_bedtime_validate(&b);
    if (why) {
        char e[80];
        snprintf(e, sizeof(e), "Bedtime: %s.", why);
        return send_msg(req, "400 Bad Request", false, e);
    }
    if (config_set_bedtime(&b) != ESP_OK) {
        return send_msg(req, "500 Internal Server Error", false, "Could not save. Try again.");
    }
    ESP_LOGI(TAG, "bedtime %s (%02d:%02d to %02d:%02d) from the web page", b.enabled ? "on" : "off", start / 60,
             start % 60, end / 60, end % 60);
    return send_msg(req, "200 OK", true, "Saved.");
}

/*
 * POST /api/pins {buttons: [{button, lamp}, ...]}: which GPIO is button i and
 * its light (lamp -1 or null = no light), 1..CFG_MAX_CONTACTS buttons in
 * order. Every pin must be in APP_ASSIGNABLE_GPIOS and used once (400
 * otherwise). Removing a button whose slot still has a number is refused
 * (409): clear the number and Save first, so Twilio's ALLOWED list never
 * holds a number no button can call. Device only, no token; the button task
 * applies the new pins within about 100 ms, no restart.
 */
static esp_err_t h_pins(httpd_req_t *req)
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
    cfg_pins_t p;
    memset(&p, 0, sizeof(p));
    memset(p.button, -1, sizeof(p.button));
    memset(p.lamp, -1, sizeof(p.lamp));
    const cJSON *arr = cJSON_GetObjectItem(j, "buttons");
    bool bad = !cJSON_IsArray(arr) || cJSON_GetArraySize(arr) < 1 || cJSON_GetArraySize(arr) > CFG_MAX_CONTACTS;
    const cJSON *it;
    if (!bad) {
        cJSON_ArrayForEach(it, arr) {
            const cJSON *b = cJSON_GetObjectItem(it, "button");
            const cJSON *l = cJSON_GetObjectItem(it, "lamp");
            if (!cJSON_IsNumber(b) || (l && !cJSON_IsNull(l) && !cJSON_IsNumber(l))) {
                bad = true;
                break;
            }
            const int bv = b->valueint, lv = cJSON_IsNumber(l) ? l->valueint : -1;
            /* Out of int8 range can never be assignable: -2 fails validation. */
            p.button[p.count] = (int8_t)(bv >= 0 && bv < 64 ? bv : -2);
            p.lamp[p.count] = (int8_t)(lv >= -1 && lv < 64 ? lv : -2);
            p.count++;
        }
    }
    cJSON_Delete(j);
    if (bad) {
        char e[80];
        snprintf(e, sizeof(e), "Send 1 to %d buttons, each with a pin.", CFG_MAX_CONTACTS);
        return send_msg(req, "400 Bad Request", false, e);
    }
    char errbuf[200];
    const char *why = pins_validate(&p, errbuf, sizeof(errbuf));
    if (why) {
        return send_msg(req, "400 Bad Request", false, why);
    }
    int slot = -1;
    esp_err_t e = config_set_pins(&p, &slot);
    if (e == ESP_ERR_INVALID_STATE) {
        snprintf(errbuf, sizeof(errbuf),
                 "Button %d still calls someone. Clear their number under Who the phone calls and press Save, "
                 "then remove the button.", slot + 1);
        return send_msg(req, "409 Conflict", false, errbuf);
    }
    if (e != ESP_OK) {
        return send_msg(req, "500 Internal Server Error", false, "Could not save. Try again.");
    }
    ESP_LOGI(TAG, "pins saved from the web page: %u button%s", p.count, p.count == 1 ? "" : "s");
    return send_msg(req, "200 OK", true, "Saved. The buttons and lights work on their new pins now.");
}

/* POST /api/call {contact: i}: test call, same rules as button i, except
 * bedtime: this is the parent testing, so it calls during bedtime too. */
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
    cfg_pins_t pins;
    config_get_pins(&pins);
    if (idx < 0 || idx >= CFG_MAX_CONTACTS || idx >= pins.count) {
        return send_msg(req, "400 Bad Request", false, "No such button.");
    }
    if (!wifi_is_connected() || !config_is_provisioned() || twilio_provision_running()) {
        return send_msg(req, "409 Conflict", false, "The phone is not ready to call yet.");
    }
    if (sip_call_state() != CALL_IDLE) {
        return send_msg(req, "409 Conflict", false, "A call is already in progress.");
    }
    if (secret_active()) {
        return send_msg(req, "409 Conflict", false, "The phone is recording a secret message right now.");
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

/* smtp_security from the page: "tls" (or missing) = SMTP_TLS, "starttls" =
 * SMTP_STARTTLS, anything else -1. */
static int json_security(const cJSON *j)
{
    const char *v = json_str(j, "smtp_security");
    return strcmp(v, "starttls") == 0 ? SMTP_STARTTLS : strcmp(v, "tls") == 0 || !v[0] ? SMTP_TLS : -1;
}

/* A whole number in lo..hi, or -1. */
static int json_int(const cJSON *j, const char *k, int lo, int hi)
{
    const cJSON *v = cJSON_GetObjectItem(j, k);
    if (!cJSON_IsNumber(v) || v->valuedouble != (double)(int)v->valuedouble || v->valueint < lo || v->valueint > hi) {
        return -1;
    }
    return v->valueint;
}

/*
 * POST /api/secret {smtp_host, smtp_port, smtp_security ("tls" | "starttls"),
 * smtp_user, smtp_pass, from_name, kid_name, max_seconds, hourly_cap,
 * bedtime_ok}: the secret message email account and limits. smtp_pass left
 * out or empty keeps the saved password; clear_password: true erases it.
 * Device only, no Twilio. The password is never sent back (/api/status has
 * has_password only).
 */
static esp_err_t h_secret(httpd_req_t *req)
{
    if (!post_ok(req)) {
        return ESP_OK;
    }
    cJSON *j = read_json(req);
    if (!j) {
        return send_msg(req, "400 Bad Request", false, "Missing, too large or bad request.");
    }
    cfg_secret_t *s = calloc(1, sizeof(cfg_secret_t));
    if (!s) {
        cJSON_Delete(j);
        return send_msg(req, "503 Service Unavailable", false, "The phone is busy. Try again.");
    }
    char host[96], user[128], pass[96];
    trim_copy(json_str(j, "smtp_host"), host, sizeof(host));
    trim_copy(json_str(j, "smtp_user"), user, sizeof(user));
    /* App passwords are shown in groups ("abcd efgh ijkl mnop"): spaces out. */
    size_t pl = 0;
    for (const char *p = json_str(j, "smtp_pass"); *p && pl + 1 < sizeof(pass); p++) {
        if (*p != ' ') {
            pass[pl++] = *p;
        }
    }
    pass[pl] = '\0';
    const bool clear_pass = cJSON_IsTrue(cJSON_GetObjectItem(j, "clear_password"));
    trim_copy(json_str(j, "from_name"), s->from_name, sizeof(s->from_name));
    trim_copy(json_str(j, "kid_name"), s->kid_name, sizeof(s->kid_name));
    const int port = json_int(j, "smtp_port", 1, 65535);
    const int sec = json_security(j);
    const int max_s = json_int(j, "max_seconds", APP_SECRET_MAX_S_LO, APP_SECRET_MAX_S_HI);
    const int cap = json_int(j, "hourly_cap", APP_SECRET_CAP_LO, APP_SECRET_CAP_HI);
    s->bed_ok = cJSON_IsTrue(cJSON_GetObjectItem(j, "bedtime_ok"));
    cJSON *pw = cJSON_GetObjectItem(j, "smtp_pass");
    if (cJSON_IsString(pw)) {
        mbedtls_platform_zeroize(pw->valuestring, strlen(pw->valuestring));
    }
    cJSON_Delete(j);

    char e[120];
    const char *err = NULL;
    bool host_valid = host[0] && strlen(host) < sizeof(s->host);
    for (const char *p = host; host_valid && *p; p++) {
        host_valid = isalnum((unsigned char)*p) || *p == '.' || *p == '-';
    }
    if (host[0] && !host_valid) {
        err = "The mail server name doesn't look right (like smtp.gmail.com).";
    } else if (port < 0) {
        err = "Pick a port from 1 to 65535 (465 or 587 for most providers).";
    } else if (sec < 0) {
        err = "Bad security setting.";
    } else if (user[0] && !email_ok(user)) {
        err = "The login should be your full email address.";
    } else if (pl > 64) {
        err = "That password is too long (64 characters at most).";
    } else if (max_s < 0) {
        snprintf(e, sizeof(e), "Pick a longest secret from %d to %d seconds.", APP_SECRET_MAX_S_LO, APP_SECRET_MAX_S_HI);
        err = e;
    } else if (cap < 0) {
        snprintf(e, sizeof(e), "Pick %d to %d secrets per hour.", APP_SECRET_CAP_LO, APP_SECRET_CAP_HI);
        err = e;
    }
    if (err) {
        mbedtls_platform_zeroize(pass, sizeof(pass));
        free(s);
        return send_msg(req, "400 Bad Request", false, err);
    }
    strlcpy(s->host, host, sizeof(s->host));
    strlcpy(s->user, user, sizeof(s->user));
    strlcpy(s->pass, pass, sizeof(s->pass));
    mbedtls_platform_zeroize(pass, sizeof(pass));
    s->port = (uint16_t)port;
    s->security = (uint8_t)sec;
    s->max_s = (uint16_t)max_s;
    s->cap_hr = (uint8_t)cap;
    const esp_err_t r = config_set_secret(s, !s->pass[0] && !clear_pass);
    mbedtls_platform_zeroize(s, sizeof(*s));
    free(s);
    if (r != ESP_OK) {
        return send_msg(req, "500 Internal Server Error", false, "Could not save. Try again.");
    }
    ESP_LOGI(TAG, "secret message settings saved from the web page");
    return send_msg(req, "200 OK", true, "Saved.");
}

/* POST /api/secret/test {}: email the login's own address with the saved
 * settings. 202, the outcome is in /api/status secret.test. */
static esp_err_t h_secret_test(httpd_req_t *req)
{
    if (!post_ok(req)) {
        return ESP_OK;
    }
    char err[120];
    if (!secret_test(err, sizeof(err))) {
        return send_msg(req, "409 Conflict", false, err);
    }
    ESP_LOGI(TAG, "test email from the web page");
    return send_msg(req, "202 Accepted", true, "Sending a test email...");
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
 * Twilio, numbers key, contacts, call length, bedtime, pins, secret
 * messages) and reboot into setup. The confirmation word is checked here
 * too, not only on the page.
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
    cfg.max_uri_handlers = 18; /* 17 registered */
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
        {.uri = "/api/prompt/sample", .method = HTTP_POST, .handler = h_prompt_sample},
        {.uri = "/api/bedtime", .method = HTTP_POST, .handler = h_bedtime},
        {.uri = "/api/pins", .method = HTTP_POST, .handler = h_pins},
        {.uri = "/api/secret", .method = HTTP_POST, .handler = h_secret},
        {.uri = "/api/secret/test", .method = HTTP_POST, .handler = h_secret_test},
        {.uri = "/api/call", .method = HTTP_POST, .handler = h_call},
        {.uri = "/api/hangup", .method = HTTP_POST, .handler = h_hangup},
        {.uri = "/api/factory_reset", .method = HTTP_POST, .handler = h_factory_reset},
        {.uri = "/api/wifi", .method = HTTP_GET, .handler = h_wifi},
        {.uri = "/api/wifi/scan", .method = HTTP_POST, .handler = h_wifi_scan},
        {.uri = "/api/wifi/add", .method = HTTP_POST, .handler = h_wifi_add},
        {.uri = "/api/wifi/remove", .method = HTTP_POST, .handler = h_wifi_remove},
    };
    _Static_assert(sizeof(uris) / sizeof(uris[0]) <= 18, "raise max_uri_handlers");
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t e = httpd_register_uri_handler(srv, &uris[i]);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(e));
        }
    }
    ESP_LOGI(TAG, "setup page: http://%s.local/ (port %d)", APP_HOSTNAME, APP_HTTP_PORT);
}
