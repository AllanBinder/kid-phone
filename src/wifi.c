#include "wifi.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "mbedtls/platform_util.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "config_store.h"
/* Optional developer override: only WIFI_STATIC_IP / WIFI_STATIC_GW are used
 * here. Credentials come from config_store (NVS). */
#if __has_include("secrets.h")
#include "secrets.h"
#endif

static const char *TAG = "wifi";

#define SCAN_CACHE     24    /* strongest records kept for wifi_scan() callers */
#define LEAVE_WAIT_MS  2000  /* wait for our own disconnect to be confirmed */
#define DROP_DELAY_MS  1500  /* leave a removed network after the HTTP reply is out */
#define TRY_RETRY_MS   1000  /* Improv: pause between attempts within its window */
#define API_WAIT_MS    30000 /* longest a wifi_scan() caller waits */

/*
 * Manager states. Only the manager task changes them (s_state is atomic so
 * wifi_state_name can read it).
 *   IDLE        no saved network: nothing to do until one is saved
 *   SCANNING    round scan running (results pick the candidates)
 *   CONNECTING  esp_wifi_connect issued, waiting for an IP or a failure
 *   LEAVING     we disconnected on purpose, waiting for the confirmation
 *   CONNECTED   associated with an IP address: stays here until a disconnect
 *   BACKOFF     no candidate worked, waiting before the next round
 */
typedef enum { ST_IDLE, ST_SCANNING, ST_CONNECTING, ST_LEAVING, ST_CONNECTED, ST_BACKOFF } state_t;

/* What follows LEAVING. */
typedef enum { AFTER_ROUND, AFTER_NEXT, AFTER_TRY } after_t;

typedef enum {
    M_START,
    M_SCAN_DONE,
    M_DISCONNECTED, /* arg = reason */
    M_GOT_IP,
    M_LOST_IP,
    M_SCAN_REQ,     /* arg = request id */
    M_TRY_REQ,      /* arg = request id */
    M_KICK,
    M_ASSOC,        /* associated with the access point, DHCP next */
} msg_type_t;

/* M_DISCONNECTED arg: reason, plus this flag when the event names a
 * different SSID than the one the manager is on or trying (a late event from
 * an earlier attempt). */
#define DISC_OTHER_SSID 0x100

typedef struct {
    msg_type_t type;
    int arg;
} msg_t;

static esp_netif_t *s_netif;
static QueueHandle_t s_q;
static _Atomic bool s_connected;
static _Atomic int s_state = ST_IDLE;

/* SSID connected to, or being tried ("" otherwise). Read by other tasks. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_cur_ssid[33];

/* ---- manager task only ---- */
static int64_t s_deadline_us;      /* state timer, 0 = none */
static int64_t s_scan_deadline_us; /* scan watchdog, 0 = none */
static cfg_wifi_t s_nets[CFG_MAX_WIFI];
static int s_nnets;
static int s_cand[CFG_MAX_WIFI];   /* indexes into s_nets, in try order */
static int s_ncand, s_cpos;
static int s_backoff_ms = APP_WIFI_BACKOFF_MIN_MS;
static after_t s_after;
static bool s_drop_pending;        /* CONNECTED: the network was removed, leave at the deadline */
static bool s_scan_busy;           /* a scan started by us is running */
static bool s_scan_for_round;      /* its result picks the round's candidates */
static bool s_scan_user_wait;      /* a wifi_scan() caller waits for it */
static bool s_scan_user_pending;   /* a wifi_scan() caller waits for a scan to start */
static uint32_t s_scan_req;
static bool s_try_pending, s_trying, s_attempt_try;
static bool s_assoc;               /* CONNECTING: associated, waiting for DHCP */
static int64_t s_try_deadline_us;
static uint32_t s_try_req;

/* ---- shared with callers of the blocking API ---- */
static SemaphoreHandle_t s_api_lock;   /* one wifi_scan / wifi_try_network at a time */
static SemaphoreHandle_t s_done;       /* manager -> caller */
static portMUX_TYPE s_done_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_done_id;
static int s_done_result;
static _Atomic uint32_t s_req_seq;
static SemaphoreHandle_t s_cache_lock;
static wifi_ap_record_t s_cache[SCAN_CACHE];
static int s_cache_n;
static cfg_wifi_t s_try;               /* written by the caller before M_TRY_REQ */
static int s_try_timeout_ms;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static void set_state(state_t st)
{
    atomic_store(&s_state, st);
}

static state_t state(void)
{
    return (state_t)atomic_load(&s_state);
}

static void arm(int ms)
{
    s_deadline_us = now_us() + (int64_t)ms * 1000;
}

static void disarm(void)
{
    s_deadline_us = 0;
}

static void set_cur(const char *ssid)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_cur_ssid, ssid, sizeof(s_cur_ssid));
    portEXIT_CRITICAL(&s_mux);
}

