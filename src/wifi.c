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
#include "secrets.h"

static const char *TAG = "wifi";

static esp_netif_t *s_netif;
static _Atomic bool s_connected;
static TimerHandle_t s_retry_timer;

static void retry_cb(TimerHandle_t t)
{
    (void)t;
    esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "connecting to \"%s\" ...", WIFI_SSID);
        esp_wifi_connect();
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
        ESP_LOGW(TAG, "%s (reason %d), retrying in %d ms", was ? "disconnected" : "connect failed",
                 d ? d->reason : -1, APP_WIFI_RETRY_MS);
        xTimerStart(s_retry_timer, 0);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "WIFI UP: ip " IPSTR ", gw " IPSTR, IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
        atomic_store(&s_connected, true);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "lost IP address");
        atomic_store(&s_connected, false);
    }
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

    /* SSID and password are copied as raw bytes: the SSID is UTF-8 and may
     * contain non-ASCII characters. */
    wifi_config_t cfg = {0};
    _Static_assert(sizeof(WIFI_SSID) - 1 <= sizeof(cfg.sta.ssid), "WIFI_SSID longer than 32 bytes");
    _Static_assert(sizeof(WIFI_PASS) - 1 <= sizeof(cfg.sta.password), "WIFI_PASS longer than 64 bytes");
    memcpy(cfg.sta.ssid, WIFI_SSID, sizeof(WIFI_SSID) - 1);
    memcpy(cfg.sta.password, WIFI_PASS, sizeof(WIFI_PASS) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
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
