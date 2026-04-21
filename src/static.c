/*
 * static.c -- Static file handler, MIME types, sendfile().
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cserve.h"

static char g_docroot[PATH_MAX];

void
cs_static_init(const char *docroot)
{
    if (realpath(docroot, g_docroot) == NULL) {
        cs_log(LOG_ERROR, "docroot '%s': %s",
               docroot, strerror(errno));
        g_docroot[0] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* MIME types                                                          */
/* ------------------------------------------------------------------ */

static const char *
mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL)
        return "application/octet-stream";
    dot++;
    if (strcmp(dot, "html") == 0 || strcmp(dot, "htm") == 0)
        return "text/html; charset=utf-8";
    if (strcmp(dot, "css")  == 0) return "text/css";
    if (strcmp(dot, "js")   == 0) return "application/javascript";
    if (strcmp(dot, "json") == 0) return "application/json";
    if (strcmp(dot, "png")  == 0) return "image/png";
    if (strcmp(dot, "jpg")  == 0 || strcmp(dot, "jpeg") == 0)
        return "image/jpeg";
    if (strcmp(dot, "gif")  == 0) return "image/gif";
    if (strcmp(dot, "svg")  == 0) return "image/svg+xml";
    if (strcmp(dot, "ico")  == 0) return "image/x-icon";
    if (strcmp(dot, "txt")  == 0) return "text/plain; charset=utf-8";
    if (strcmp(dot, "pdf")  == 0) return "application/pdf";
    return "application/octet-stream";
}

/* ------------------------------------------------------------------ */
/* ETag helpers                                                        */
/* ------------------------------------------------------------------ */

/*
 * Compute the ETag for a file: quoted hex of (mtime << 32 | size).
 * Size is truncated to 32 bits per design spec.
 */
static void
make_etag(const struct stat *st, char *buf, size_t size)
{
    uint64_t v = ((uint64_t)st->st_mtime << 32)
               | ((uint64_t)st->st_size & 0xFFFFFFFFu);
    snprintf(buf, size, "\"%llx\"", (unsigned long long)v);
}

/* ------------------------------------------------------------------ */
/* Conditional GET                                                     */
/* ------------------------------------------------------------------ */

/*
 * Returns 1 if the request If-None-Match matches the given ETag string.
 * Handles wildcard (*) and comma-separated lists.
 */
static int
inm_matches(const uint8_t *inbuf, slice_t inm, const char *etag)
{
    char buf[256];
    size_t cplen = inm.len < sizeof(buf) - 1 ? inm.len : sizeof(buf) - 1;
    memcpy(buf, inbuf + inm.off, cplen);
    buf[cplen] = '\0';

    if (strcmp(buf, "*") == 0)
        return 1;
    return strstr(buf, etag) != NULL;
}

/*
 * Returns 1 if the file mtime is not newer than the If-Modified-Since
 * header value, meaning a 304 should be sent.
 */
static int
ims_not_newer(const uint8_t *inbuf, slice_t ims, time_t mtime)
{
    char buf[64];
    size_t cplen = ims.len < sizeof(buf) - 1 ? ims.len : sizeof(buf) - 1;
    memcpy(buf, inbuf + ims.off, cplen);
    buf[cplen] = '\0';

    struct tm tm = {0};
    if (strptime(buf, "%a, %d %b %Y %H:%M:%S GMT", &tm) == NULL)
        return 0;
    time_t ims_time = timegm(&tm);
    return mtime <= ims_time;
}

/* ------------------------------------------------------------------ */
/* Handler                                                             */
/* ------------------------------------------------------------------ */

