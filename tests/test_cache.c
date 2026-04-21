/*
 * test_cache.c -- Tests for ETag, Last-Modified, and conditional GET.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cserve.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s\n", msg); \
        failures++; \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static char g_tmpdir[256];
static char g_tmpfile[300];

static int
setup_tmpfile(const char *content)
{
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/cserve_test_%d", getpid());
    if (mkdir(g_tmpdir, 0755) < 0 && errno != EEXIST)
        return -1;
    snprintf(g_tmpfile, sizeof(g_tmpfile), "%s/test.txt", g_tmpdir);
    int fd = open(g_tmpfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, content, strlen(content));
    close(fd);
    return (n == (ssize_t)strlen(content)) ? 0 : -1;
}

static void
teardown_tmpfile(void)
{
    unlink(g_tmpfile);
    rmdir(g_tmpdir);
}

/*
 * Build a minimal conn_t whose inbuf contains path and optional
 * caching headers so cs_static_handler() can read slices from it.
 */
static void
build_conn(conn_t *conn, const char *path,
           const char *inm_val, const char *ims_val)
{
    memset(conn, 0, sizeof(*conn));
    conn->response.file_fd = -1;
    conn->fd = -1;

    /* Method: always GET by default */
    conn->parser.req.method.off = 0;
    conn->parser.req.method.len = 3;
    memcpy(conn->inbuf, "GET", 3);
    conn->inbuf_len = 3;

    size_t plen = strlen(path);
    conn->parser.req.path.off = conn->inbuf_len;
    conn->parser.req.path.len = plen;
    memcpy(conn->inbuf + conn->inbuf_len, path, plen);
    conn->inbuf_len += plen;

    if (inm_val != NULL) {
        size_t vlen = strlen(inm_val);
        conn->parser.req.if_none_match.off = conn->inbuf_len;
        conn->parser.req.if_none_match.len = vlen;
        memcpy(conn->inbuf + conn->inbuf_len, inm_val, vlen);
        conn->inbuf_len += vlen;
    }

    if (ims_val != NULL) {
        size_t vlen = strlen(ims_val);
        conn->parser.req.if_modified_since.off = conn->inbuf_len;
        conn->parser.req.if_modified_since.len = vlen;
        memcpy(conn->inbuf + conn->inbuf_len, ims_val, vlen);
        conn->inbuf_len += vlen;
    }
}

/* ------------------------------------------------------------------ */
/* cs_http_date tests                                                  */
/* ------------------------------------------------------------------ */

static int
test_http_date_epoch(void)
{
    char buf[64];
    int n = cs_http_date(0, buf, sizeof(buf));
    CHECK(n > 0, "http_date epoch: returns positive");
    CHECK(strcmp(buf, "Thu, 01 Jan 1970 00:00:00 GMT") == 0,
          "http_date epoch: correct format");
    return 0;
}

static int
test_http_date_known(void)
{
    /* 2001-09-09 01:46:40 UTC = 1000000000 seconds */
    char buf[64];
    int n = cs_http_date(1000000000, buf, sizeof(buf));
    CHECK(n > 0, "http_date known: returns positive");
    CHECK(strcmp(buf, "Sun, 09 Sep 2001 01:46:40 GMT") == 0,
          "http_date known: correct value");
    return 0;
}