static void get_cur(char *out, size_t len)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(out, s_cur_ssid, len);
    portEXIT_CRITICAL(&s_mux);
}

static void complete(uint32_t id, int result)
{
    portENTER_CRITICAL(&s_done_mux);
    s_done_id = id;
    s_done_result = result;
    portEXIT_CRITICAL(&s_done_mux);
    xSemaphoreGive(s_done);
}

static void post(msg_type_t type, int arg)
{
    msg_t m = {.type = type, .arg = arg};
    if (!s_q || xQueueSend(s_q, &m, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "manager queue full, event %d dropped", (int)type);
    }
}

static bool reason_is_password(int r)
{
    return r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || r == WIFI_REASON_HANDSHAKE_TIMEOUT ||
           r == WIFI_REASON_AUTH_FAIL;
}

static bool reason_is_not_found(int r)
{
    return r == WIFI_REASON_NO_AP_FOUND || r == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
           r == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD || r == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

/* Plain words for a disconnect reason, NULL when there are none. */
static const char *reason_words(int r)
{
    if (reason_is_password(r)) {
        return "wrong password";
    }
    if (reason_is_not_found(r)) {
        return "not in range";
    }
    switch (r) {
    case WIFI_REASON_BEACON_TIMEOUT:
        return "signal lost";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "the router has too many devices";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_CONNECTION_FAIL:
        return "the router refused the connection";
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_TIMEOUT:
        return "the router did not answer";
    default:
        return NULL;
    }
}

/* Log suffix for a disconnect reason ("" when there are no words). */
static const char *reason_text(int r)
{
    static char buf[48];
    const char *w = reason_words(r);
    if (!w) {
        return "";
    }
    snprintf(buf, sizeof(buf), ", %s%s", w, reason_is_password(r) ? "?" : "");
    return buf;
}

/* ---------------------------------------------------------------------------
 * Connection history, in RAM only, for the setup page (no serial needed).
 * The manager task writes it; readers copy it under s_hist_mux.
 * ------------------------------------------------------------------------- */

#define HIST_ATTEMPTS 8
#define HIST_NETS     8

typedef struct {
    char ssid[33];
    wifi_net_info_t info;
    uint32_t touch_s; /* last update, picks the slot to reuse */
} net_hist_t;

static portMUX_TYPE s_hist_mux = portMUX_INITIALIZER_UNLOCKED;
static wifi_attempt_t s_att[HIST_ATTEMPTS];
static int s_att_n, s_att_head;    /* count, next slot to write */
static net_hist_t s_nh[HIST_NETS];
static bool s_any_scan;            /* a scan has finished since boot */

uint32_t wifi_uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

/* Slot for ssid (caller holds s_hist_mux). keep[] = SSIDs still saved, a
 * slot of one of those is reused only when nothing else is free. */
static net_hist_t *nh_slot_locked(const char *ssid, bool create, const cfg_wifi_info_t *keep, int nkeep)
{
    for (int i = 0; i < HIST_NETS; i++) {
        if (s_nh[i].ssid[0] && strcmp(s_nh[i].ssid, ssid) == 0) {
            return &s_nh[i];
        }
    }
    if (!create) {
        return NULL;
    }
    int pick = -1;
    bool pick_kept = true;
    for (int i = 0; i < HIST_NETS; i++) {
        if (!s_nh[i].ssid[0]) {
            pick = i;
            break;
        }
        bool kept = false;
        for (int k = 0; k < nkeep && !kept; k++) {
            kept = strcmp(keep[k].ssid, s_nh[i].ssid) == 0;
        }
        if (pick < 0 || (pick_kept && !kept) ||
            (pick_kept == kept && s_nh[i].touch_s < s_nh[pick].touch_s)) {
            pick = i;
            pick_kept = kept;
        }
    }
    memset(&s_nh[pick], 0, sizeof(s_nh[pick]));
    strlcpy(s_nh[pick].ssid, ssid, sizeof(s_nh[pick].ssid));
    return &s_nh[pick];
}

/* A scan finished: note which saved networks it saw and how strong
 * (recs sorted strongest first, so the first match is the best). */
static void hist_scan(const wifi_ap_record_t *recs, int n)
{
    cfg_wifi_info_t saved[CFG_MAX_WIFI];
    int ns = config_wifi_list(saved, CFG_MAX_WIFI);
    const uint32_t t = wifi_uptime_s();
    portENTER_CRITICAL(&s_hist_mux);
    s_any_scan = true;
    for (int i = 0; i < ns; i++) {
        net_hist_t *h = nh_slot_locked(saved[i].ssid, true, saved, ns);
        h->info.in_scan = false;
        for (int r = 0; r < n; r++) {
            if (strncmp((const char *)recs[r].ssid, saved[i].ssid, sizeof(recs[r].ssid)) == 0) {
                h->info.in_scan = true;
                h->info.ever_seen = true;
                h->info.rssi = recs[r].rssi;
                h->info.seen_s = t;
                break;
            }
        }
        h->touch_s = t;
    }
    portEXIT_CRITICAL(&s_hist_mux);
}

/* One attempt ended (or an established connection dropped). */
static void hist_attempt(const char *ssid, wifi_result_t result, int reason, bool improv)
{
    cfg_wifi_info_t saved[CFG_MAX_WIFI];
    int ns = config_wifi_list(saved, CFG_MAX_WIFI);
    const uint32_t t = wifi_uptime_s();
    portENTER_CRITICAL(&s_hist_mux);
    wifi_attempt_t *a = &s_att[s_att_head];
    memset(a, 0, sizeof(*a));
    strlcpy(a->ssid, ssid, sizeof(a->ssid));
    a->result = (uint8_t)result;
    a->reason = (uint8_t)(reason > 0 && reason < 256 ? reason : 0);
    a->improv = improv;
    a->at_s = t;
    net_hist_t *h = nh_slot_locked(ssid, result != WIFI_RES_DROPPED, saved, ns);
    if (h && h->info.in_scan) {
        a->seen = true;
        a->rssi = h->info.rssi;
    }
    if (h && result != WIFI_RES_DROPPED) {
        h->info.tried = true;
        h->info.result = (uint8_t)result;
        h->info.reason = a->reason;
        h->info.try_s = t;
        h->touch_s = t;
    }
    s_att_head = (s_att_head + 1) % HIST_ATTEMPTS;
    if (s_att_n < HIST_ATTEMPTS) {
        s_att_n++;
    }
    portEXIT_CRITICAL(&s_hist_mux);
}

/* Result of a failed attempt from its disconnect reason. */
static wifi_result_t classify(int reason)
{
    if (reason_is_password(reason)) {
        return WIFI_RES_WRONG_PASSWORD;
    }
    if (reason_is_not_found(reason)) {
        return WIFI_RES_NOT_FOUND;
    }
    return WIFI_RES_OTHER;
}

int wifi_attempts(wifi_attempt_t *out, int max)
{
    portENTER_CRITICAL(&s_hist_mux);
    int n = s_att_n < max ? s_att_n : max;
    for (int i = 0; i < n; i++) {
        out[i] = s_att[(s_att_head - 1 - i + HIST_ATTEMPTS) % HIST_ATTEMPTS];
    }
    portEXIT_CRITICAL(&s_hist_mux);
    return n;
}

bool wifi_net_info(const char *ssid, wifi_net_info_t *out, bool *scanned)
{
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_hist_mux);
    if (scanned) {
        *scanned = s_any_scan;
    }
    const net_hist_t *h = nh_slot_locked(ssid, false, NULL, 0);
    if (h) {
        *out = h->info;
    }
    portEXIT_CRITICAL(&s_hist_mux);
    return h != NULL;
}

