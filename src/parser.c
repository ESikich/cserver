/*
 * parser.c -- HTTP/1.1 request parser state machine.
 */

#include <string.h>
#include <stdlib.h>

#include "cserve.h"

#define NEED_MORE     0
#define PARSE_DONE    1
#define PARSE_ERR   (-1)
#define PARSE_ERR_431 (-431)  /* too many header fields */
#define PARSE_ERR_501 (-501)  /* Transfer-Encoding not implemented */

#define REQ_LINE_MAX   8192
#define HEADER_MAX     8192
#define TOTAL_HDR_MAX  65536

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static int
slice_eq_ci(const uint8_t *buf, slice_t s, const char *str)
{
    size_t slen = strlen(str);
    if (s.len != slen)
        return 0;
    for (size_t i = 0; i < slen; i++) {
        uint8_t a = buf[s.off + i];
        uint8_t b = (uint8_t)str[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static uint64_t
parse_uint64(const uint8_t *buf, slice_t s, int *ok)
{
    if (s.len == 0 || s.len > 20) { *ok = 0; return 0; }
    uint64_t v = 0;
    for (size_t i = 0; i < s.len; i++) {
        uint8_t c = buf[s.off + i];
        if (c < '0' || c > '9') { *ok = 0; return 0; }
        uint64_t next = v * 10 + (c - '0');
        if (next < v) { *ok = 0; return 0; } /* overflow */
        v = next;
    }
    *ok = 1;
    return v;
}

/* Trim trailing whitespace from a slice. */
static slice_t
trim_right(const uint8_t *buf, slice_t s)
{
    while (s.len > 0 && (buf[s.off + s.len - 1] == ' '
                         || buf[s.off + s.len - 1] == '\t'))
        s.len--;
    return s;
}

/* ------------------------------------------------------------------ */
/* Post-headers processing                                             */
/* ------------------------------------------------------------------ */

static int
has_transfer_encoding(const uint8_t *buf, const http_request_t *req)
{
    for (int i = 0; i < req->header_count; i++) {
        if (slice_eq_ci(buf, req->headers[i].name, "transfer-encoding"))
            return 1;
    }
    return 0;
}

static void
finalize_headers(parser_t *p, const uint8_t *buf)
{
    int ka_explicit = 0;
    int seen_cl = 0;

    for (int i = 0; i < p->req.header_count; i++) {
        slice_t name  = p->req.headers[i].name;
        slice_t value = p->req.headers[i].value;

        if (slice_eq_ci(buf, name, "content-length")) {
            int ok;
            uint64_t v = parse_uint64(buf, value, &ok);
            if (!ok) {
                p->state = PS_ERROR;
            } else if (seen_cl && v != p->req.content_length) {
                /* RFC 7230 §3.3.3: differing Content-Length values */
                p->state = PS_ERROR;
            } else {
                p->req.content_length = v;
                seen_cl = 1;
            }

        } else if (slice_eq_ci(buf, name, "connection")) {
            ka_explicit = 1;
            if (slice_eq_ci(buf, value, "keep-alive"))
                p->req.keep_alive = 1;
            else
                p->req.keep_alive = 0;

        } else if (slice_eq_ci(buf, name, "if-none-match")) {
            p->req.if_none_match = value;

        } else if (slice_eq_ci(buf, name, "if-modified-since")) {
            p->req.if_modified_since = value;
        }
    }

    if (!ka_explicit) {
        /* HTTP/1.1 is keep-alive by default; 1.0 is not */
        p->req.keep_alive =
            slice_eq_ci(buf, p->req.version, "HTTP/1.1");
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void
cs_parser_init(parser_t *p)
{
    memset(p, 0, sizeof(*p));
    p->state = PS_REQ_METHOD;
}

/*
 * Feed buf[0..len) to the parser. The parser updates p->pos as it
 * consumes bytes. Returns PARSE_DONE (1), NEED_MORE (0), or
 * PARSE_ERR (-1). On PARSE_DONE, p->req is fully populated.
 */
int
cs_parser_feed(parser_t *p, const uint8_t *buf, size_t len)
{
    while (p->pos < len) {
        uint8_t c = buf[p->pos];

        switch (p->state) {

        /* ---------------------------------------------------------- */
        case PS_REQ_METHOD:
            if (c == ' ') {
                p->req.method.off = p->mark;
                p->req.method.len = p->pos - p->mark;
                if (p->req.method.len == 0)
                    return PARSE_ERR;
                p->pos++;
                p->mark = p->pos;
                p->state = PS_REQ_PATH;
            } else if (p->pos - p->mark >= REQ_LINE_MAX) {
                return PARSE_ERR;
            } else {
                p->pos++;
            }
            break;

        /* ---------------------------------------------------------- */
        case PS_REQ_PATH:
            if (c == ' ') {
                p->req.path.off = p->mark;
                p->req.path.len = p->pos - p->mark;
                if (p->req.path.len == 0)
                    return PARSE_ERR;
                p->pos++;
                p->mark = p->pos;
                p->state = PS_REQ_VERSION;
            } else if (p->pos - p->mark >= REQ_LINE_MAX) {
                return PARSE_ERR;
            } else {
                p->pos++;
            }
            break;

        /* ---------------------------------------------------------- */
        case PS_REQ_VERSION:
            if (c == '\r') {
                /* Peek: need \n to be available */
                if (p->pos + 1 >= len)
                    return NEED_MORE;
                if (buf[p->pos + 1] != '\n')
                    return PARSE_ERR;
                p->req.version.off = p->mark;
                p->req.version.len = p->pos - p->mark;
                p->pos += 2; /* consume \r\n */
                p->mark = p->pos;
                p->state = PS_HEADER_NAME;
            } else if (p->pos - p->mark >= REQ_LINE_MAX) {
                return PARSE_ERR;
            } else {
                p->pos++;
            }
            break;

        /* ---------------------------------------------------------- */
        case PS_HEADER_NAME:
            if (c == '\r') {
                /* Blank line: end of headers */
                if (p->pos + 1 >= len)
                    return NEED_MORE;
                if (buf[p->pos + 1] != '\n')
                    return PARSE_ERR;
                p->pos += 2;
                finalize_headers(p, buf);
                if (p->state == PS_ERROR)
                    return PARSE_ERR;
                /* RFC 7230 §3.3.1: chunked TE is not implemented */
                if (has_transfer_encoding(buf, &p->req))
                    return PARSE_ERR_501;
                if (p->req.content_length > 0) {
                    p->mark = p->pos;
                    p->state = PS_BODY;
                } else {
                    p->state = PS_DONE;
                    return PARSE_DONE;
                }
            } else if (c == ':') {
                if (p->req.header_count >= MAX_HEADERS)
                    return PARSE_ERR_431;
                int idx = p->req.header_count;
                p->req.headers[idx].name.off = p->mark;
                p->req.headers[idx].name.len = p->pos - p->mark;
                if (p->req.headers[idx].name.len == 0)
                    return PARSE_ERR;
                p->pos++; /* consume ':' */
                /* Skip optional leading whitespace */
                while (p->pos < len
                       && (buf[p->pos] == ' '
                           || buf[p->pos] == '\t'))
                    p->pos++;
                p->mark = p->pos;
                p->state = PS_HEADER_VALUE;
            } else if (p->pos - p->mark >= HEADER_MAX) {
                return PARSE_ERR;
            } else {
                p->pos++;
            }
            break;

        /* ---------------------------------------------------------- */
        case PS_HEADER_VALUE:
            if (c == '\r') {
                /* Peek: need \n */
                if (p->pos + 1 >= len)
                    return NEED_MORE;
                if (buf[p->pos + 1] != '\n')
                    return PARSE_ERR;

                int idx = p->req.header_count;
                slice_t val = { .off = p->mark,
                                .len = p->pos - p->mark };
                p->req.headers[idx].value = trim_right(buf, val);
                p->req.header_count++;

                p->pos += 2; /* consume \r\n */
                p->mark = p->pos;
                p->state = PS_HEADER_NAME;
            } else if (p->pos - p->mark >= HEADER_MAX) {
                return PARSE_ERR;
            } else {
                p->pos++;
            }
            break;

        /* ---------------------------------------------------------- */
        case PS_BODY: {
            size_t have  = len - p->pos;
            size_t need  = (size_t)p->req.content_length
                         - (p->pos - p->mark);
            if (have >= need) {
                p->req.body.off = p->mark;
                p->req.body.len = (size_t)p->req.content_length;
                p->pos += need;
                p->state = PS_DONE;
                return PARSE_DONE;
            }
            /* Need more data */
            p->pos = len;
            return NEED_MORE;
        }

        /* ---------------------------------------------------------- */
        case PS_DONE:
            return PARSE_DONE;

        case PS_ERROR:
            return PARSE_ERR;

        case PS_HEADER_END:
            /* Not used directly; kept for enum completeness. */
            return PARSE_ERR;
        }
    }
    return NEED_MORE;
}
