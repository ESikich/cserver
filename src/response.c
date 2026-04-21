/*
 * response.c -- Response builder helpers.
 */

#include <stdio.h>
#include <string.h>

#include "cserve.h"

const char *
cs_status_str(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Content Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}

int
cs_add_header(http_response_t *resp,
              const char *name, const char *value)
{
    size_t avail = sizeof(resp->headers) - resp->headers_len;
    int n = snprintf(resp->headers + resp->headers_len, avail,
                     "%s: %s\r\n", name, value);
    if (n < 0 || (size_t)n >= avail)
        return -1;
    resp->headers_len += (size_t)n;
    return 0;
}

const char *
cs_get_header(const http_request_t *req, conn_t *conn,
              const char *name)
{
    size_t nlen = strlen(name);

    for (int i = 0; i < req->header_count; i++) {
        slice_t hn = req->headers[i].name;
        if (hn.len != nlen)
            continue;

        int match = 1;
        for (size_t j = 0; j < nlen && match; j++) {
            uint8_t a = conn->inbuf[hn.off + j];
            uint8_t b = (uint8_t)name[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) match = 0;
        }
        if (!match)
            continue;

        slice_t hv = req->headers[i].value;
        char *out = cs_arena_alloc(conn, hv.len + 1);
        if (out == NULL)
            return NULL;
        memcpy(out, conn->inbuf + hv.off, hv.len);
        out[hv.len] = '\0';
        return out;
    }
    return NULL;
}
