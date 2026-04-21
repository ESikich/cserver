/*
 * test_parser.c -- Unit tests for the HTTP parser.
 */

#include <stdio.h>
#include <string.h>

#include "cserve.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s\n", msg); \
        failures++; \
    } \
} while (0)

/* ------------------------------------------------------------------ */

static int
test_get_request(void)
{
    parser_t p;
    cs_parser_init(&p);

    const char *raw =
        "GET /hello HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    int rc = cs_parser_feed(&p, (const uint8_t *)raw, strlen(raw));

    CHECK(rc == 1, "GET: parse done");
    CHECK(p.req.method.len == 3, "GET: method len");
    CHECK(memcmp(raw + p.req.method.off, "GET", 3) == 0,
          "GET: method value");
    CHECK(p.req.path.len == 6, "GET: path len");
    CHECK(memcmp(raw + p.req.path.off, "/hello", 6) == 0,
          "GET: path value");
    CHECK(p.req.version.len == 8, "GET: version len");
    CHECK(p.req.header_count == 2, "GET: header count");
    CHECK(p.req.keep_alive == 1, "GET: keep-alive");
    CHECK(p.req.content_length == 0, "GET: no content-length");
    return 0;
}

static int
test_post_with_body(void)
{
    parser_t p;
    cs_parser_init(&p);

    const char *raw =
        "POST /echo HTTP/1.1\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    int rc = cs_parser_feed(&p, (const uint8_t *)raw, strlen(raw));

    CHECK(rc == 1, "POST: parse done");
    CHECK(p.req.content_length == 5, "POST: content-length");
    CHECK(p.req.body.len == 5, "POST: body len");
    CHECK(memcmp(raw + p.req.body.off, "hello", 5) == 0,
          "POST: body value");
    return 0;
}

static int
test_need_more(void)
{
    parser_t p;
    cs_parser_init(&p);

    const char *part1 = "GET /foo HTTP/1.1\r\n";
    int rc = cs_parser_feed(&p, (const uint8_t *)part1,
                             strlen(part1));
    CHECK(rc == 0, "partial: need more after first chunk");

    const char *part2 = "\r\n";
    /* feed the full buffer (part1 + part2 concatenated) */
    char full[256];
    size_t len1 = strlen(part1);
    size_t len2 = strlen(part2);
    memcpy(full, part1, len1);
    memcpy(full + len1, part2, len2);
    cs_parser_init(&p);
    rc = cs_parser_feed(&p, (const uint8_t *)full, len1 + len2);
    CHECK(rc == 1, "partial: done after full buffer");
    return 0;
}

static int
test_invalid_request(void)
{
    parser_t p;
    cs_parser_init(&p);

    const char *bad = "BADREQUEST\r\n\r\n";
    int rc = cs_parser_feed(&p, (const uint8_t *)bad, strlen(bad));
    /* No space in request line -> PS_REQ_METHOD never terminates */
    /* Will fail at REQ_LINE_MAX or return error */
    /* At minimum it must not return PARSE_DONE (1) */
    CHECK(rc != 1, "invalid: must not parse successfully");
    return 0;
}

static int
test_connection_close(void)
{
    parser_t p;
    cs_parser_init(&p);

    const char *raw =
        "GET / HTTP/1.1\r\n"
        "Connection: close\r\n"
        "\r\n";
    int rc = cs_parser_feed(&p, (const uint8_t *)raw, strlen(raw));

    CHECK(rc == 1, "conn-close: parse done");
    CHECK(p.req.keep_alive == 0, "conn-close: keep_alive is 0");
    return 0;
}

static int
test_transfer_encoding_chunked(void)
{
    parser_t p;
    cs_parser_init(&p);

    const char *raw =
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    int rc = cs_parser_feed(&p, (const uint8_t *)raw, strlen(raw));
    CHECK(rc == -501, "TE-chunked: returns -501");
    return 0;
}

static int
test_duplicate_content_length_same(void)
{
    parser_t p;
    cs_parser_init(&p);

    const char *raw =
        "POST /echo HTTP/1.1\r\n"
        "Content-Length: 5\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    int rc = cs_parser_feed(&p, (const uint8_t *)raw, strlen(raw));
    CHECK(rc == 1, "dup-cl-same: identical values accepted");
    CHECK(p.req.content_length == 5, "dup-cl-same: correct length");
    return 0;
}

static int
test_duplicate_content_length_differ(void)
{
    parser_t p;
    cs_parser_init(&p);

    const char *raw =
        "POST /echo HTTP/1.1\r\n"
        "Content-Length: 5\r\n"
        "Content-Length: 10\r\n"
        "\r\n"
        "hello";
    int rc = cs_parser_feed(&p, (const uint8_t *)raw, strlen(raw));
    CHECK(rc == -1, "dup-cl-differ: differing values rejected");
    return 0;
}

static int
test_max_headers(void)
{
    parser_t p;
    cs_parser_init(&p);

    /* Build a request with exactly MAX_HEADERS headers */
    char buf[65536];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "GET / HTTP/1.1\r\n");
    for (int i = 0; i < MAX_HEADERS; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "X-H%d: v\r\n", i);
        if (pos >= (int)sizeof(buf) - 4) break;
    }
    snprintf(buf + pos, sizeof(buf) - pos, "\r\n");

    /* Must either succeed or fail cleanly (not crash) */
    cs_parser_feed(&p, (const uint8_t *)buf, strlen(buf));
    CHECK(1, "max-headers: did not crash");
    return 0;
}

/* ------------------------------------------------------------------ */

int
main(void)
{
    int t = 0, f_before;

    f_before = failures; test_get_request();
    printf("%s: test_get_request\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    f_before = failures; test_post_with_body();
    printf("%s: test_post_with_body\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    f_before = failures; test_need_more();
    printf("%s: test_need_more\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    f_before = failures; test_invalid_request();
    printf("%s: test_invalid_request\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    f_before = failures; test_connection_close();
    printf("%s: test_connection_close\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    f_before = failures; test_transfer_encoding_chunked();
    printf("%s: test_transfer_encoding_chunked\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    f_before = failures; test_duplicate_content_length_same();
    printf("%s: test_duplicate_content_length_same\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    f_before = failures; test_duplicate_content_length_differ();
    printf("%s: test_duplicate_content_length_differ\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    f_before = failures; test_max_headers();
    printf("%s: test_max_headers\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    printf("test_parser: %d tests, %d failures\n", t, failures);
    return failures > 0 ? 1 : 0;
}