static int
test_http_date_small_buf(void)
{
    char buf[4];
    int n = cs_http_date(0, buf, sizeof(buf));
    CHECK(n < 0, "http_date small_buf: returns error");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Static handler 200 with caching headers                             */
/* ------------------------------------------------------------------ */

static int
test_static_200_has_etag_and_last_modified(void)
{
    if (setup_tmpfile("hello") < 0) {
        printf("SKIP: test_static_200_has_etag_and_last_modified "
               "(tmp file setup failed)\n");
        return 0;
    }

    cs_static_init(g_tmpdir);

    conn_t *conn = cs_conn_alloc();
    build_conn(conn, "/test.txt", NULL, NULL);

    http_response_t resp = { .file_fd = -1 };
    int rc = cs_static_handler(conn, &conn->parser.req, &resp);

    CHECK(rc == HANDLER_OK, "200 has-etag: handler returns OK");
    CHECK(resp.status == 200, "200 has-etag: status is 200");
    CHECK(strstr(resp.headers, "ETag:") != NULL,
          "200 has-etag: ETag header present");
    CHECK(strstr(resp.headers, "Last-Modified:") != NULL,
          "200 has-etag: Last-Modified header present");

    if (resp.file_fd >= 0)
        close(resp.file_fd);
    cs_conn_free(conn);
    teardown_tmpfile();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Conditional GET: If-None-Match                                      */
/* ------------------------------------------------------------------ */

static int
test_static_304_inm_match(void)
{
    if (setup_tmpfile("hello") < 0) {
        printf("SKIP: test_static_304_inm_match "
               "(tmp file setup failed)\n");
        return 0;
    }

    cs_static_init(g_tmpdir);

    /* First request: get the ETag */
    conn_t *conn = cs_conn_alloc();
    build_conn(conn, "/test.txt", NULL, NULL);

    http_response_t resp = { .file_fd = -1 };
    cs_static_handler(conn, &conn->parser.req, &resp);

    char etag[64] = "";
    const char *ep = strstr(resp.headers, "ETag: ");
    if (ep != NULL) {
        ep += 6;
        size_t i = 0;
        while (ep[i] != '\r' && ep[i] != '\0' && i < sizeof(etag) - 1) {
            etag[i] = ep[i];
            i++;
        }
        etag[i] = '\0';
    }

    if (resp.file_fd >= 0)
        close(resp.file_fd);
    cs_conn_free(conn);

    CHECK(etag[0] != '\0', "inm-match: first request returned ETag");

    /* Second request: If-None-Match matches */
    conn = cs_conn_alloc();
    build_conn(conn, "/test.txt", etag, NULL);

    http_response_t resp2 = { .file_fd = -1 };
    int rc = cs_static_handler(conn, &conn->parser.req, &resp2);

    CHECK(rc == HANDLER_OK, "inm-match: handler returns OK");
    CHECK(resp2.status == 304, "inm-match: status is 304");
    CHECK(resp2.file_fd < 0, "inm-match: no file_fd on 304");
    CHECK(resp2.body == NULL, "inm-match: no body on 304");

    cs_conn_free(conn);
    teardown_tmpfile();
    return 0;
}

static int
test_static_200_inm_no_match(void)
{
    if (setup_tmpfile("hello") < 0) {
        printf("SKIP: test_static_200_inm_no_match "
               "(tmp file setup failed)\n");
        return 0;
    }

    cs_static_init(g_tmpdir);

    conn_t *conn = cs_conn_alloc();
    build_conn(conn, "/test.txt", "\"deadbeef\"", NULL);

    http_response_t resp = { .file_fd = -1 };
    int rc = cs_static_handler(conn, &conn->parser.req, &resp);

    CHECK(rc == HANDLER_OK, "inm-no-match: handler returns OK");
    CHECK(resp.status == 200, "inm-no-match: status is 200");

    if (resp.file_fd >= 0)
        close(resp.file_fd);
    cs_conn_free(conn);
    teardown_tmpfile();
    return 0;
}

static int
test_static_304_inm_wildcard(void)
{
    if (setup_tmpfile("hello") < 0) {
        printf("SKIP: test_static_304_inm_wildcard "
               "(tmp file setup failed)\n");
        return 0;
    }

    cs_static_init(g_tmpdir);

    conn_t *conn = cs_conn_alloc();
    build_conn(conn, "/test.txt", "*", NULL);

    http_response_t resp = { .file_fd = -1 };
    int rc = cs_static_handler(conn, &conn->parser.req, &resp);

    CHECK(rc == HANDLER_OK, "inm-wildcard: handler returns OK");
    CHECK(resp.status == 304, "inm-wildcard: status is 304");

    cs_conn_free(conn);
    teardown_tmpfile();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Conditional GET: If-Modified-Since                                  */
/* ------------------------------------------------------------------ */

static int
test_static_304_ims_future(void)
{
    if (setup_tmpfile("hello") < 0) {
        printf("SKIP: test_static_304_ims_future "
               "(tmp file setup failed)\n");
        return 0;
    }

    cs_static_init(g_tmpdir);

    /* Set If-Modified-Since to year 2100 -- always in the future */
    conn_t *conn = cs_conn_alloc();
    build_conn(conn, "/test.txt", NULL,
               "Thu, 01 Jan 2100 00:00:00 GMT");

    http_response_t resp = { .file_fd = -1 };
    int rc = cs_static_handler(conn, &conn->parser.req, &resp);

    CHECK(rc == HANDLER_OK, "ims-future: handler returns OK");
    CHECK(resp.status == 304, "ims-future: status is 304");
    CHECK(resp.file_fd < 0, "ims-future: no file_fd on 304");

    cs_conn_free(conn);
    teardown_tmpfile();
    return 0;
}

static int
test_static_200_ims_past(void)
{
    if (setup_tmpfile("hello") < 0) {
        printf("SKIP: test_static_200_ims_past "
               "(tmp file setup failed)\n");
        return 0;
    }

    cs_static_init(g_tmpdir);

    /* Set If-Modified-Since to epoch -- always in the past */
    conn_t *conn = cs_conn_alloc();
    build_conn(conn, "/test.txt", NULL,
               "Thu, 01 Jan 1970 00:00:00 GMT");

    http_response_t resp = { .file_fd = -1 };
    int rc = cs_static_handler(conn, &conn->parser.req, &resp);

    CHECK(rc == HANDLER_OK, "ims-past: handler returns OK");
    CHECK(resp.status == 200, "ims-past: status is 200");

    if (resp.file_fd >= 0)
        close(resp.file_fd);
    cs_conn_free(conn);
    teardown_tmpfile();
    return 0;
}

/* ------------------------------------------------------------------ */
/* 405 Method Not Allowed                                              */
/* ------------------------------------------------------------------ */

static int
test_static_405_post(void)
{
    if (setup_tmpfile("hello") < 0) {
        printf("SKIP: test_static_405_post (tmp file setup failed)\n");
        return 0;
    }

    cs_static_init(g_tmpdir);

    conn_t *conn = cs_conn_alloc();
    build_conn(conn, "/test.txt", NULL, NULL);

    /* Override method to POST */
    const char *method = "POST";
    conn->parser.req.method.off = conn->inbuf_len;
    conn->parser.req.method.len = 4;
    memcpy(conn->inbuf + conn->inbuf_len, method, 4);
    conn->inbuf_len += 4;

    http_response_t resp = { .file_fd = -1 };
    int rc = cs_static_handler(conn, &conn->parser.req, &resp);

    CHECK(rc == HANDLER_OK, "405-post: handler returns OK");
    CHECK(resp.status == 405, "405-post: status is 405");
    CHECK(strstr(resp.headers, "Allow:") != NULL,
          "405-post: Allow header present");
    CHECK(resp.file_fd < 0, "405-post: no file opened");

    cs_conn_free(conn);
    teardown_tmpfile();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test runner                                                         */
/* ------------------------------------------------------------------ */

int
main(void)
{
    cs_log_set_level(LOG_OFF);
    cs_conn_pool_init(8);

    int t = 0, f_before;

#define RUN(fn) do { \
    f_before = failures; fn(); \
    printf("%s: " #fn "\n", failures == f_before ? "PASS" : "FAIL"); \
    t++; \
} while (0)

    RUN(test_http_date_epoch);
    RUN(test_http_date_known);
    RUN(test_http_date_small_buf);
    RUN(test_static_200_has_etag_and_last_modified);
    RUN(test_static_304_inm_match);
    RUN(test_static_200_inm_no_match);
    RUN(test_static_304_inm_wildcard);
    RUN(test_static_304_ims_future);
    RUN(test_static_200_ims_past);
    RUN(test_static_405_post);

#undef RUN

    printf("test_cache: %d tests, %d failures\n", t, failures);
    return failures > 0 ? 1 : 0;
}
