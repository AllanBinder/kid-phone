/*
 * Email message writer for secret messages (smtp.c sends what this writes).
 *
 * One multipart/mixed message: a short UTF-8 text part and one attachment,
 * both base64. The attachment is encoded straight from the caller's buffer
 * (the recording in PSRAM) a few KB at a time into the sink, so the message
 * is never held in memory as a whole. Header text that is not plain ASCII
 * (the subject's emoji, a name with accents) goes out as RFC 2047 encoded
 * words. Every line ends in CRLF; nothing here starts a line with "." except
 * what the caller puts in the text (the SMTP sink dot-stuffs anyway).
 *
 * Pure C, no ESP-IDF dependency (host-testable).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Writes len bytes; false stops the message (connection gone). */
typedef bool (*mime_sink_t)(void *ctx, const char *data, size_t len);

typedef struct {
    const char *from_name;  /* display name, UTF-8, may be "" */
    const char *from_addr;  /* plain address, no angle brackets */
    const char *to_addr;
    const char *subject;    /* UTF-8 */
    const char *date;       /* RFC 5322 date ("Thu, 29 Oct 2026 19:42:00 -0400") or NULL */
    const char *message_id; /* "<...@...>" or NULL */
    const char *boundary;   /* multipart boundary, ASCII, not in any part */
    const char *text;       /* body text, UTF-8, "\n" line ends */
    const uint8_t *att;     /* attachment bytes, NULL = text only (no multipart) */
    size_t att_len;
    const char *att_name;   /* "secret-20261029-1942.wav" */
    const char *att_type;   /* "audio/wav" */
} mime_msg_t;

/* Base64 of n bytes into out (4 * ceil(n / 3) chars plus a NUL). Returns the
 * length written, not counting the NUL. */
size_t mime_b64(const uint8_t *in, size_t n, char *out);

/* Write the whole message (headers, blank line, body) to sink. Returns false
 * if the sink failed or memory ran out. */
bool mime_write(const mime_msg_t *m, mime_sink_t sink, void *ctx);
