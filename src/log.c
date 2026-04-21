/*
 * log.c -- Logger. Single-threaded; not thread-safe.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cserve.h"

static int  g_log_level = LOG_INFO;
static int  g_log_fd    = STDOUT_FILENO;
static char g_log_path[PATH_MAX];

static uint64_t
log_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000
         + (uint64_t)ts.tv_nsec / 1000000;
}

static const char *
level_label(int level)
{
    switch (level) {
    case LOG_ERROR: return "ERROR";
    case LOG_INFO:  return "INFO ";
    case LOG_DEBUG: return "DEBUG";
    default:        return "?????";
    }
}

void
cs_log_set_level(int level)
{
    g_log_level = level;
}

/*
 * Open log_path for appending. Empty string means stdout.
 * Calls cs_fatal() on failure -- intended for startup only.
 */
void
cs_log_open(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        g_log_fd      = STDOUT_FILENO;
        g_log_path[0] = '\0';
        return;
    }

    snprintf(g_log_path, sizeof(g_log_path), "%s", path);

    int fd = open(g_log_path,
                  O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (fd < 0)
        cs_fatal("cannot open log file '%s': %s",
                 g_log_path, strerror(errno));
    g_log_fd = fd;
}

/*
 * Close the current log file and reopen it at the same path.
 * Called from the SIGHUP handler. Safe to call when using stdout.
 */
void
cs_log_reopen(void)
{
    if (g_log_path[0] == '\0')
        return;

    int fd = open(g_log_path,
                  O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (fd < 0) {
        cs_log(LOG_ERROR, "log reopen '%s': %s",
               g_log_path, strerror(errno));
        return;
    }

    if (g_log_fd != STDOUT_FILENO)
        close(g_log_fd);
    g_log_fd = fd;
    cs_log(LOG_INFO, "log reopened");
}

void
cs_log(int level, const char *fmt, ...)
{
    if (level > g_log_level)
        return;

    char buf[2048];
    int n = snprintf(buf, sizeof(buf), "[%s] %" PRIu64 " ",
                     level_label(level), log_now_ms());

    if (n > 0 && (size_t)n < sizeof(buf) - 1) {
        va_list ap;
        va_start(ap, fmt);
        int m = vsnprintf(buf + n, sizeof(buf) - (size_t)n - 1,
                          fmt, ap);
        va_end(ap);
        if (m > 0)
            n += m;
    }

    if (n >= (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 2;
    buf[n]     = '\n';
    buf[n + 1] = '\0';

    /* Single write() so lines cannot interleave */
    if (write(g_log_fd, buf, (size_t)n + 1) < 0) {}
}

[[noreturn]] void
cs_fatal(const char *fmt, ...)
{
    char buf[2048];
    int n = snprintf(buf, sizeof(buf), "[FATAL] ");

    if (n > 0 && (size_t)n < sizeof(buf) - 1) {
        va_list ap;
        va_start(ap, fmt);
        int m = vsnprintf(buf + n, sizeof(buf) - (size_t)n - 1,
                          fmt, ap);
        va_end(ap);
        if (m > 0)
            n += m;
    }

    if (n >= (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 2;
    buf[n]     = '\n';
    buf[n + 1] = '\0';

    if (write(STDERR_FILENO, buf, (size_t)n + 1) < 0) {}
    exit(1);
}
