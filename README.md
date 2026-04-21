# cserve

A minimal, event-driven HTTP/1.1 server written in C23 for Linux.

Single-process, epoll-based, no external dependencies beyond libc.
Serves static files and exposes a handler API for dynamic content.

```
$ ab -n 10000 -c 100 -k http://127.0.0.1:18080/
Requests per second: 48142 [#/sec]
Failed requests:     0
```

---

## Features

- HTTP/1.1 keep-alive
- ETag and Last-Modified headers — conditional GET (304 Not Modified)
- HEAD method
- Static file serving via `sendfile()`
- Dynamic handler API
- Structured access log with file output and SIGHUP rotation
- Graceful shutdown on SIGTERM/SIGINT
- systemd unit file included
- Parser and URL decoder fuzz-tested with AFL++

---

## Building

Requires GCC >= 13 or Clang >= 17, CMake >= 3.20.

```sh
cmake -DCMAKE_BUILD_TYPE=Release -B build_release
cmake --build build_release
```

For a debug build with AddressSanitizer and UBSan:

```sh
cmake -DCMAKE_BUILD_TYPE=Debug -B build
cmake --build build
```

The output is a single executable — `build_release/cserve`. No install
step, no shared libraries.

---

## Running

```sh
./build_release/cserve --port 8080 --root ./www
```

All options:

```
--host       <addr>   Interface to bind          (default: 0.0.0.0)
--port       <port>   Port to listen on          (default: 8080)
--root       <path>   Document root              (default: ./www)
--config     <path>   Path to INI file
--log        <level>  off|error|info|debug       (default: info)
--log-file   <path>   Log file path              (default: stdout)
--max-conn   <n>      Max concurrent connections (default: 4096)
```

---

## Configuration file

Command-line arguments take priority over the INI file. The INI file
takes priority over compiled-in defaults.

```ini
[server]
host     = 0.0.0.0
port     = 8080
root     = /var/www
log      = info
log_file = /var/log/cserve/access.log
max_conn = 4096

[limits]
request_timeout_ms   = 10000
keepalive_timeout_ms = 30000
max_body_bytes       = 1048576
```

Pass the file path with `--config cserve.ini`.

---

## Running as a service

A systemd unit file is included at `cserve.service`. Copy it into place
and enable it:

```sh
sudo cp cserve.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now cserve
```

Log rotation with logrotate:

```
/var/log/cserve/access.log {
    daily
    rotate 14
    compress
    missingok
    notifempty
    postrotate
        systemctl kill -s HUP cserve
    endscript
}
```

---

## Caching

The static file handler sets `ETag` and `Last-Modified` on every 200
response. Subsequent requests with `If-None-Match` or
`If-Modified-Since` receive a 304 with no body if the file has not
changed. Browsers and well-behaved HTTP clients handle this
automatically.

```sh
# First request
curl -I http://127.0.0.1:8080/
# HTTP/1.1 200 OK
# ETag: "69e7a5cf0002c717"
# Last-Modified: Tue, 21 Apr 2026 16:29:03 GMT

# Subsequent request
curl -I -H 'If-None-Match: "69e7a5cf0002c717"' http://127.0.0.1:8080/
# HTTP/1.1 304 Not Modified
```

---

## Dynamic handlers

Register handlers before starting the server:

```c
#include "cserve.h"

static int
hello_handler(conn_t *conn, const http_request_t *req,
              http_response_t *resp)
{
    const char *body = "hello\n";
    resp->status   = 200;
    resp->body     = (uint8_t *)body;
    resp->body_len = strlen(body);
    cs_add_header(resp, "Content-Type", "text/plain");
    return HANDLER_OK;
}

int main(void)
{
    server_config_t cfg = cs_config_defaults();
    cs_add_route(&cfg, "GET", "/hello", hello_handler);
    cs_server_run(&cfg);
}
```

A handler receives a fully parsed request and an empty response struct.
It must not block, call `malloc()`, or retain pointers into `req` after
returning. Use `cs_arena_alloc(conn, size)` for any per-request
allocation.

Routes are matched in registration order. The first match wins. Any
path not matched by a dynamic route falls through to the static file
handler if `--root` is configured.

---

## Tests

```sh
cmake --build build -- tests
ctest --test-dir build --output-on-failure
```

Tests cover the HTTP parser, router, utility functions, and conditional
GET logic. They run without network access.

---

## Fuzzing

The `fuzz/` directory contains AFL++ harnesses for the HTTP parser and
URL decoder.

```sh
# Build harnesses (afl-cc required)
AFL_SKIP_CPUFREQ=1 afl-cc -std=c2x -D_GNU_SOURCE -I include \
    fuzz/fuzz_parser.c src/parser.c src/util.c \
    -o fuzz/cserve_fuzz

# Run
AFL_SKIP_CPUFREQ=1 AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 \
    afl-fuzz -i fuzz/corpus -o fuzz/findings \
    -- fuzz/cserve_fuzz
```

Check `fuzz/findings/crashes/` when done — empty is the result you want.

---

## Design

The server is documented in `cserve-design.txt`. Short version:

- **Concurrency:** single-threaded epoll event loop, edge-triggered
  (`EPOLLET`). timerfd for the timeout wheel. signalfd for
  SIGTERM/SIGHUP/SIGINT. All sockets `O_NONBLOCK` via `accept4`.
- **Memory:** fixed-size connection pool (default 4096). Per-request
  arena allocator reset between keep-alive cycles. No `malloc()` in
  the hot path.
- **Parser:** hand-rolled state machine. Zero-copy — all parsed values
  are slices into the input buffer. Hard limits on request line,
  headers, and body enforced at parse time.
- **File serving:** `sendfile()` for zero-copy file-to-socket transfer.
  `realpath()` + prefix check on every path to prevent traversal.
- **Caching:** ETag computed from mtime and file size. Conditional GET
  checked before opening the file.

---

## Source layout

```
src/
  main.c       entry point, argv parsing
  server.c     epoll loop, timerfd, signalfd, accept, shutdown
  conn.c       connection pool, arena allocator
  parser.c     HTTP/1.1 request parser
  router.c     route table, dispatch
  static.c     static file handler, MIME types, ETag, conditional GET
  response.c   response builder, HEAD suppression
  log.c        logger, log file, SIGHUP reopen
  config.c     INI parser, config defaults
  util.c       URL decode, path utilities, HTTP-date formatting

include/
  cserve.h     all public types and declarations

tests/         unit tests (no framework, plain C)
fuzz/          AFL++ harnesses
cserve.service systemd unit file
```

---

## Requirements

- Linux (epoll, sendfile, signalfd, timerfd)
- GCC >= 13 or Clang >= 17
- CMake >= 3.20
- No runtime dependencies beyond libc