int
cs_static_handler(conn_t *conn, const http_request_t *req,
                  http_response_t *resp)
{
    /* Static files support GET and HEAD only */
    {
        const uint8_t *m = conn->inbuf + req->method.off;
        size_t mlen = req->method.len;
        int ok = (mlen == 3 && memcmp(m, "GET",  3) == 0)
              || (mlen == 4 && memcmp(m, "HEAD", 4) == 0);
        if (!ok) {
            resp->status = 405;
            cs_add_header(resp, "Allow", "GET, HEAD");
            return HANDLER_OK;
        }
    }

    /* URL-decode the path */
    char decoded[PATH_MAX];
    if (cs_url_decode((const char *)(conn->inbuf + req->path.off),
                      req->path.len,
                      decoded, sizeof(decoded)) < 0) {
        resp->status = 400;
        return HANDLER_OK;
    }

    /* Strip query string */
    char *q = strchr(decoded, '?');
    if (q != NULL)
        *q = '\0';

    /* Build full filesystem path */
    char full[PATH_MAX];
    int n = snprintf(full, sizeof(full), "%s%s", g_docroot, decoded);
    if (n < 0 || (size_t)n >= sizeof(full)) {
        resp->status = 404;
        return HANDLER_OK;
    }

    /* Resolve to canonical path */
    char resolved[PATH_MAX];
    if (realpath(full, resolved) == NULL) {
        resp->status = 404;
        return HANDLER_OK;
    }

    /* Traversal check */
    if (!cs_path_safe(g_docroot, resolved)) {
        resp->status = 403;
        return HANDLER_OK;
    }

    /* Open the file */
    int fd = open(resolved, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        resp->status = (errno == ENOENT) ? 404 : 403;
        return HANDLER_OK;
    }

    /* Stat for size; retry with index.html for directories */
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        resp->status = 404;
        return HANDLER_OK;
    }
    if (S_ISDIR(st.st_mode)) {
        close(fd);
        size_t dlen = strlen(decoded);
        const char *sep = (dlen > 0 && decoded[dlen-1] == '/')
                          ? "" : "/";
        int m = snprintf(full, sizeof(full), "%s%s%sindex.html",
                         g_docroot, decoded, sep);
        if (m < 0 || (size_t)m >= sizeof(full)) {
            resp->status = 404;
            return HANDLER_OK;
        }
        if (realpath(full, resolved) == NULL) {
            resp->status = 404;
            return HANDLER_OK;
        }
        if (!cs_path_safe(g_docroot, resolved)) {
            resp->status = 403;
            return HANDLER_OK;
        }
        fd = open(resolved, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            resp->status = 404;
            return HANDLER_OK;
        }
        if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
            close(fd);
            resp->status = 404;
            return HANDLER_OK;
        }
    } else if (!S_ISREG(st.st_mode)) {
        close(fd);
        resp->status = 404;
        return HANDLER_OK;
    }

    /* Compute caching metadata */
    char etag[64];
    make_etag(&st, etag, sizeof(etag));

    char last_mod[64];
    cs_http_date(st.st_mtime, last_mod, sizeof(last_mod));

    /* Conditional GET: If-None-Match takes precedence */
    if (req->if_none_match.len > 0
        && inm_matches(conn->inbuf, req->if_none_match, etag)) {
        close(fd);
        resp->status = 304;
        cs_add_header(resp, "ETag", etag);
        cs_add_header(resp, "Last-Modified", last_mod);
        return HANDLER_OK;
    }

    /* Conditional GET: If-Modified-Since */
    if (req->if_modified_since.len > 0
        && ims_not_newer(conn->inbuf, req->if_modified_since,
                         st.st_mtime)) {
        close(fd);
        resp->status = 304;
        cs_add_header(resp, "ETag", etag);
        cs_add_header(resp, "Last-Modified", last_mod);
        return HANDLER_OK;
    }

    /* Build 200 response */
    resp->status      = 200;
    resp->file_fd     = fd;
    resp->file_offset = 0;
    resp->file_size   = st.st_size;

    cs_add_header(resp, "Content-Type", mime_type(resolved));

    char clen[32];
    snprintf(clen, sizeof(clen), "%lld", (long long)st.st_size);
    cs_add_header(resp, "Content-Length", clen);
    cs_add_header(resp, "ETag", etag);
    cs_add_header(resp, "Last-Modified", last_mod);

    return HANDLER_OK;
}
