/*
 * server.c -- epoll event loop, listen socket, connection dispatch.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "cserve.h"

#define MAX_EVENTS  64

static int              g_listen_fd     = -1;
static server_config_t *g_cfg           = NULL;
static int              g_shutting_down = 0;
static uint64_t         g_shutdown_ms   = 0;

#define SHUTDOWN_GRACE_MS 5000

/* Sentinels stored by address so data.ptr can distinguish event types */
static int g_listen_sentinel;
static int g_signal_sentinel;

/* ------------------------------------------------------------------ */
/* Socket and signal setup                                             */
/* ------------------------------------------------------------------ */

static int
make_listen_socket(server_config_t *cfg)
{
    int fd = socket(AF_INET,
                    SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        cs_fatal("socket: %s", strerror(errno));

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   &opt, sizeof(opt)) < 0)
        cs_fatal("setsockopt SO_REUSEADDR: %s", strerror(errno));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(cfg->port),
        .sin_addr.s_addr = inet_addr(cfg->host),
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        cs_fatal("bind %s:%u: %s",
                 cfg->host, cfg->port, strerror(errno));

    if (listen(fd, SOMAXCONN) < 0)
        cs_fatal("listen: %s", strerror(errno));

    return fd;
}

static int
make_signalfd(void)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);

    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
        cs_fatal("sigprocmask: %s", strerror(errno));

    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd < 0)
        cs_fatal("signalfd: %s", strerror(errno));

    return sfd;
}

/* ------------------------------------------------------------------ */
/* Output buffer helpers                                               */
/* ------------------------------------------------------------------ */

static void
outbuf_append(conn_t *conn, const uint8_t *data, size_t len)
{
    size_t free_space = (conn->outbuf_head - conn->outbuf_tail - 1
                         + OUTBUF_SIZE) % OUTBUF_SIZE;
    if (len > free_space)
        len = free_space;
    if (len == 0)
        return;

    size_t first = OUTBUF_SIZE - conn->outbuf_tail;
    if (first > len)
        first = len;
    memcpy(conn->outbuf + conn->outbuf_tail, data, first);
    if (len > first)
        memcpy(conn->outbuf, data + first, len - first);
    conn->outbuf_tail = (conn->outbuf_tail + len) % OUTBUF_SIZE;
}

/*
 * Drain outbuf to the socket. Returns 0 if empty, 1 if EAGAIN, -1 on
 * error.
 */
static int
outbuf_drain(conn_t *conn)
{
    while (conn->outbuf_head != conn->outbuf_tail) {
        struct iovec iov[2];
        int niov;

        if (conn->outbuf_tail > conn->outbuf_head) {
            iov[0].iov_base = conn->outbuf + conn->outbuf_head;
            iov[0].iov_len  = conn->outbuf_tail - conn->outbuf_head;
            niov = 1;
        } else {
            iov[0].iov_base = conn->outbuf + conn->outbuf_head;
            iov[0].iov_len  = OUTBUF_SIZE - conn->outbuf_head;
            iov[1].iov_base = conn->outbuf;
            iov[1].iov_len  = conn->outbuf_tail;
            niov = 2;
        }

        ssize_t n = writev(conn->fd, iov, niov);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 1;
            cs_log(LOG_ERROR, "writev fd=%d: %s",
                   conn->fd, strerror(errno));
            return -1;
        }
        conn->outbuf_head =
            (conn->outbuf_head + (size_t)n) % OUTBUF_SIZE;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Connection helpers                                                  */
/* ------------------------------------------------------------------ */

static void
conn_close(int epfd, conn_t *conn)
{
    cs_log(LOG_DEBUG, "conn fd=%d closing", conn->fd);
    if (conn->response.file_fd >= 0) {
        close(conn->response.file_fd);
        conn->response.file_fd = -1;
    }
    epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
    shutdown(conn->fd, SHUT_RDWR);
    close(conn->fd);
    cs_conn_free(conn);
}

static void
conn_set_events(int epfd, conn_t *conn, uint32_t ev_flags)
{
    struct epoll_event ev = {
        .events   = ev_flags | EPOLLET,
        .data.ptr = conn,
    };
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, conn->fd, &ev) < 0) {
        cs_log(LOG_ERROR, "epoll_ctl mod fd=%d: %s",
               conn->fd, strerror(errno));
        conn_close(epfd, conn);
    }
}

/* ------------------------------------------------------------------ */
/* Response writing                                                    */
/* ------------------------------------------------------------------ */

static void conn_dispatch(int epfd, conn_t *conn);

