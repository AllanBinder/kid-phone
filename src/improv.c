/*
 * Packet: "IMPROV" | version 1 | type | length | data[length] | checksum
 * checksum = sum of all preceding bytes, mod 256. Sent packets are framed as
 * "\n<packet>\n": the browser reader only recognises a packet at the start of
 * a line, and log output may have left a partial line.
 *
 * Types: 0x01 current state, 0x02 error state, 0x03 RPC command (host to
 * device), 0x04 RPC result. RPC data: command | length | payload.
 * Commands handled: 0x01 WiFi settings, 0x02 current state, 0x03 device info,
 * 0x04 scan. Others answer "unknown RPC command" (the spec makes 0x05-0x07
 * optional). There is no authorization step on serial: state is Ready
 * (authorized) until WiFi is up, then Provisioned.
 */
#include "improv.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"

#include "app_config.h"
#include "config_store.h"
#include "wifi.h"

static const char *TAG = "improv";

enum { T_STATE = 0x01, T_ERROR = 0x02, T_RPC = 0x03, T_RESULT = 0x04 };
enum { S_READY = 0x02, S_PROVISIONING = 0x03, S_PROVISIONED = 0x04 };
enum { E_NONE = 0x00, E_INVALID = 0x01, E_UNKNOWN_CMD = 0x02, E_NO_CONNECT = 0x03, E_UNKNOWN = 0xff };
enum { C_WIFI = 0x01, C_STATE = 0x02, C_INFO = 0x03, C_SCAN = 0x04 };

static void send_packet(uint8_t type, const uint8_t *data, uint8_t len)
{
    uint8_t buf[2 + 9 + 255 + 1];
    size_t n = 0;
    buf[n++] = '\n';
    memcpy(buf + n, "IMPROV", 6);
    n += 6;
    buf[n++] = 1;
    buf[n++] = type;
    buf[n++] = len;
    memcpy(buf + n, data, len);
    n += len;
    uint8_t sum = 0;
    for (size_t i = 1; i < n; i++) {
        sum += buf[i];
    }
    buf[n++] = sum;
    buf[n++] = '\n';
    uart_write_bytes(APP_IMPROV_UART, buf, n); /* one call: atomic against log writes */
}

static void send_state(uint8_t s)
{
    send_packet(T_STATE, &s, 1);
}

static void send_error(uint8_t e)
{
    send_packet(T_ERROR, &e, 1);
}

/* RPC result: command | total length | (len | bytes) per string. */
static void send_result(uint8_t cmd, const char *const *strs, int n)
{
    uint8_t d[255];
    size_t len = 2;
    for (int i = 0; i < n; i++) {
        size_t sl = strlen(strs[i]);
        if (len + 1 + sl > sizeof(d)) {
            break;
        }
        d[len++] = (uint8_t)sl;
        memcpy(d + len, strs[i], sl);
        len += sl;
    }
    d[0] = cmd;
    d[1] = (uint8_t)(len - 2);
    send_packet(T_RESULT, d, (uint8_t)len);
}

/* Redirect: the setup page by mDNS name, then by IP for networks without mDNS. */
static void send_redirect(uint8_t cmd)
{
    char ip[16] = "";
    char ip_url[40] = "";
    if (wifi_get_ip(ip, sizeof(ip))) {
        snprintf(ip_url, sizeof(ip_url), "http://%s/", ip);
    }
    const char *strs[] = {"http://" APP_HOSTNAME ".local/", ip_url};
    send_result(cmd, strs, ip_url[0] ? 2 : 1);
}

