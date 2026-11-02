#include "mime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Attachment base64: 57 input bytes make one 76 character line (RFC 2045's
 * longest). B64_LINES lines go to the sink per write, about 5 KB. */
#define B64_LINE_IN  57
#define B64_LINES    64
/* Raw bytes per RFC 2047 encoded word: 39 bytes = 52 base64 chars, plus the
 * 12 of "=?UTF-8?B?" and "?=" is 64, so even the first line ("Subject: "
 * and one word) stays under RFC 2047's 76 characters. */
#define WORD_RAW     39

static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t mime_b64(const uint8_t *in, size_t n, char *out)
{
    size_t o = 0, i = 0;
    for (; i + 3 <= n; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        out[o++] = k_b64[(v >> 18) & 63];
        out[o++] = k_b64[(v >> 12) & 63];
        out[o++] = k_b64[(v >> 6) & 63];
        out[o++] = k_b64[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        out[o++] = k_b64[(v >> 18) & 63];
        out[o++] = k_b64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? k_b64[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

static bool put(mime_sink_t sink, void *ctx, const char *s)
{
    return sink(ctx, s, strlen(s));
}

static bool is_ascii_printable(const char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7e) {
            return false;
        }
    }
    return true;
}

/* Header text without control characters (a CR or LF would end the header). */
static void clean(const char *in, char *out, size_t len)
{
    size_t o = 0;
    for (; in && *in && o + 1 < len; in++) {
        out[o++] = (unsigned char)*in < 0x20 || *in == 0x7f ? ' ' : *in;
    }
    out[o] = '\0';
}

/* Length of the UTF-8 sequence starting with byte c (1 for anything odd). */
static size_t utf8_len(unsigned char c)
{
    return c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
}

/* UTF-8 text as encoded words, split between characters, folded onto
 * continuation lines ("\r\n " between words). */
static bool put_words(mime_sink_t sink, void *ctx, const char *s)
{
    char enc[96];
    const size_t n = strlen(s);
    size_t i = 0;
    bool first = true;
    while (i < n) {
        size_t take = 0;
        while (i + take < n) {
            size_t cl = utf8_len((unsigned char)s[i + take]);
            if (i + take + cl > n) {
                cl = n - i - take;
            }
            if (take + cl > WORD_RAW) {
                break;
            }
            take += cl;
        }
        if (!take) {
            take = 1; /* cannot happen with WORD_RAW >= 4, but never loop forever */
        }
        mime_b64((const uint8_t *)s + i, take, enc);
        if ((!first && !put(sink, ctx, "\r\n ")) || !put(sink, ctx, "=?UTF-8?B?") || !put(sink, ctx, enc) ||
            !put(sink, ctx, "?=")) {
            return false;
        }
        first = false;
        i += take;
    }
    return true;
}

/* "Name" <addr>, the name quoted (ASCII) or encoded (anything else). */
static bool put_mailbox(mime_sink_t sink, void *ctx, const char *name, const char *addr)
{
    char nm[128], ad[128];
    clean(name, nm, sizeof(nm));
    clean(addr, ad, sizeof(ad));
    if (nm[0]) {
        if (is_ascii_printable(nm)) {
            char q[2 * sizeof(nm) + 3];
            size_t o = 0;
            q[o++] = '"';
            for (const char *p = nm; *p; p++) {
                if (*p == '"' || *p == '\\') {
                    q[o++] = '\\';
                }
                q[o++] = *p;
            }
            q[o++] = '"';
            q[o] = '\0';
            if (!put(sink, ctx, q)) {
                return false;
            }
        } else if (!put_words(sink, ctx, nm)) {
            return false;
        }
        if (!put(sink, ctx, " ")) {
            return false;
        }
    }
    return put(sink, ctx, "<") && put(sink, ctx, ad) && put(sink, ctx, ">");
}

/* base64 of n bytes in 76 character CRLF lines, B64_LINES lines per write. */
static bool put_b64_lines(mime_sink_t sink, void *ctx, const uint8_t *data, size_t n)
{
    char *buf = malloc(B64_LINES * 78 + 1);
    if (!buf) {
        return false;
    }
    bool ok = true;
    size_t pos = 0;
    while (ok && pos < n) {
        size_t o = 0;
        for (int l = 0; l < B64_LINES && pos < n; l++) {
            const size_t take = n - pos < B64_LINE_IN ? n - pos : B64_LINE_IN;
            o += mime_b64(data + pos, take, buf + o);
            buf[o++] = '\r';
            buf[o++] = '\n';
            pos += take;
        }
        ok = sink(ctx, buf, o);
    }
    free(buf);
    return ok;
}

/* The text with every line ending made CRLF, then base64. */
static bool put_text_b64(mime_sink_t sink, void *ctx, const char *text)
{
    size_t n = 0;
    for (const char *p = text; *p; p++) {
        n += *p == '\n' ? 2 : *p == '\r' ? 0 : 1;
    }
    char *crlf = malloc(n + 1);
    if (!crlf) {
        return false;
    }
    size_t o = 0;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            crlf[o++] = '\r';
            crlf[o++] = '\n';
        } else if (*p != '\r') {
            crlf[o++] = *p;
        }
    }
    const bool ok = put_b64_lines(sink, ctx, (const uint8_t *)crlf, o);
    free(crlf);
    return ok;
}