static void
conn_reset(int epfd, conn_t *conn)
{
    size_t consumed = conn->parser.pos;
    if (consumed < conn->inbuf_len) {
        memmove(conn->inbuf, conn->inbuf + consumed,
                conn->inbuf_len - consumed);
        conn->inbuf_len -= consumed;
    } else {
        conn->inbuf_len = 0;
    }

    cs_parser_init(&conn->parser);
    conn->arena_pos      = 0;
    conn->response       = (http_response_t){ .file_fd = -1 };
    conn->state          = CONN_READING_REQUEST;
    conn->last_active_ms = cs_now_ms();

    /* Process any pipelined request already in inbuf */
    if (conn->inbuf_len > 0) {
        int rc = cs_parser_feed(&conn->parser,
                                conn->inbuf, conn->inbuf_len);
        if (rc == 1) {
            conn_dispatch(epfd, conn);
            return;
        }
        if (rc < 0) {
            /* Bad pipelined request; just close */
            conn_close(epfd, conn);
            return;
        }
    }
    conn_set_events(epfd, conn, EPOLLIN);
}

static void
conn_write_response(int epfd, conn_t *conn)
{
    int rc = outbuf_drain(conn);
    if (rc < 0) {
        conn_close(epfd, conn);
        return;
    }

    /* sendfile for file responses, once headers are flushed */
    if (rc == 0 && conn->response.file_fd >= 0) {
        off_t rem = conn->response.file_size
                  - conn->response.file_offset;
        while (rem > 0) {
            ssize_t n = sendfile(conn->fd, conn->response.file_fd,
                                 &conn->response.file_offset, rem);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    conn_set_events(epfd, conn,
                                    EPOLLIN | EPOLLOUT);
                    return;
                }
                conn_close(epfd, conn);
                return;
            }
            rem -= n;
        }
        close(conn->response.file_fd);
        conn->response.file_fd = -1;
    }

    if (rc == 1) {
        conn_set_events(epfd, conn, EPOLLIN | EPOLLOUT);
        return;
    }

    /* Response fully sent -- emit access log line */
    {
        size_t body = (conn->response.file_size > 0)
            ? (size_t)conn->response.file_size
            : conn->response.body_len;
        size_t sent = conn->response.headers_len + body;

        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        char addr[INET_ADDRSTRLEN] = "?";
        if (getpeername(conn->fd,
                        (struct sockaddr *)&peer, &plen) == 0)
            inet_ntop(AF_INET, &peer.sin_addr,
                      addr, sizeof(addr));

        cs_log(LOG_INFO, "%.*s %.*s %d %zu %s",
               (int)conn->parser.req.method.len,
               conn->inbuf + conn->parser.req.method.off,
               (int)conn->parser.req.path.len,
               conn->inbuf + conn->parser.req.path.off,
               conn->response.status, sent, addr);
    }

    if (conn->parser.req.keep_alive)
        conn_reset(epfd, conn);
    else
        conn_close(epfd, conn);
}

static void
conn_send_error(int epfd, conn_t *conn, int status)
{
    const char *reason = cs_status_str(status);
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s\r\n",
        status, reason, strlen(reason) + 2, reason);
    if (n > 0)
        outbuf_append(conn, (uint8_t *)buf, (size_t)n);
    outbuf_drain(conn);
    conn_close(epfd, conn);
}

static void
resp_write_status_and_headers(conn_t *conn)
{
    http_response_t *resp = &conn->response;
    char line[64];
    int n = snprintf(line, sizeof(line), "HTTP/1.1 %d %s\r\n",
                     resp->status, cs_status_str(resp->status));
    if (n > 0)
        outbuf_append(conn, (uint8_t *)line, (size_t)n);
    if (resp->headers_len > 0)
        outbuf_append(conn, (uint8_t *)resp->headers,
                      resp->headers_len);
    outbuf_append(conn, (uint8_t *)"\r\n", 2);
    if (resp->body != NULL && resp->body_len > 0)
        outbuf_append(conn, resp->body, resp->body_len);
}