void wifi_history_new_password(const char *ssid)
{
    portENTER_CRITICAL(&s_hist_mux);
    net_hist_t *h = nh_slot_locked(ssid, false, NULL, 0);
    if (h) {
        h->info.tried = false;
        h->info.result = 0;
        h->info.reason = 0;
        h->info.try_s = 0;
    }
    portEXIT_CRITICAL(&s_hist_mux);
}

const char *wifi_result_key(int result)
{
    switch (result) {
    case WIFI_RES_CONNECTED:
        return "connected";
    case WIFI_RES_WRONG_PASSWORD:
        return "wrong_password";
    case WIFI_RES_NOT_FOUND:
        return "not_found";
    case WIFI_RES_TIMEOUT:
        return "timeout";
    case WIFI_RES_NO_IP:
        return "no_ip";
    case WIFI_RES_DROPPED:
        return "dropped";
    default:
        return "other";
    }
}

void wifi_result_text(int result, int reason, char *buf, size_t len)
{
    const char *w = reason_words(reason);
    switch (result) {
    case WIFI_RES_CONNECTED:
        strlcpy(buf, "connected", len);
        break;
    case WIFI_RES_WRONG_PASSWORD:
        strlcpy(buf, "wrong password", len);
        break;
    case WIFI_RES_NOT_FOUND:
        strlcpy(buf, "network not found", len);
        break;
    case WIFI_RES_TIMEOUT:
        strlcpy(buf, "no answer from the router (timed out)", len);
        break;
    case WIFI_RES_NO_IP:
        strlcpy(buf, "joined, but the router gave no IP address", len);
        break;
    case WIFI_RES_DROPPED:
        if (reason == 0) {
            strlcpy(buf, "connection dropped (lost the IP address)", len);
        } else if (w) {
            snprintf(buf, len, "connection dropped: %s", w);
        } else {
            snprintf(buf, len, "connection dropped (code %d)", reason);
        }
        break;
    default:
        if (reason == 0) {
            strlcpy(buf, "could not start", len);
        } else if (w) {
            strlcpy(buf, w, len);
        } else {
            snprintf(buf, len, "failed (code %d)", reason);
        }
        break;
    }
}

