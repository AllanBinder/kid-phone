/*
 * WiFi station: connects to the SSID stored in config_store (NVS) at boot and
 * reconnects forever on any disconnect. With no credentials it starts the
 * radio but stays idle (Improv can scan and then apply credentials).
 * Power save is off to keep RTP latency low.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_wifi_types.h"

/* Init NVS, netif, event loop and WiFi, then start connecting. Non-blocking. */
void wifi_start(void);

/* True while associated and holding an IPv4 address. */
bool wifi_is_connected(void);

/* Copy the current IPv4 address as dotted text. Returns false if none. */
bool wifi_get_ip(char *buf, size_t len);

/* Switch to new credentials now (RAM only, the caller persists them on
 * success) and start connecting. */
void wifi_apply_credentials(const char *ssid, const char *pass);

/* SSID currently targeted ("" if none). */
const char *wifi_ssid(void);

/* Blocking scan, fills up to max records. Returns the count (0 on error). */
int wifi_scan(wifi_ap_record_t *out, int max);
