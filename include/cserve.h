/*
 * cserve.h -- Public types and function declarations.
 */

#ifndef CSERVE_H
#define CSERVE_H

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <sys/types.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define MAX_HEADERS     64
#define INBUF_SIZE      65536
#define OUTBUF_SIZE     131072
#define ARENA_SIZE      8192
#define MAX_ROUTES      64

#define HANDLER_OK      0
#define HANDLER_ERR     (-1)

#define LOG_OFF         0
#define LOG_ERROR       1
#define LOG_INFO        2
#define LOG_DEBUG       3

/* ------------------------------------------------------------------ */
/* Core types                                                          */
/* ------------------------------------------------------------------ */

/* A slice into a buffer -- no ownership, no copy */
typedef struct {
    size_t  off;
    size_t  len;
} slice_t;

/* A parsed HTTP header (name and value are slices into inbuf) */
typedef struct {
    slice_t name;
    slice_t value;
} header_t;

/* A fully parsed HTTP request */
typedef struct {
    slice_t   method;
    slice_t   path;           /* URL-decoded, not yet path-resolved */
    slice_t   version;        /* "HTTP/1.1" etc */
    header_t  headers[MAX_HEADERS];
    int       header_count;
    slice_t   body;
    int       keep_alive;     /* 1 if Connection: keep-alive */
    uint64_t  content_length;
    /* Conditional GET fields -- populated by parser if present */
    slice_t   if_none_match;      /* value of If-None-Match header */
    slice_t   if_modified_since;  /* value of If-Modified-Since header */
} http_request_t;

/* HTTP response built by a handler */
typedef struct {
    int      status;          /* e.g. 200, 404 */
    char     headers[4096];   /* pre-formatted header block */
    size_t   headers_len;
    /* For in-memory body (dynamic handlers): */
    uint8_t *body;            /* points into conn arena */
    size_t   body_len;
    /* For file body (static handler): */
    int      file_fd;         /* -1 if none */
    off_t    file_offset;
    off_t    file_size;
} http_response_t;

/* Parser state machine */
typedef enum {
    PS_REQ_METHOD, PS_REQ_PATH, PS_REQ_VERSION,
    PS_HEADER_NAME, PS_HEADER_VALUE, PS_HEADER_END,
    PS_BODY, PS_DONE, PS_ERROR
} parse_state_t;

typedef struct {
    parse_state_t  state;
    size_t         pos;   /* current read position in inbuf */
    size_t         mark;  /* start of current token */
    http_request_t req;
} parser_t;

/* Connection state */
typedef enum {
    CONN_READING_REQUEST,
    CONN_DISPATCHING,
    CONN_WRITING_RESPONSE,
    CONN_CLOSING
} conn_state_t;

typedef struct conn_s conn_t;

/* outbuf is a ring; head chases tail. Empty when head == tail. */
struct conn_s {
    int             fd;
    conn_state_t    state;
    uint64_t        last_active_ms;  /* for timeout wheel */

    uint8_t         inbuf[INBUF_SIZE];
    size_t          inbuf_len;

    uint8_t         outbuf[OUTBUF_SIZE];
    size_t          outbuf_head;
    size_t          outbuf_tail;

    uint8_t         arena[ARENA_SIZE];
    size_t          arena_pos;

    parser_t        parser;
    http_response_t response;

    conn_t         *next_free;    /* free-list linkage */
};

/* Handler callback signature */
typedef int (*handler_fn)(conn_t *conn, const http_request_t *req,
                          http_response_t *resp);

/* Route table entry */
typedef struct {
    char        method[16];   /* "GET", "POST", "" = any */
    char        path[256];    /* exact match; "" = catch-all */
    handler_fn  handler;
} route_t;

/* Server configuration -- populated at startup, then read-only */
typedef struct {
    char     host[64];               /* default "0.0.0.0" */
    uint16_t port;                   /* default 8080 */
    char     docroot[PATH_MAX];      /* document root for static files */
    int      max_connections;        /* default 4096 */
    int      request_timeout_ms;     /* default 10000 */
    int      keepalive_timeout_ms;   /* default 30000 */
    size_t   max_body_bytes;         /* default 1 MB */
    int      log_level;
    char     log_file[PATH_MAX];     /* "" = stdout */
    route_t  routes[MAX_ROUTES];
    int      route_count;
} server_config_t;

/* ------------------------------------------------------------------ */
/* Function declarations                                               */
/* ------------------------------------------------------------------ */

/* log.c */
void              cs_log_set_level(int level);
void              cs_log_open(const char *path);
void              cs_log_reopen(void);
void              cs_log(int level, const char *fmt, ...);
[[noreturn]] void cs_fatal(const char *fmt, ...);

/* config.c */
void  cs_config_defaults(server_config_t *cfg);
void  cs_config_parse_args(server_config_t *cfg,
                            int argc, char **argv);

/* conn.c */
void              cs_conn_pool_init(int max_connections);
conn_t           *cs_conn_alloc(void);
void              cs_conn_free(conn_t *conn);
void              cs_conn_pool_foreach(void (*fn)(conn_t *, void *),
                                       void *arg);
int               cs_conn_active_count(void);
[[nodiscard]] void *cs_arena_alloc(conn_t *conn, size_t size);

/* server.c */
void  cs_server_run(server_config_t *cfg);

/* parser.c */
void  cs_parser_init(parser_t *p);
int   cs_parser_feed(parser_t *p,
                     const uint8_t *buf, size_t len);

/* router.c */
void  cs_add_route(server_config_t *cfg, const char *method,
                   const char *path, handler_fn handler);
int   cs_router_dispatch(server_config_t *cfg, conn_t *conn,
                         const http_request_t *req,
                         http_response_t *resp);

/* response.c */
const char *cs_status_str(int status);
int         cs_add_header(http_response_t *resp,
                          const char *name, const char *value);
const char *cs_get_header(const http_request_t *req,
                           conn_t *conn, const char *name);

/* static.c */
void cs_static_init(const char *docroot);
int  cs_static_handler(conn_t *conn, const http_request_t *req,
                        http_response_t *resp);

/* util.c */
uint64_t cs_now_ms(void);
int      cs_url_decode(const char *in, size_t in_len,
                       char *out, size_t out_size);
int      cs_path_safe(const char *docroot, const char *path);
int      cs_http_date(time_t t, char *buf, size_t size);

#endif /* CSERVE_H */