/* SSID and password are copied as raw bytes: the SSID is UTF-8 and may
 * contain non-ASCII characters. */
static void fill_sta_config(wifi_config_t *cfg, const char *ssid, const char *pass)
{
    memset(cfg, 0, sizeof(*cfg));
    memcpy(cfg->sta.ssid, ssid, strnlen(ssid, sizeof(cfg->sta.ssid)));
    memcpy(cfg->sta.password, pass, strnlen(pass, sizeof(cfg->sta.password)));
    cfg->sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
}

/* ---------------------------------------------------------------------------
 * Manager: steps
 * ------------------------------------------------------------------------- */

static void start_round(void);

static void wipe_nets(void)
{
    mbedtls_platform_zeroize(s_nets, sizeof(s_nets));
    s_nnets = 0;
    s_ncand = 0;
    s_cpos = 0;
}

static bool start_scan(void)
{
    if (s_scan_busy) {
        return true; /* a scan is already running: share its result */
    }
    const wifi_scan_config_t sc = {.show_hidden = true, .scan_type = WIFI_SCAN_TYPE_ACTIVE};
    esp_err_t err = esp_wifi_scan_start(&sc, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan could not start: %s", esp_err_to_name(err));
        return false;
    }
    s_scan_busy = true;
    s_scan_deadline_us = now_us() + (int64_t)APP_WIFI_SCAN_TIMEOUT_MS * 1000;
    return true;
}

/* Order the saved networks for this round. recs sorted strongest first;
 * n < 0 = the scan failed. */
static void build_candidates(const wifi_ap_record_t *recs, int n)
{
    int best[CFG_MAX_WIFI];
    bool used[CFG_MAX_WIFI] = {0};
    bool hidden_seen = false;
    for (int i = 0; i < s_nnets; i++) {
        best[i] = INT_MIN;
    }
    for (int r = 0; r < n; r++) {
        const char *ss = (const char *)recs[r].ssid;
        if (!ss[0]) {
            hidden_seen = true;
            continue;
        }
        for (int i = 0; i < s_nnets; i++) {
            if (strncmp(ss, s_nets[i].ssid, sizeof(recs[r].ssid)) == 0 && recs[r].rssi > best[i]) {
                best[i] = recs[r].rssi;
            }
        }
    }
    s_ncand = 0;
    s_cpos = 0;
    char line[160] = "";
    size_t ll = 0;
    while (1) {
        int pick = -1;
        for (int i = 0; i < s_nnets; i++) {
            if (!used[i] && best[i] != INT_MIN && (pick < 0 || best[i] > best[pick])) {
                pick = i;
            }
        }
        if (pick < 0) {
            break;
        }
        used[pick] = true;
        s_cand[s_ncand++] = pick;
        if (ll < sizeof(line)) {
            ll += (size_t)snprintf(line + ll, sizeof(line) - ll, "%s\"%s\" %d dBm", ll ? ", " : "",
                                   s_nets[pick].ssid, best[pick]);
        }
    }
    const int visible = s_ncand;
    /* Not seen: maybe the hidden network the scan reported, or the scan
     * itself failed or came back empty. Try them last, in saved order. */
    if (n <= 0 || hidden_seen) {
        for (int i = 0; i < s_nnets; i++) {
            if (!used[i]) {
                s_cand[s_ncand++] = i;
            }
        }
    }
    if (n < 0) {
        ESP_LOGW(TAG, "no scan result: trying all %d saved networks", s_nnets);
    } else {
        ESP_LOGI(TAG, "scan: %d access points, %d of %d saved networks in range%s%s%s", n, visible, s_nnets,
                 visible ? " (" : "", line, visible ? ")" : "");
        if (s_ncand > visible) {
            ESP_LOGI(TAG, "%d saved network%s not seen, tried last (hidden network or missed by the scan)",
                     s_ncand - visible, s_ncand - visible == 1 ? "" : "s");
        }
    }
}

static bool connect_to(const char *ssid, const char *pass, int timeout_ms)
{
    wifi_config_t cfg;
    /* Before esp_wifi_connect: its disconnect event is matched against it. */
    set_cur(ssid);
    fill_sta_config(&cfg, ssid, pass);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    mbedtls_platform_zeroize(&cfg, sizeof(cfg));
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect to \"%s\" could not start: %s", ssid, esp_err_to_name(err));
        hist_attempt(ssid, WIFI_RES_OTHER, 0, s_trying);
        return false;
    }
    ESP_LOGI(TAG, "connecting to \"%s\" ...", ssid);
    s_assoc = false;
    set_state(ST_CONNECTING);
    arm(timeout_ms);
    return true;
}

