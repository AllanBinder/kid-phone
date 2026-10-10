#include "wifi.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "lwip/inet.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "config_store.h"
/* Optional developer override: only WIFI_STATIC_IP / WIFI_STATIC_GW are used
 * here. Credentials come from config_store (NVS). */
#if __has_include("secrets.h")
#include "secrets.h"
#endif

static const char *TAG = "wifi";

static esp_netif_t *s_netif;
static _Atomic bool s_connected;
static TimerHandle_t s_retry_timer;
static char s_ssid[33]; /* current target SSID, for logs; empty = no credentials */

static void retry_cb(TimerHandle_t t)
{
    (void)t;
    if (s_ssid[0]) {
        esp_wifi_connect();
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_ssid[0]) {
            ESP_LOGI(TAG, "connecting to \"%s\" ...", s_ssid);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "no WiFi credentials yet: set them up from the browser (Improv Serial)");
        }
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
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        bool was = atomic_exchange(&s_connected, false);
        if (s_ssid[0]) {
            ESP_LOGW(TAG, "%s (reason %d), retrying in %d ms", was ? "disconnected" : "connect failed",
                     d ? d->reason : -1, APP_WIFI_RETRY_MS);
            xTimerStart(s_retry_timer, 0);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "WIFI UP: ip " IPSTR ", gw " IPSTR, IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
        atomic_store(&s_connected, true);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "lost IP address");
        atomic_store(&s_connected, false);
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
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
}

void wifi_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));

    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(APP_WIFI_RETRY_MS), pdFALSE, NULL, retry_cb);
    assert(s_retry_timer);

    app_cfg_t c;
    config_get(&c);
    wifi_config_t cfg;
    fill_sta_config(&cfg, c.wifi_ssid, c.wifi_pass);
    config_wipe(&c);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    memset(&cfg, 0, sizeof(cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
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

/* ---------------------------------------------------------------------------
 * Runtime credentials (Improv Serial). Kept apart from the connect logic above.
 * ------------------------------------------------------------------------- */

void wifi_apply_credentials(const char *ssid, const char *pass)
{
    wifi_config_t cfg;
    fill_sta_config(&cfg, ssid, pass);
    xTimerStop(s_retry_timer, 0);
    esp_wifi_disconnect();
    atomic_store(&s_connected, false);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    memset(&cfg, 0, sizeof(cfg));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config failed: %s", esp_err_to_name(err));
    }
    /* The retry timer does the connect (the disconnect event restarts it too),
     * so exactly one connect attempt follows. */
    ESP_LOGI(TAG, "new credentials, connecting to \"%s\" ...", s_ssid);
    xTimerStart(s_retry_timer, 0);
}

const char *wifi_ssid(void)
{
    return s_ssid;
}

int wifi_scan(wifi_ap_record_t *out, int max)
{
    /* A scan fails while the station is mid-connect; stop retrying meanwhile. */
    xTimerStop(s_retry_timer, 0);
    const wifi_scan_config_t sc = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    uint16_t n = (uint16_t)max;
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&n, out);
    }
    if (!atomic_load(&s_connected) && s_ssid[0]) {
        xTimerStart(s_retry_timer, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return 0;
    }
    return n;
}