static void cmd_wifi(const uint8_t *p, uint8_t len)
{
    if (len < 1 || p[0] + 1 > len || p[0] > 32) {
        send_error(E_INVALID);
        return;
    }
    char ssid[33] = "", pass[65] = "";
    uint8_t sl = p[0];
    memcpy(ssid, p + 1, sl);
    uint8_t pl = 0;
    if (1 + sl < len) {
        pl = p[1 + sl];
        if (pl > 64 || 2 + sl + pl > len) {
            send_error(E_INVALID);
            return;
        }
        memcpy(pass, p + 2 + sl, pl);
    }
    ESP_LOGI(TAG, "WiFi settings received for \"%s\"", ssid);
    send_state(S_PROVISIONING);

    /* Remember the old credentials so a typo does not lock the phone out. */
    app_cfg_t old;
    config_get(&old);
    wifi_apply_credentials(ssid, pass);
    int waited = 0;
    while (!wifi_is_connected() && waited < APP_IMPROV_CONNECT_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    if (wifi_is_connected()) {
        config_set_wifi(ssid, pass);
        ESP_LOGI(TAG, "WiFi connected, credentials saved");
        send_state(S_PROVISIONED);
        send_redirect(C_WIFI);
    } else {
        ESP_LOGW(TAG, "could not connect to \"%s\"", ssid);
        send_error(E_NO_CONNECT);
        send_state(S_READY);
        if (old.wifi_ssid[0]) {
            wifi_apply_credentials(old.wifi_ssid, old.wifi_pass);
        }
    }
    config_wipe(&old);
    mbedtls_platform_zeroize(pass, sizeof(pass));
}

static void cmd_scan(void)
{
    static wifi_ap_record_t recs[20];
    int n = wifi_scan(recs, 20);
    for (int i = 0; i < n; i++) {
        if (!recs[i].ssid[0]) {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < i && !dup; j++) {
            dup = strcmp((const char *)recs[j].ssid, (const char *)recs[i].ssid) == 0;
        }
        if (dup) {
            continue; /* records are sorted by RSSI: keep the strongest */
        }
        char rssi[8];
        snprintf(rssi, sizeof(rssi), "%d", recs[i].rssi);
        const char *strs[] = {(const char *)recs[i].ssid, rssi, recs[i].authmode == WIFI_AUTH_OPEN ? "NO" : "YES"};
        send_result(C_SCAN, strs, 3);
    }
    send_result(C_SCAN, NULL, 0);
}

static void handle_rpc(const uint8_t *d, uint8_t len)
{
    if (len < 2 || d[1] + 2 > len) {
        send_error(E_INVALID);
        return;
    }
    const uint8_t cmd = d[0];
    const uint8_t *p = d + 2;
    const uint8_t plen = d[1];
    send_error(E_NONE); /* spec: clear the error state before answering */
    switch (cmd) {
    case C_WIFI:
        cmd_wifi(p, plen);
        break;
    case C_STATE:
        if (wifi_is_connected()) {
            send_state(S_PROVISIONED);
            send_redirect(C_STATE);
        } else {
            send_state(S_READY);
        }
        break;
    case C_INFO: {
        const esp_app_desc_t *app = esp_app_get_description();
        const char *strs[] = {APP_FIRMWARE_NAME, app->version, "ESP32-S3", APP_DEVICE_NAME};
        send_result(C_INFO, strs, 4);
        break;
    }
    case C_SCAN:
        cmd_scan();
        break;
    default:
        send_error(E_UNKNOWN_CMD);
        break;
    }
}

/* Byte-stream parser: hunts for "IMPROV", then collects one packet. */
static void improv_task(void *arg)
{
    (void)arg;
    static const char hdr[] = "IMPROV";
    uint8_t pkt[9 + 255 + 1];
    size_t have = 0;
    size_t need = 0;
    while (1) {
        uint8_t b;
        if (uart_read_bytes(APP_IMPROV_UART, &b, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (have < 6) {
            if (b == (uint8_t)hdr[have]) {
                pkt[have++] = b;
            } else {
                have = b == 'I' ? 1 : 0;
                pkt[0] = 'I';
            }
            continue;
        }
        pkt[have++] = b;
        if (have == 9) {
            if (pkt[6] != 1) { /* unsupported version */
                have = 0;
                continue;
            }
            need = 9 + pkt[8] + 1;
        }
        if (have >= 9 && have == need) {
            uint8_t sum = 0;
            for (size_t i = 0; i < need - 1; i++) {
                sum += pkt[i];
            }
            if (sum != pkt[need - 1]) {
                ESP_LOGW(TAG, "bad checksum, packet dropped");
                send_error(E_INVALID);
            } else if (pkt[7] == T_RPC) {
                handle_rpc(pkt + 9, pkt[8]);
            }
            mbedtls_platform_zeroize(pkt, sizeof(pkt)); /* may have held a WiFi password */
            have = 0;
        }
    }
}

void improv_start(void)
{
    /* RX buffer only; TX writes go straight to the FIFO under the driver lock. */
    esp_err_t err = uart_driver_install(APP_IMPROV_UART, APP_IMPROV_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(err));
        return;
    }
    /* Route stdout (ESP_LOG) through the driver too, so a log write and an
     * Improv packet can never interleave byte by byte. */
    uart_vfs_dev_use_driver(APP_IMPROV_UART);
    xTaskCreatePinnedToCore(improv_task, "improv", 4096, NULL, 3, NULL, 0);
    ESP_LOGI(TAG, "Improv Serial ready on UART%d", APP_IMPROV_UART);
}