static void
conn_dispatch(int epfd, conn_t *conn)
{
    conn->response = (http_response_t){ .file_fd = -1 };
    conn->state    = CONN_DISPATCHING;

    /* Enforce body size limit before calling any handler */
    if (conn->parser.req.content_length > g_cfg->max_body_bytes) {
        conn->parser.req.keep_alive = 0;
        conn_send_error(epfd, conn, 413);
        return;
    }

    int rc = cs_router_dispatch(g_cfg, conn,
                                &conn->parser.req,
                                &conn->response);
    if (rc == HANDLER_ERR) {
        conn_send_error(epfd, conn, 500);
        return;
    }

    /* For error-status responses with no body, synthesize one */
    if (conn->response.status >= 400
        && conn->response.body == NULL
        && conn->response.file_fd < 0) {
        const char *reason = cs_status_str(conn->response.status);
        size_t rlen = strlen(reason);
        uint8_t *body = cs_arena_alloc(conn, rlen + 3);
        if (body != NULL) {
            memcpy(body, reason, rlen);
            body[rlen]     = '\r';
            body[rlen + 1] = '\n';
            body[rlen + 2] = '\0';
            conn->response.body     = body;
            conn->response.body_len = rlen + 2;
        }
        char clen[32];
        snprintf(clen, sizeof(clen), "%zu",
                 conn->response.body_len);
        cs_add_header(&conn->response, "Content-Type", "text/plain");
        cs_add_header(&conn->response, "Content-Length", clen);
        conn->parser.req.keep_alive = 0;
    }

    cs_add_header(&conn->response,
                  "Connection",
                  conn->parser.req.keep_alive ? "keep-alive"
                                              : "close");

    conn->state = CONN_WRITING_RESPONSE;
    resp_write_status_and_headers(conn);
    conn_write_response(epfd, conn);
}

/* ------------------------------------------------------------------ */
/* Accept and connection event handlers                                */
/* ------------------------------------------------------------------ */

static void
handle_accept(int epfd)
{
    while (1) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int fd = accept4(g_listen_fd,
                         (struct sockaddr *)&addr, &addrlen,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            cs_log(LOG_ERROR, "accept4: %s", strerror(errno));
            break;
        }

        conn_t *conn = cs_conn_alloc();
        if (conn == NULL) {
            cs_log(LOG_ERROR,
                   "conn pool exhausted, dropping fd=%d", fd);
            /* Send 503 before closing -- best effort, no epoll */
            const char *msg =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 19\r\n"
                "Retry-After: 1\r\n"
                "Connection: close\r\n"
                "\r\n"
                "Service Unavailable";
            /* best effort; ignore partial write on pool exhaustion */
            if (write(fd, msg, strlen(msg)) < 0) {}
            close(fd);
            continue;
        }

        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                   &nodelay, sizeof(nodelay));

        conn->fd             = fd;
        conn->state          = CONN_READING_REQUEST;
        conn->last_active_ms = cs_now_ms();
        cs_parser_init(&conn->parser);

        struct epoll_event ev = {
            .events   = EPOLLIN | EPOLLET,
            .data.ptr = conn,
        };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            cs_log(LOG_ERROR, "epoll_ctl add fd=%d: %s",
                   fd, strerror(errno));
            cs_conn_free(conn);
            close(fd);
            continue;
        }

        cs_log(LOG_DEBUG, "accepted fd=%d from %s:%u",
               fd, inet_ntoa(addr.sin_addr),
               ntohs(addr.sin_port));
    }
}

static void
handle_conn_event(int epfd, conn_t *conn, uint32_t events)
{
    if (events & (EPOLLERR | EPOLLHUP)) {
        conn_close(epfd, conn);
        return;
    }

    if (conn->state == CONN_WRITING_RESPONSE) {
        if (events & EPOLLOUT)
            conn_write_response(epfd, conn);
        return;
    }

    /* CONN_READING_REQUEST */
    if (!(events & EPOLLIN))
        return;

    /* Drain socket into inbuf */
    int eof = 0;
    while (1) {
        size_t space = INBUF_SIZE - conn->inbuf_len;
        if (space == 0) {
            /* Buffer full: headers too large (431) or body too large
             * if we're past the header stage (413). */
            int status = (conn->parser.state == PS_BODY) ? 413 : 431;
            conn->parser.req.keep_alive = 0;
            conn_send_error(epfd, conn, status);
            return;
        }
        ssize_t n = read(conn->fd,
                         conn->inbuf + conn->inbuf_len, space);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            cs_log(LOG_ERROR, "read fd=%d: %s",
                   conn->fd, strerror(errno));
            conn_close(epfd, conn);
            return;
        }
        if (n == 0) {
            eof = 1;
            break;
        }
        conn->inbuf_len += (size_t)n;
    }

    conn->last_active_ms = cs_now_ms();
    int rc = cs_parser_feed(&conn->parser,
                             conn->inbuf, conn->inbuf_len);
    if (rc == -431) {
        conn->parser.req.keep_alive = 0;
        conn_send_error(epfd, conn, 431);
        return;
    }
    if (rc < 0) {
        conn_send_error(epfd, conn, 400);
        return;
    }

    /* Enforce body limit as soon as Content-Length is known */
    if (conn->parser.req.content_length > g_cfg->max_body_bytes) {
        conn->parser.req.keep_alive = 0;
        conn_send_error(epfd, conn, 413);
        return;
    }

    if (rc == 1) {
        conn_dispatch(epfd, conn);
        return;
    }
    /* Partial request: if peer closed, nothing more is coming */
    if (eof)
        conn_close(epfd, conn);
    /* rc == 0 without eof: need more data, wait for next EPOLLIN */
}

