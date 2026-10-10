/*
 * Setup web page served by the phone on the home network:
 * http://kidphone.local/ (mDNS) or http://<ip>/.
 *
 *   GET  /                 the setup page (one self-contained HTML file in flash)
 *   GET  /api/status       WiFi, Twilio, contacts with availability, clock,
 *                          handset, call and setup progress (JSON)
 *   POST /api/provision    {account_sid, auth_token, contacts}: Twilio setup;
 *                          only the numbers if the phone is already set up
 *   POST /api/contacts     {contacts}: names, button order and schedules, no
 *                          token; 409 needs_token if the set of numbers changed
 *   POST /api/call         {contact}: test call (same rules as that button)
 *   POST /api/hangup       hang up
 *
 * No secret is ever returned. POST bodies must be application/json, which a
 * cross-site page cannot send without a CORS preflight this server never
 * answers; the Host header must be kidphone.local or an IP (DNS rebinding).
 */
#pragma once

/* Start mDNS and the HTTP server. Call after wifi_start(). */
void setup_server_start(void);