static void round_failed(void)
{
    wipe_nets();
    set_cur("");
    ESP_LOGW(TAG, "no saved network could be joined, scanning again in %d s", s_backoff_ms / 1000);
    set_state(ST_BACKOFF);
    arm(s_backoff_ms);
    s_backoff_ms = s_backoff_ms * 2 > APP_WIFI_BACKOFF_MAX_MS ? APP_WIFI_BACKOFF_MAX_MS : s_backoff_ms * 2;
}

static void try_next(void)
{
    s_attempt_try = false;
    while (s_cpos < s_ncand) {
        const cfg_wifi_t *w = &s_nets[s_cand[s_cpos++]];
        if (connect_to(w->ssid, w->pass, APP_WIFI_ATTEMPT_MS)) {
            return;
        }
    }
    round_failed();
}

static void start_round(void)
{
    s_drop_pending = false;
    wipe_nets();
    set_cur("");
    s_nnets = config_wifi_get(s_nets, CFG_MAX_WIFI);
    if (s_nnets == 0) {
        ESP_LOGW(TAG, "no WiFi network saved yet: set one up from the browser installer (Improv Serial)");
        set_state(ST_IDLE);
        disarm();
        return;
    }
    set_state(ST_SCANNING);
    disarm();
    s_scan_for_round = true;
    if (!start_scan()) {
        s_scan_for_round = false;
        build_candidates(NULL, -1);
        try_next();
    }
}

static void leave(after_t after)
{
    s_after = after;
    s_drop_pending = false;
    atomic_store(&s_connected, false);
    esp_wifi_disconnect();
    set_state(ST_LEAVING);
    arm(LEAVE_WAIT_MS);
}

/* Improv window over: report failure, back to the saved networks. The
 * driver is idle whenever this runs. */
static void try_failed(void)
{
    ESP_LOGW(TAG, "could not connect to \"%s\" for Improv, back to the saved networks", s_try.ssid);
    s_trying = false;
    s_attempt_try = false;
    mbedtls_platform_zeroize(&s_try, sizeof(s_try));
    complete(s_try_req, 0);
    s_backoff_ms = APP_WIFI_BACKOFF_MIN_MS;
    start_round();
}

static void try_connect(void)
{
    int64_t left_ms = (s_try_deadline_us - now_us()) / 1000;
    if (left_ms <= 0) {
        try_failed();
        return;
    }
    int t = left_ms < APP_WIFI_ATTEMPT_MS ? (int)left_ms : APP_WIFI_ATTEMPT_MS;
    if (connect_to(s_try.ssid, s_try.pass, t)) {
        s_attempt_try = true;
    } else {
        s_after = AFTER_TRY; /* could not even start: wait a moment, retry */
        set_state(ST_LEAVING);
        arm(TRY_RETRY_MS);
    }
}

static void begin_try(void)
{
    s_try_pending = false;
    s_trying = true;
    s_scan_for_round = false;
    s_try_deadline_us = now_us() + (int64_t)s_try_timeout_ms * 1000;
    ESP_LOGI(TAG, "Improv: trying \"%s\"", s_try.ssid);
    state_t st = state();
    if (st == ST_CONNECTED || st == ST_CONNECTING || st == ST_LEAVING) {
        leave(AFTER_TRY);
    } else {
        wipe_nets();
        try_connect();
    }
}