/* ------------------------------------------------------------------ */
/* Timeout wheel                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    int      epfd;
    uint64_t now;
} timeout_ctx_t;

static void
timeout_one(conn_t *conn, void *arg)
{
    timeout_ctx_t *ctx = arg;
    uint64_t elapsed = ctx->now - conn->last_active_ms;

    /*
     * Use keepalive_timeout_ms for idle connections (no bytes yet for
     * the next request), request_timeout_ms for partial requests.
     */
    uint64_t limit = (conn->parser.pos == 0)
        ? (uint64_t)g_cfg->keepalive_timeout_ms
        : (uint64_t)g_cfg->request_timeout_ms;

    if (elapsed >= limit) {
        cs_log(LOG_DEBUG,
               "timeout fd=%d state=%d elapsed=%" PRIu64 "ms",
               conn->fd, conn->state, elapsed);
        conn_close(ctx->epfd, conn);
    }
}

static void
check_timeouts(int epfd)
{
    timeout_ctx_t ctx = { .epfd = epfd, .now = cs_now_ms() };
    cs_conn_pool_foreach(timeout_one, &ctx);
}

/* Close idle connections during graceful shutdown. */
static void
close_idle_conn(conn_t *conn, void *arg)
{
    int epfd = *(int *)arg;
    if (conn->state == CONN_READING_REQUEST)
        conn_close(epfd, conn);
}

/* ------------------------------------------------------------------ */
/* Main event loop                                                     */
/* ------------------------------------------------------------------ */

static void
server_loop(int epfd, int sfd)
{
    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            cs_fatal("epoll_wait: %s", strerror(errno));
        }
        if (n == 0) {
            check_timeouts(epfd);
            if (g_shutting_down) {
                uint64_t elapsed = cs_now_ms() - g_shutdown_ms;
                if (cs_conn_active_count() == 0
                    || elapsed >= SHUTDOWN_GRACE_MS)
                    return;
            }
            continue;
        }

        for (int i = 0; i < n; i++) {
            void *ptr = events[i].data.ptr;

            if (ptr == &g_signal_sentinel) {
                struct signalfd_siginfo si;
                ssize_t rd = read(sfd, &si, sizeof(si));
                if (rd == (ssize_t)sizeof(si))
                    cs_log(LOG_INFO,
                           "signal %u received, shutting down",
                           si.ssi_signo);

                g_shutting_down = 1;
                g_shutdown_ms   = cs_now_ms();

                /* Stop accepting new connections */
                epoll_ctl(epfd, EPOLL_CTL_DEL,
                          g_listen_fd, NULL);
                close(g_listen_fd);
                g_listen_fd = -1;

                /* Close idle keep-alive connections immediately */
                cs_conn_pool_foreach(close_idle_conn, &epfd);

                if (cs_conn_active_count() == 0)
                    return;
                continue;
            }
            if (ptr == &g_listen_sentinel) {
                if (!g_shutting_down)
                    handle_accept(epfd);
                continue;
            }
            handle_conn_event(epfd, (conn_t *)ptr,
                               events[i].events);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

void
cs_server_run(server_config_t *cfg)
{
    g_cfg = cfg;
    signal(SIGPIPE, SIG_IGN);

    cs_log_set_level(cfg->log_level);
    cs_conn_pool_init(cfg->max_connections);

    if (cfg->docroot[0] != '\0')
        cs_static_init(cfg->docroot);

    g_listen_fd = make_listen_socket(cfg);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0)
        cs_fatal("epoll_create1: %s", strerror(errno));

    struct epoll_event ev = {
        .events   = EPOLLIN | EPOLLET,
        .data.ptr = &g_listen_sentinel,
    };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, g_listen_fd, &ev) < 0)
        cs_fatal("epoll_ctl add listen_fd: %s", strerror(errno));

    int sfd = make_signalfd();
    struct epoll_event sev = {
        .events   = EPOLLIN,
        .data.ptr = &g_signal_sentinel,
    };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &sev) < 0)
        cs_fatal("epoll_ctl add signalfd: %s", strerror(errno));

    cs_log(LOG_INFO, "listening on %s:%u (docroot: %s)",
           cfg->host, cfg->port,
           cfg->docroot[0] ? cfg->docroot : "(none)");

    server_loop(epfd, sfd);

    cs_log(LOG_INFO, "shutdown complete");
    close(sfd);
    if (g_listen_fd >= 0)
        close(g_listen_fd);
    close(epfd);
}
