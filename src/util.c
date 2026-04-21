/*
 * util.c -- URL decode, path safety check, string utilities.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <time.h>

#include "cserve.h"

uint64_t
cs_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000
         + (uint64_t)ts.tv_nsec / 1000000;
}

static int
hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int
cs_url_decode(const char *in, size_t in_len,
              char *out, size_t out_size)
{
    if (out_size == 0)
        return -1;

    size_t o = 0;
    for (size_t i = 0; i < in_len && o < out_size - 1; i++) {
        if (in[i] == '%' && i + 2 < in_len) {
            int hi = hex_val(in[i + 1]);
            int lo = hex_val(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out[o++] = in[i];
    }
    out[o] = '\0';
    return (int)o;
}

/*
 * Format time_t as an RFC 7231 HTTP-date into buf.
 * Returns the number of bytes written (excluding NUL), or -1 on error.
 */
int
cs_http_date(time_t t, char *buf, size_t size)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    size_t n = strftime(buf, size,
                        "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return (n > 0) ? (int)n : -1;
}

/*
 * Returns 1 if resolved_path is safely within docroot, 0 otherwise.
 * Caller must have already run realpath() on the path.
 */
int
cs_path_safe(const char *docroot, const char *resolved)
{
    size_t rootlen = strlen(docroot);
    if (strncmp(resolved, docroot, rootlen) != 0)
        return 0;
    /* resolved must be docroot itself or a child path */
    return resolved[rootlen] == '/' || resolved[rootlen] == '\0';
}