static void after_leave(void)
{
    switch (s_after) {
    case AFTER_NEXT:
        try_next();
        break;
    case AFTER_TRY:
        try_connect();
        break;
    default:
        start_round();
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Manager: events
 * ------------------------------------------------------------------------- */

static void on_scan_done(void)
{
    if (!s_scan_busy) {
        esp_wifi_clear_ap_list(); /* not ours (or abandoned): free it */
        return;
    }
    s_scan_busy = false;
    s_scan_deadline_us = 0;
    uint16_t total = 0;
    esp_wifi_scan_get_ap_num(&total);
    wifi_ap_record_t *all = total ? calloc(total, sizeof(*all)) : NULL;
    uint16_t n = total;
    if (!all || esp_wifi_scan_get_ap_records(&n, all) != ESP_OK) {
        n = 0;
        esp_wifi_clear_ap_list();
    }
    /* Strongest first (insertion sort: a few dozen records at most). */
    for (int i = 1; i < n; i++) {
        wifi_ap_record_t r = all[i];
        int j = i - 1;
        while (j >= 0 && all[j].rssi < r.rssi) {
            all[j + 1] = all[j];
            j--;
        }
        all[j + 1] = r;
    }
    if (all) {
        hist_scan(all, (int)n);
    }
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    s_cache_n = n < SCAN_CACHE ? n : SCAN_CACHE;
    if (s_cache_n) {
        memcpy(s_cache, all, sizeof(s_cache[0]) * (size_t)s_cache_n);
    }
    xSemaphoreGive(s_cache_lock);

    if (s_scan_user_wait) {
        s_scan_user_wait = false;
        complete(s_scan_req, (int)n);
    }
    if (s_scan_for_round) {
        s_scan_for_round = false;
        if (state() == ST_SCANNING) {
            build_candidates(all, all ? (int)n : -1);
            try_next();
        }
    }
    free(all);
}

static void on_scan_timeout(void)
{
    ESP_LOGW(TAG, "scan did not finish in %d s, abandoned", APP_WIFI_SCAN_TIMEOUT_MS / 1000);
    esp_wifi_scan_stop();
    esp_wifi_clear_ap_list();
    s_scan_busy = false;
    s_scan_deadline_us = 0;
    if (s_scan_user_wait) {
        s_scan_user_wait = false;
        complete(s_scan_req, -1);
    }
    if (s_scan_for_round) {
        s_scan_for_round = false;
        if (state() == ST_SCANNING) {
            build_candidates(NULL, -1);
            try_next();
        }
    }
}

static void on_got_ip(void)
{
    state_t st = state();
    if (st == ST_CONNECTED) {
        atomic_store(&s_connected, true); /* new lease, same network */
        return;
    }
    if (st != ST_CONNECTING) {
        return; /* stale: we are leaving that network */
    }
    char cur[33];
    get_cur(cur, sizeof(cur));
    set_state(ST_CONNECTED);
    disarm();
    s_backoff_ms = APP_WIFI_BACKOFF_MIN_MS;
    wipe_nets();
    atomic_store(&s_connected, true);
    hist_attempt(cur, WIFI_RES_CONNECTED, 0, s_attempt_try);
    if (s_attempt_try) {
        s_attempt_try = false;
        s_trying = false;
        mbedtls_platform_zeroize(&s_try, sizeof(s_try));
        complete(s_try_req, 1);
        ESP_LOGI(TAG, "connected to \"%s\" (Improv)", cur);
    } else {
        ESP_LOGI(TAG, "connected to \"%s\"", cur);
        if (!config_wifi_has(cur)) {
            /* Removed while this attempt was running. */
            s_drop_pending = true;
            arm(DROP_DELAY_MS);
        }
    }
}

static void on_disconnected(int arg)
{
    const int reason = arg & 0xff;
    char cur[33];
    get_cur(cur, sizeof(cur));
    const state_t st = state();
    if ((arg & DISC_OTHER_SSID) && (st == ST_CONNECTING || st == ST_CONNECTED)) {
        /* A late event from an earlier attempt: it must not end this one. */
        ESP_LOGI(TAG, "ignoring a late disconnect event (reason %d) for another network", reason);
        return;
    }
    switch (st) {
    case ST_CONNECTING:
        ESP_LOGW(TAG, "could not connect to \"%s\" (reason %d%s)", cur, reason, reason_text(reason));
        hist_attempt(cur, classify(reason), reason, s_attempt_try);
        if (s_attempt_try) {
            s_attempt_try = false;
            s_after = AFTER_TRY;
            set_state(ST_LEAVING);
            arm(TRY_RETRY_MS);
        } else {
            try_next();
        }
        break;
    case ST_CONNECTED:
        ESP_LOGW(TAG, "disconnected from \"%s\" (reason %d%s), looking for the best saved network", cur, reason,
                 reason_text(reason));
        hist_attempt(cur, WIFI_RES_DROPPED, reason, false);
        s_backoff_ms = APP_WIFI_BACKOFF_MIN_MS;
        start_round();
        break;
    case ST_LEAVING:
        disarm();
        after_leave();
        break;
    default:
        break; /* stale */
    }
}

static void on_deadline(void)
{
    char cur[33];
    get_cur(cur, sizeof(cur));
    switch (state()) {
    case ST_CONNECTING:
        ESP_LOGW(TAG, "\"%s\": no connection after %d s%s", cur, APP_WIFI_ATTEMPT_MS / 1000,
                 s_assoc ? " (joined, but no IP address from the router)" : "");
        hist_attempt(cur, s_assoc ? WIFI_RES_NO_IP : WIFI_RES_TIMEOUT, 0, s_attempt_try);
        leave(s_attempt_try ? AFTER_TRY : AFTER_NEXT);
        s_attempt_try = false;
        break;
    case ST_LEAVING:
        after_leave();
        break;
    case ST_BACKOFF:
        start_round();
        break;
    case ST_CONNECTED:
        if (s_drop_pending) {
            ESP_LOGI(TAG, "\"%s\" was removed from the saved networks, leaving it", cur);
            leave(AFTER_ROUND);
        }
        break;
    default:
        break;
    }
}

static void on_kick(void)
{
    state_t st = state();
    if (st == ST_IDLE || st == ST_BACKOFF) {
        s_backoff_ms = APP_WIFI_BACKOFF_MIN_MS;
        start_round();
    } else if (st == ST_CONNECTED && !s_trying) {
        char cur[33];
        get_cur(cur, sizeof(cur));
        if (cur[0] && !config_wifi_has(cur)) {
            s_drop_pending = true;
            arm(DROP_DELAY_MS);
        }
    }
    /* Otherwise the next round reads the new list. */
}

/* Requests that wait for the radio to be free. */
static void service_pending(void)
{
    if (s_try_pending && !s_scan_busy) {
        begin_try();
    }
    if (!s_scan_user_pending || s_trying || s_try_pending) {
        return;
    }
    state_t st = state();
    if (s_scan_busy) {
        s_scan_user_pending = false;
        s_scan_user_wait = true; /* join the running scan */
    } else if (st == ST_CONNECTED || st == ST_IDLE) {
        s_scan_user_pending = false;
        if (start_scan()) {
            s_scan_user_wait = true;
        } else {
            complete(s_scan_req, -1);
        }
    } else if (st == ST_BACKOFF) {
        /* Offline and waiting anyway: make it a round, the caller gets its scan. */
        s_scan_user_pending = false;
        s_scan_user_wait = true;
        start_round();
        if (!s_scan_busy && s_scan_user_wait) {
            s_scan_user_wait = false;
            complete(s_scan_req, -1);
        }
    }
    /* CONNECTING / LEAVING / SCANNING without our scan: wait for the attempt. */
}

static void manager_task(void *arg)
{
    (void)arg;
    while (1) {
        int64_t wake = s_deadline_us;
        if (s_scan_deadline_us && (!wake || s_scan_deadline_us < wake)) {
            wake = s_scan_deadline_us;
        }
        TickType_t ticks = portMAX_DELAY;
        if (wake) {
            int64_t ms = (wake - now_us()) / 1000;
            ticks = ms <= 0 ? 0 : pdMS_TO_TICKS(ms) + 1;
        }
        msg_t m;
        if (xQueueReceive(s_q, &m, ticks) == pdTRUE) {
            switch (m.type) {
            case M_START:
                if (state() == ST_IDLE || state() == ST_BACKOFF) {
                    start_round();
                }
                break;
            case M_SCAN_DONE:
                on_scan_done();
                break;
            case M_DISCONNECTED:
                on_disconnected(m.arg);
                break;
            case M_GOT_IP:
                on_got_ip();
                break;
            case M_LOST_IP:
                if (state() == ST_CONNECTED) {
                    ESP_LOGW(TAG, "lost the IP address, reconnecting");
                    char cur[33];
                    get_cur(cur, sizeof(cur));
                    hist_attempt(cur, WIFI_RES_DROPPED, 0, false);
                    leave(AFTER_ROUND);
                }
                break;
            case M_SCAN_REQ:
                s_scan_req = (uint32_t)m.arg;
                s_scan_user_pending = true;
                break;
            case M_TRY_REQ:
                s_try_req = (uint32_t)m.arg;
                s_try_pending = true;
                break;
            case M_KICK:
                on_kick();
                break;
            case M_ASSOC:
                if (state() == ST_CONNECTING) {
                    s_assoc = true;
                }
                break;
            }
        }
        const int64_t now = now_us();
        if (s_scan_deadline_us && now >= s_scan_deadline_us) {
            on_scan_timeout();
        }
        if (s_deadline_us && now >= s_deadline_us) {
            disarm();
            on_deadline();
        }
        service_pending();
    }
}

/* ---------------------------------------------------------------------------
 * Driver events (event loop task): post to the manager
 * ------------------------------------------------------------------------- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        post(M_START, 0);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        post(M_SCAN_DONE, 0);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
#ifdef WIFI_STATIC_IP
        /* Bring-up: fixed address, skips DHCP (set in secrets.h) */
        esp_netif_dhcpc_stop(s_netif);
        esp_netif_ip_info_t ip = {0};
        ip.ip.addr = ipaddr_addr(WIFI_STATIC_IP);
        ip.gw.addr = ipaddr_addr(WIFI_STATIC_GW);
        ip.netmask.addr = ipaddr_addr("255.255.255.0");
        esp_netif_dns_info_t dns = {0};
        dns.ip.u_addr.ip4.addr = ipaddr_addr("8.8.8.8");
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        ESP_LOGI(TAG, "associated, static IP %s gw %s", WIFI_STATIC_IP, WIFI_STATIC_GW);
        esp_netif_set_ip_info(s_netif, &ip);
        esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
#else
        ESP_LOGI(TAG, "associated, waiting for DHCP");
#endif
        post(M_ASSOC, 0);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        atomic_store(&s_connected, false);
        int arg = d ? d->reason : 0;
        if (d && d->ssid_len > 0 && d->ssid_len <= sizeof(d->ssid)) {
            char cur[33];
            get_cur(cur, sizeof(cur));
            if (strlen(cur) != d->ssid_len || memcmp(cur, d->ssid, d->ssid_len) != 0) {
                arg |= DISC_OTHER_SSID;
            }
        }
        post(M_DISCONNECTED, arg);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "WIFI UP: ip " IPSTR ", gw " IPSTR, IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
        post(M_GOT_IP, 0);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "lost IP address");
        atomic_store(&s_connected, false);
        post(M_LOST_IP, 0);
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void wifi_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_q = xQueueCreate(24, sizeof(msg_t));
    s_api_lock = xSemaphoreCreateMutex();
    s_cache_lock = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    assert(s_q && s_api_lock && s_cache_lock && s_done);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, "kidphone");

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    /* The manager sets the config before every attempt: keep it out of the
     * driver's own NVS copy (no flash write per network switch). */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));

    xTaskCreatePinnedToCore(manager_task, "wifi_mgr", 4608, NULL, 4, NULL, 0);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start()); /* STA_START starts the first round */
    /* Modem sleep adds up to a beacon interval of latency per packet. */
    esp_wifi_set_ps(WIFI_PS_NONE);
}

