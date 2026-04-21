/*
 * router.c -- Route table and request dispatch.
 */

#include <stdio.h>
#include <string.h>

#include "cserve.h"

void
cs_add_route(server_config_t *cfg, const char *method,
             const char *path, handler_fn handler)
{
    if (cfg->route_count >= MAX_ROUTES)
        cs_fatal("route table full (max %d)", MAX_ROUTES);

    route_t *r = &cfg->routes[cfg->route_count++];
    snprintf(r->method, sizeof(r->method), "%s", method);
    snprintf(r->path,   sizeof(r->path),   "%s", path);
    r->handler = handler;
}

/*
 * Match a route. An empty method field matches any method; an empty
 * path field is a catch-all. Routes are checked in registration order.
 */
static handler_fn
find_handler(server_config_t *cfg, const uint8_t *inbuf,
             const http_request_t *req)
{
    for (int i = 0; i < cfg->route_count; i++) {
        route_t *r = &cfg->routes[i];

        if (r->method[0] != '\0') {
            size_t mlen = strlen(r->method);
            if (req->method.len != mlen
                || memcmp(inbuf + req->method.off,
                          r->method, mlen) != 0)
                continue;
        }

        if (r->path[0] != '\0') {
            size_t plen = strlen(r->path);
            if (req->path.len != plen
                || memcmp(inbuf + req->path.off,
                          r->path, plen) != 0)
                continue;
        }

        return r->handler;
    }
    return NULL;
}

int
cs_router_dispatch(server_config_t *cfg, conn_t *conn,
                   const http_request_t *req,
                   http_response_t *resp)
{
    handler_fn h = find_handler(cfg, conn->inbuf, req);

    if (h == NULL) {
        /* No dynamic route: fall back to static file handler */
        if (cfg->docroot[0] != '\0')
            return cs_static_handler(conn, req, resp);

        /* No docroot configured: 404 */
        resp->status = 404;
        return HANDLER_OK;
    }

    return h(conn, req, resp);
}
