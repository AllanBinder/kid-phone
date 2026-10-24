/*
 * WiFi station with up to CFG_MAX_WIFI saved networks (config_store).
 *
 * Connection policy, run by one manager task so every step is serialized:
 *   - at boot and after every disconnect: scan all channels, then try the
 *     saved networks that are visible, strongest RSSI first (each attempt
 *     keeps WIFI_CONNECT_AP_BY_SIGNAL, so it joins the strongest access point
 *     of that SSID: mesh, extenders). If the scan also saw a hidden network,
 *     or failed, saved networks that were not seen are tried after those
 *     (one of them may be the hidden one).
 *   - a failed attempt (wrong password, timeout, no DHCP) moves on to the next
 *     candidate; when none works, wait APP_WIFI_BACKOFF_MIN_MS, doubling up to
 *     APP_WIFI_BACKOFF_MAX_MS, then rescan. Never gives up.
 *   - a working connection is kept: no roaming to a stronger saved network,
 *     switching only happens after a disconnect (or when the connected network
 *     is removed from the list).
 * With no saved network the radio is started but stays idle until Improv
 * saves one. Power save is off to keep RTP latency low.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_wifi_types.h"

/* Init netif, event loop and WiFi, then start connecting. Non-blocking.
 * Call after config_store_init(). */
void wifi_start(void);

/* True while associated and holding an IPv4 address. */
bool wifi_is_connected(void);

/* Copy the current IPv4 address as dotted text. Returns false if none. */
bool wifi_get_ip(char *buf, size_t len);

/* SSID of the network the phone is on (or trying, while connecting; "" if
 * none) and the current signal in dBm (0 when not connected). Returns
 * wifi_is_connected(). */
bool wifi_current(char *ssid, size_t len, int *rssi);

/* What the manager is doing: "connected", "connecting", "scanning",
 * "waiting" (pause before the next scan), "no networks". */
const char *wifi_state_name(void);

/* Blocking scan (all channels, a few seconds), fills up to max records,
 * strongest first. Safe while connected (the connection stays up; callers
 * refuse it during a call). While the manager is mid-attempt the scan waits
 * for that attempt to finish. Returns the count, -1 on failure. */
int wifi_scan(wifi_ap_record_t *out, int max);

/* Improv: drop whatever the phone is doing and try this network for up to
 * timeout_ms (credentials in RAM only). Blocks. Returns true once it has an
 * IP address; the caller then saves the network. On failure the normal
 * policy resumes with the saved networks. */
bool wifi_try_network(const char *ssid, const char *pass, int timeout_ms);

/* The saved list changed: a new network is tried right away if the phone
 * is offline; if the network it is on was removed, it leaves it (after a
 * short delay so an HTTP reply can get out) and joins the best other one. */
void wifi_networks_changed(void);

/* ---- Connection history (RAM only, cleared at boot) ---- */

typedef enum {
    WIFI_RES_CONNECTED,
    WIFI_RES_WRONG_PASSWORD, /* handshake or authentication failed */
    WIFI_RES_NOT_FOUND,      /* no access point of that name answered */
    WIFI_RES_TIMEOUT,        /* no result within APP_WIFI_ATTEMPT_MS */
    WIFI_RES_NO_IP,          /* associated, but DHCP gave no address in time */
    WIFI_RES_DROPPED,        /* an established connection was lost */
    WIFI_RES_OTHER,          /* see reason (0 = the attempt could not start) */
} wifi_result_t;

/* One attempt (or a drop of an established connection). */
typedef struct {
    char ssid[33];
    bool seen;       /* the latest scan before it saw this network ... */
    int8_t rssi;     /* ... this strong (dBm) */
    uint8_t result;  /* wifi_result_t */
    uint8_t reason;  /* driver disconnect reason, 0 = none */
    bool improv;     /* an Improv Serial try, not a saved network */
    uint32_t at_s;   /* wifi_uptime_s() when it ended */
} wifi_attempt_t;

/* What the phone last knew about one saved network. */
typedef struct {
    bool in_scan;    /* the latest scan saw it */
    bool ever_seen;  /* some scan since boot saw it: rssi, seen_s are valid */
    int8_t rssi;     /* strongest access point in the last scan that saw it */
    uint32_t seen_s;
    bool tried;      /* result, reason, try_s are valid */
    uint8_t result;
    uint8_t reason;
    uint32_t try_s;
} wifi_net_info_t;

/* Seconds since boot (the time base of the history). */
uint32_t wifi_uptime_s(void);

/* Up to max recent attempts, newest first (the phone keeps 8). */
int wifi_attempts(wifi_attempt_t *out, int max);

/* History of one network. Returns false (and a zeroed *out) if nothing is
 * known about it. *scanned (optional) = some scan has finished since boot. */
bool wifi_net_info(const char *ssid, wifi_net_info_t *out, bool *scanned);

/* A network was saved again (new password): forget its last result so a
 * stale "wrong password" does not linger. The attempts list is kept. */
void wifi_history_new_password(const char *ssid);

/* "connected", "wrong_password", "not_found", "timeout", "no_ip",
 * "dropped", "other". */
const char *wifi_result_key(int result);

/* Short plain-English text for a result ("wrong password", ...). */
void wifi_result_text(int result, int reason, char *buf, size_t len);