bool wifi_is_connected(void)
{
    return atomic_load(&s_connected);
}

bool wifi_get_ip(char *buf, size_t len)
{
    esp_netif_ip_info_t ip;
    if (!s_netif || !wifi_is_connected() || esp_netif_get_ip_info(s_netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        return false;
    }
    esp_ip4addr_ntoa(&ip.ip, buf, (int)len);
    return true;
}

bool wifi_current(char *ssid, size_t len, int *rssi)
{
    get_cur(ssid, len);
    bool up = wifi_is_connected();
    if (rssi) {
        wifi_ap_record_t ap;
        *rssi = up && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    }
    return up;
}

const char *wifi_state_name(void)
{
    switch (state()) {
    case ST_CONNECTED:
        return "connected";
    case ST_CONNECTING:
    case ST_LEAVING:
        return "connecting";
    case ST_SCANNING:
        return "scanning";
    case ST_BACKOFF:
        return "waiting";
    default:
        return "no networks";
    }
}

/* Post a request and wait for the manager's answer to that request id. */
static int request(msg_type_t type, int wait_ms)
{
    uint32_t id = atomic_fetch_add(&s_req_seq, 1) + 1;
    xSemaphoreTake(s_done, 0); /* drop a stale answer */
    post(type, (int)id);
    const int64_t end = now_us() + (int64_t)wait_ms * 1000;
    while (1) {
        int64_t left = (end - now_us()) / 1000;
        if (left <= 0) {
            return -1;
        }
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(left) + 1) == pdTRUE) {
            portENTER_CRITICAL(&s_done_mux);
            bool mine = s_done_id == id;
            int r = s_done_result;
            portEXIT_CRITICAL(&s_done_mux);
            if (mine) {
                return r;
            }
        }
    }
}

