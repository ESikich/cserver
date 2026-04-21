# CLAUDE.md — cserve

## Role

You are implementing cserve according to cserve-design.txt. Read the
design doc before writing any code. When the spec is silent on a
detail, choose the most conservative option and leave a comment noting
the assumption.


## Language & Standard

C23 (gcc/clang -std=c23). Use C23 features where they genuinely
clarify intent -- typeof, constexpr for compile-time constants,
[[nodiscard]], [[maybe_unused]], binary literals if useful. Do not
reach for new features to be clever. If a C89 construct is clearer,
use it.

_GNU_SOURCE is defined project-wide. Do not define it per-file.


## Naming

  Functions:   cs_verb_noun()        cs_conn_alloc(), cs_parser_feed()
  Types:       noun_t                conn_t, slice_t, route_t
  Enums:       NOUN_ADJECTIVE        CONN_CLOSING, PS_DONE
  Constants:   ALL_CAPS              INBUF_SIZE, MAX_HEADERS
  Parameters:  short, clear          conn, req, resp, buf, len
  Locals:      brief                 i, n, p, fd -- acceptable at scope

No Hungarian notation. No redundant type tags (conn_t *conn, not
conn_t *p_conn). No abbreviations that require a glossary.


## Style

Indentation:    4 spaces. No tabs.
Braces:         K&R -- opening brace on the same line.
Line length:    79 columns. Hard limit.
Blank lines:    one between functions, one between logical blocks
                within a function. Not more.

  /* Good */
  static int
  conn_drain(conn_t *conn)
  {
      if (conn->outbuf_head == conn->outbuf_tail)
          return 0;
      ...
  }

Function return type on its own line for definitions (not declarations).
Pointer attached to the name, not the type: char *p, not char* p.


## Comments

File header: a single block comment with filename, one-line purpose,
and nothing else. No author, no date, no license boilerplate.

  /*
   * parser.c -- HTTP/1.1 request parser state machine.
   */

Function comments: only for non-obvious contracts or invariants.
Do not narrate what the code already says clearly.

  /* Bad */
  /* Increment i by 1 */
  i++;

  /* Good */
  /* outbuf is a ring; head chases tail. Empty when head == tail. */

Use /* */ throughout. No // line comments except for temporary TODOs,
which must be prefixed TODO: and resolved before a phase is complete.


## Error Handling

Every syscall return value is checked. No silent discards.
Use early returns to keep the happy path unindented:

  if (fd < 0) {
      log_error("accept4 failed: %s", strerror(errno));
      return -1;
  }

Do not chain assignments and checks: n = read(...); if (n < 0) not
if ((n = read(...)) < 0).

Fatal errors call cs_fatal(fmt, ...) which logs to stderr and exits(1).
Connection errors return -1 to the event loop, which closes the conn.
Never call exit() outside of cs_fatal() and main().


## Memory

malloc() and free() are banned outside of the startup/shutdown path.
All per-request allocation goes through cs_arena_alloc().
If an arena allocation fails, return HANDLER_ERR immediately.

Do not cache pointers into inbuf across event loop iterations.
Slices (slice_t) are only valid for the lifetime of the current request.


## Headers

One include block per file, sorted: system headers first, then
standard C headers, then cserve.h. A blank line between each group.

  #include <sys/epoll.h>
  #include <sys/sendfile.h>
  #include <unistd.h>

  #include <stdint.h>
  #include <string.h>

  #include "cserve.h"

Do not include headers speculatively. If a symbol is not used, the
header is not included.


## Structs & Initialization

Always initialize structs explicitly. Zero-init with = {0} only when
every field being zero is a meaningful, correct initial state and that
invariant is worth documenting. Otherwise initialize each field.

Prefer designated initializers (C99 and valid in C23):

  http_response_t resp = {
      .status  = 200,
      .file_fd = -1,
  };


## Logging

The logger (log.c) owns the log fd. All other modules call cs_log()
or the level macros (log_error, log_info, log_debug). No module other
than log.c and main.c may write directly to stderr or stdout.

Log file reopen (SIGHUP) is handled entirely within log.c via
cs_log_reopen(). The event loop calls this when it reads SIGHUP from
the signalfd -- no other module is involved.

Do not buffer log output. Each line must reach the fd before the
function returns.


## Signals

All signal handling goes through the signalfd added to the epoll set.
Do not use signal handlers, sigaction callbacks, or SA_RESTART anywhere
in the codebase. The only exception is signal(SIGPIPE, SIG_IGN) in
main(), which requires no handler.

Graceful shutdown is triggered by the event loop reading SIGTERM or
SIGINT from the signalfd. The shutdown sequence is defined in section 4
of the design doc. Do not call exit() from the signal path -- set the
global shutdown flag and let the event loop drain normally.


## Phasing

Implement one phase at a time. Do not write phase 6 code while in
phase 5. Stubs are acceptable -- mark them:

  /* TODO: implement conditional GET (phase 5) */

A phase is complete when:
  - Code compiles clean at -Wall -Wextra -Wpedantic -Werror
  - All tests in tests/ pass under both Debug and Release builds
  - No TODO comments remain for that phase
  - The design doc section for that phase has been reviewed and
    updated if anything changed during implementation


## Tests

Every function in parser.c, router.c, util.c, and static.c must have
at least one test in the corresponding test file. Tests are plain C --
no framework. Each test is a function returning int (0 pass, 1 fail).
A test runner in each file calls them in sequence and reports results
to stdout.

Tests must not depend on network sockets or wall-clock time. File I/O
is permitted in test_cache.c only, using temp files created and
removed within the test.


## What Not To Do

- Do not introduce any header beyond cserve.h as a public interface.
- Do not use assert() in production code paths. Check and handle.
- Do not swallow errno -- log it or propagate it.
- Do not add convenience wrappers that the design doc does not specify.
- Do not reformat code outside the files you are editing in a session.
- Do not silently change struct fields or function signatures. If the
  design doc must change, say so explicitly before writing the code.
- Do not write to stdout or stderr directly -- use the logger.
- Do not handle signals outside of the signalfd path.