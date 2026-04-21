/*
 * test_router.c -- Unit tests for route matching.
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

static int g_hello_called = 0;
static int g_catch_called = 0;

static int
hello_handler(conn_t *conn, const http_request_t *req,
              http_response_t *resp)
{
    (void)conn; (void)req;
    g_hello_called = 1;
    resp->status = 200;
    return HANDLER_OK;
}

static int
catch_handler(conn_t *conn, const http_request_t *req,
              http_response_t *resp)
{
    (void)conn; (void)req;
    g_catch_called = 1;
    resp->status = 200;
    return HANDLER_OK;
}

/* Build a minimal parser_t with method and path set via a real parse */
static void
make_request(parser_t *p, const char *method, const char *path,
             char *buf, size_t bufsz)
{
    cs_parser_init(p);
    snprintf(buf, bufsz,
             "%s %s HTTP/1.1\r\n\r\n", method, path);
    cs_parser_feed(p, (const uint8_t *)buf, strlen(buf));
}

static int
test_exact_match(void)
{
    server_config_t cfg;
    cs_config_defaults(&cfg);
    cfg.docroot[0] = '\0'; /* disable static handler */
    cs_add_route(&cfg, "GET", "/hello", hello_handler);

    conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.response.file_fd = -1;

    char buf[256];
    make_request(&conn.parser, "GET", "/hello",
                 buf, sizeof(buf));
    memcpy(conn.inbuf, buf, strlen(buf));
    conn.inbuf_len = strlen(buf);

    http_response_t resp = { .file_fd = -1 };
    g_hello_called = 0;
    int rc = cs_router_dispatch(&cfg, &conn,
                                &conn.parser.req, &resp);

    CHECK(rc == HANDLER_OK, "exact: dispatch ok");
    CHECK(g_hello_called == 1, "exact: handler called");
    CHECK(resp.status == 200, "exact: status 200");
    return 0;
}

static int
test_catch_all(void)
{
    server_config_t cfg;
    cs_config_defaults(&cfg);
    cfg.docroot[0] = '\0';
    cs_add_route(&cfg, "", "", catch_handler);

    conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.response.file_fd = -1;

    char buf[256];
    make_request(&conn.parser, "POST", "/anything",
                 buf, sizeof(buf));
    memcpy(conn.inbuf, buf, strlen(buf));
    conn.inbuf_len = strlen(buf);

    http_response_t resp = { .file_fd = -1 };
    g_catch_called = 0;
    int rc = cs_router_dispatch(&cfg, &conn,
                                &conn.parser.req, &resp);

    CHECK(rc == HANDLER_OK, "catch-all: dispatch ok");
    CHECK(g_catch_called == 1, "catch-all: handler called");
    return 0;
}

static int
test_no_match_no_docroot(void)
{
    server_config_t cfg;
    cs_config_defaults(&cfg);
    cfg.docroot[0] = '\0';
    cs_add_route(&cfg, "GET", "/specific", hello_handler);

    conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.response.file_fd = -1;

    char buf[256];
    make_request(&conn.parser, "GET", "/other",
                 buf, sizeof(buf));
    memcpy(conn.inbuf, buf, strlen(buf));
    conn.inbuf_len = strlen(buf);

    http_response_t resp = { .file_fd = -1 };
    g_hello_called = 0;
    cs_router_dispatch(&cfg, &conn, &conn.parser.req, &resp);

    CHECK(g_hello_called == 0, "no-match: specific handler not called");
    CHECK(resp.status == 404, "no-match: 404 returned");
    return 0;
}

static int
test_method_filter(void)
{
    server_config_t cfg;
    cs_config_defaults(&cfg);
    cfg.docroot[0] = '\0';
    cs_add_route(&cfg, "POST", "/submit", hello_handler);

    conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.response.file_fd = -1;

    char buf[256];
    /* GET should not match a POST-only route */
    make_request(&conn.parser, "GET", "/submit",
                 buf, sizeof(buf));
    memcpy(conn.inbuf, buf, strlen(buf));
    conn.inbuf_len = strlen(buf);

    http_response_t resp = { .file_fd = -1 };
    g_hello_called = 0;
    cs_router_dispatch(&cfg, &conn, &conn.parser.req, &resp);

    CHECK(g_hello_called == 0, "method-filter: GET skips POST route");
    return 0;
}

/* ------------------------------------------------------------------ */

int
main(void)
{
    int t = 0, f_before;

    f_before = failures; test_exact_match();
    printf("%s: test_exact_match\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    f_before = failures; test_catch_all();
    printf("%s: test_catch_all\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    f_before = failures; test_no_match_no_docroot();
    printf("%s: test_no_match_no_docroot\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    f_before = failures; test_method_filter();
    printf("%s: test_method_filter\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    printf("test_router: %d tests, %d failures\n", t, failures);
    return failures > 0 ? 1 : 0;
}