int wifi_scan(wifi_ap_record_t *out, int max)
{
    if (!s_q || xSemaphoreTake(s_api_lock, pdMS_TO_TICKS(API_WAIT_MS)) != pdTRUE) {
        return -1;
    }
    int n = request(M_SCAN_REQ, API_WAIT_MS);
    if (n >= 0) {
        xSemaphoreTake(s_cache_lock, portMAX_DELAY);
        n = s_cache_n < max ? s_cache_n : max;
        memcpy(out, s_cache, sizeof(s_cache[0]) * (size_t)n);
        xSemaphoreGive(s_cache_lock);
    } else {
        ESP_LOGW(TAG, "scan failed or timed out");
    }
    xSemaphoreGive(s_api_lock);
    return n;
}

bool wifi_try_network(const char *ssid, const char *pass, int timeout_ms)
{
    if (!s_q || xSemaphoreTake(s_api_lock, pdMS_TO_TICKS(API_WAIT_MS)) != pdTRUE) {
        return false;
    }
    mbedtls_platform_zeroize(&s_try, sizeof(s_try));
    strlcpy(s_try.ssid, ssid, sizeof(s_try.ssid));
    strlcpy(s_try.pass, pass, sizeof(s_try.pass));
    s_try_timeout_ms = timeout_ms;
    /* The manager may first finish a scan (APP_WIFI_SCAN_TIMEOUT_MS) and
     * leave the current network (LEAVE_WAIT_MS). */
    int r = request(M_TRY_REQ, timeout_ms + APP_WIFI_SCAN_TIMEOUT_MS + LEAVE_WAIT_MS + 3000);
    xSemaphoreGive(s_api_lock);
    return r == 1;
}

void wifi_networks_changed(void)
{
    post(M_KICK, 0);
}
