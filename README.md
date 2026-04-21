# cserve

A minimal, event-driven HTTP/1.1 server written in C23 for Linux.

Single-process, epoll-based, no external dependencies beyond libc.
Serves static files and exposes a handler API for dynamic content.

```
$ ab -n 10000 -c 100 -k http://127.0.0.1:18080/
Requests per second: 42663 [#/sec]
Failed requests:     0
```

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
--host     <addr>   Interface to bind       (default: 0.0.0.0)
--port     <port>   Port to listen on       (default: 8080)
--root     <path>   Document root           (default: ./www)
--config   <path>   Path to INI file
--log      <level>  off|error|info|debug    (default: info)
--max-conn <n>      Max concurrent connections (default: 4096)
```

---

## Configuration file

Command-line arguments take priority over the INI file. The INI file
takes priority over compiled-in defaults.

```ini
[server]
host     = 0.0.0.0
port     = 8080
root     = ./www
log      = info
max_conn = 4096

[limits]
request_timeout_ms   = 10000
keepalive_timeout_ms = 30000
max_body_bytes       = 1048576
```

Pass the file path with `--config cserve.ini`.

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

Tests cover the HTTP parser, router, and utility functions. They run
without network access or file I/O.

---

## Fuzzing

The `fuzz/` directory contains an AFL++ harness for the HTTP parser.

```sh
AFL_SKIP_CPUFREQ=1 afl-fuzz -i fuzz/corpus -o fuzz/findings \
    -- ./build/cserve_fuzz
```

---

## Design

The server is documented in `cserve-design.txt`. Short version:

- **Concurrency:** single-threaded epoll event loop, edge-triggered
  (`EPOLLET`). All sockets `O_NONBLOCK` via `accept4`.
- **Memory:** fixed-size connection pool (default 4096). Per-request
  arena allocator reset between keep-alive cycles. No `malloc()` in
  the hot path.
- **Parser:** hand-rolled state machine. Zero-copy — all parsed values
  are slices into the input buffer. Hard limits on request line,
  headers, and body enforced at parse time.
- **File serving:** `sendfile()` for zero-copy file-to-socket transfer.
  `realpath()` + prefix check on every path to prevent traversal.

---

## Source layout

```
src/
  main.c       entry point, argv parsing
  server.c     epoll loop, accept, event dispatch
  conn.c       connection pool, arena allocator
  parser.c     HTTP/1.1 request parser
  router.c     route table, dispatch
  static.c     static file handler, MIME types
  response.c   response builder helpers
  config.c     INI parser, config defaults
  log.c        logger
  util.c       URL decode, path utilities

include/
  cserve.h     all public types and declarations

tests/         unit tests (no framework, plain C)
fuzz/          AFL++ harness
```

---

## Requirements

- Linux (epoll, sendfile, signalfd)
- GCC >= 13 or Clang >= 17
- CMake >= 3.20
- No runtime dependencies beyond libc