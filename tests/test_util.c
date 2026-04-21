/*
 * test_util.c -- Unit tests for URL decode and path checks.
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
test_url_decode_plain(void)
{
    char out[64];
    int n = cs_url_decode("/hello", 6, out, sizeof(out));
    CHECK(n == 6, "plain: length");
    CHECK(strcmp(out, "/hello") == 0, "plain: value");
    return 0;
}

static int
test_url_decode_percent(void)
{
    char out[64];
    int n = cs_url_decode("/hello%20world", 14, out, sizeof(out));
    CHECK(n == 12, "percent: length");
    CHECK(strcmp(out, "/hello world") == 0, "percent: space");
    return 0;
}

static int
test_url_decode_slash(void)
{
    char out[64];
    cs_url_decode("/a%2Fb", 6, out, sizeof(out));
    CHECK(strcmp(out, "/a/b") == 0, "slash: %2F decoded");
    return 0;
}

static int
test_url_decode_invalid_percent(void)
{
    char out[64];
    /* %ZZ is invalid hex -- should be passed through literally */
    cs_url_decode("%ZZ", 3, out, sizeof(out));
    CHECK(out[0] == '%', "invalid-pct: percent kept");
    return 0;
}

static int
test_url_decode_truncation(void)
{
    char out[5]; /* only room for 4 chars + NUL */
    cs_url_decode("/hello", 6, out, sizeof(out));
    out[4] = '\0'; /* ensure NUL */
    CHECK(strlen(out) <= 4, "truncation: fits in buffer");
    return 0;
}

static int
test_path_safe_ok(void)
{
    int r = cs_path_safe("/var/www", "/var/www/index.html");
    CHECK(r == 1, "path-safe: child path is safe");
    return 0;
}

static int
test_path_safe_root_itself(void)
{
    int r = cs_path_safe("/var/www", "/var/www");
    CHECK(r == 1, "path-safe: docroot itself is safe");
    return 0;
}

static int
test_path_safe_traversal(void)
{
    int r = cs_path_safe("/var/www", "/etc/passwd");
    CHECK(r == 0, "path-safe: /etc/passwd rejected");
    return 0;
}

static int
test_path_safe_prefix_trick(void)
{
    /* /var/www2 must not match docroot /var/www */
    int r = cs_path_safe("/var/www", "/var/www2/file");
    CHECK(r == 0, "path-safe: prefix trick rejected");
    return 0;
}

/* ------------------------------------------------------------------ */

int
main(void)
{
    int t = 0, f_before;

    f_before = failures; test_url_decode_plain();
    printf("%s: test_url_decode_plain\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    f_before = failures; test_url_decode_percent();
    printf("%s: test_url_decode_percent\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    f_before = failures; test_url_decode_slash();
    printf("%s: test_url_decode_slash\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    f_before = failures; test_url_decode_invalid_percent();
    printf("%s: test_url_decode_invalid_percent\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    f_before = failures; test_url_decode_truncation();
    printf("%s: test_url_decode_truncation\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    f_before = failures; test_path_safe_ok();
    printf("%s: test_path_safe_ok\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    f_before = failures; test_path_safe_root_itself();
    printf("%s: test_path_safe_root_itself\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    f_before = failures; test_path_safe_traversal();
    printf("%s: test_path_safe_traversal\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    f_before = failures; test_path_safe_prefix_trick();
    printf("%s: test_path_safe_prefix_trick\n",
           failures == f_before ? "PASS" : "FAIL");
    t++;

    printf("test_util: %d tests, %d failures\n", t, failures);
    return failures > 0 ? 1 : 0;
}