static bool put_header(mime_sink_t sink, void *ctx, const char *name, const char *value)
{
    char v[256];
    clean(value, v, sizeof(v));
    return put(sink, ctx, name) && put(sink, ctx, ": ") && put(sink, ctx, v) && put(sink, ctx, "\r\n");
}

bool mime_write(const mime_msg_t *m, mime_sink_t sink, void *ctx)
{
    char subj[256];
    clean(m->subject, subj, sizeof(subj));
    bool ok = put(sink, ctx, "From: ") && put_mailbox(sink, ctx, m->from_name, m->from_addr) &&
              put(sink, ctx, "\r\nTo: ") && put_mailbox(sink, ctx, "", m->to_addr) &&
              put(sink, ctx, "\r\nSubject: ");
    if (ok) {
        ok = is_ascii_printable(subj) ? put(sink, ctx, subj) : put_words(sink, ctx, subj);
    }
    ok = ok && put(sink, ctx, "\r\n");
    if (ok && m->date) {
        ok = put_header(sink, ctx, "Date", m->date);
    }
    if (ok && m->message_id) {
        ok = put_header(sink, ctx, "Message-ID", m->message_id);
    }
    ok = ok && put(sink, ctx, "MIME-Version: 1.0\r\n");
    if (!ok) {
        return false;
    }
    if (!m->att) {
        return put(sink, ctx, "Content-Type: text/plain; charset=UTF-8\r\n"
                              "Content-Transfer-Encoding: base64\r\n\r\n") &&
               put_text_b64(sink, ctx, m->text);
    }
    char name[96];
    clean(m->att_name, name, sizeof(name));
    for (char *p = name; *p; p++) {
        if (*p == '"' || *p == '\\') {
            *p = '_';
        }
    }
    return put(sink, ctx, "Content-Type: multipart/mixed; boundary=\"") && put(sink, ctx, m->boundary) &&
           put(sink, ctx, "\"\r\n\r\nThis is a multi-part message in MIME format.\r\n\r\n--") &&
           put(sink, ctx, m->boundary) &&
           put(sink, ctx, "\r\nContent-Type: text/plain; charset=UTF-8\r\n"
                          "Content-Transfer-Encoding: base64\r\n\r\n") &&
           put_text_b64(sink, ctx, m->text) && put(sink, ctx, "\r\n--") && put(sink, ctx, m->boundary) &&
           put(sink, ctx, "\r\nContent-Type: ") && put(sink, ctx, m->att_type) && put(sink, ctx, "; name=\"") &&
           put(sink, ctx, name) && put(sink, ctx, "\"\r\nContent-Transfer-Encoding: base64\r\n"
                                              "Content-Disposition: attachment; filename=\"") &&
           put(sink, ctx, name) && put(sink, ctx, "\"\r\n\r\n") && put_b64_lines(sink, ctx, m->att, m->att_len) &&
           put(sink, ctx, "\r\n--") && put(sink, ctx, m->boundary) && put(sink, ctx, "--\r\n");
}
