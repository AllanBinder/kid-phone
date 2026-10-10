/*
 * WiFi station: connects to WIFI_SSID (secrets.h) at boot and reconnects
 * forever on any disconnect. Power save is off to keep RTP latency low.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Init NVS, netif, event loop and WiFi, then start connecting. Non-blocking. */
void wifi_start(void);

/* True while associated and holding an IPv4 address. */
bool wifi_is_connected(void);

/* Copy the current IPv4 address as dotted text. Returns false if none. */
bool wifi_get_ip(char *buf, size_t len);
