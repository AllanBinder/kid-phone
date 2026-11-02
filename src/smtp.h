/*
 * Minimal SMTP client for secret messages: one message per connection, sent
 * straight to the parent's own mail provider (smtp.gmail.com and the like)
 * with their login. Nothing passes through any other server.
 *
 *   security SMTP_TLS       TLS from the first byte (port 465)
 *   security SMTP_STARTTLS  plain greeting, then STARTTLS (port 587); a
 *                           server that does not offer it is refused, so
 *                           the password never goes out in the clear
 * Server certificates are checked against the ESP-IDF certificate bundle.
 * Login: AUTH PLAIN, else AUTH LOGIN (whichever the server lists).
 *
 * Blocking, with a timeout on every step; call it from a task with a big
 * stack (the TLS handshake, see APP_SECRET_STACK).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mime.h"

typedef enum {
    SMTP_TLS = 0,
    SMTP_STARTTLS = 1,
} smtp_security_t;

typedef enum {
    SMTP_OK = 0,
    SMTP_ERR_DNS,       /* server name not found */
    SMTP_ERR_CONNECT,   /* no TCP connection (wrong port, firewall, no internet) */
    SMTP_ERR_TLS,       /* TLS handshake or certificate check failed */
    SMTP_ERR_NO_TLS,    /* STARTTLS asked for but not offered */
    SMTP_ERR_AUTH,      /* login refused (wrong user or app password) */
    SMTP_ERR_NO_AUTH,   /* the server offers no password login */
    SMTP_ERR_REJECTED,  /* 5xx on the sender, recipient or message: permanent */
    SMTP_ERR_TEMP,      /* 4xx: try again later */
    SMTP_ERR_IO,        /* timeout or connection dropped */
    SMTP_ERR_NOMEM,
} smtp_err_t;

typedef struct {
    const char *host;
    uint16_t port;
    smtp_security_t security;
    const char *user;
    const char *pass;
    const char *helo;  /* EHLO argument, e.g. "[192.168.1.20]" */
} smtp_account_t;

/* Send one message (envelope from = m->from_addr, to = m->to_addr). detail
 * gets the server's reply line for the step that failed, or a short
 * description (empty on success). */
smtp_err_t smtp_send(const smtp_account_t *a, const mime_msg_t *m, char *detail, size_t detail_len);

/* True for errors that another try cannot fix without a settings change. */
bool smtp_err_permanent(smtp_err_t e);

/* Short log name ("auth", "tls", ...). */
const char *smtp_err_name(smtp_err_t e);
