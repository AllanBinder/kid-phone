/*
 * Improv Serial (https://www.improv-wifi.com/serial/) on the console UART, so
 * ESP Web Tools can send WiFi credentials right after flashing from a browser.
 *
 * The UART driver is installed on the console port and stdout is switched to
 * it, so log lines and Improv packets never interleave mid-packet. Nothing
 * else reads UART0 RX (the app has no console/REPL).
 */
#pragma once

/* Call after config_store_init() and wifi_start(). */
void improv_start(void);
