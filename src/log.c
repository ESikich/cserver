/*
 * log.c -- Logger. Single-threaded; not thread-safe.
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "cserve.h"

static int g_log_level = LOG_INFO;

static uint64_t
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
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

void
cs_log(int level, const char *fmt, ...)
{
    if (level > g_log_level)
        return;

    va_list ap;
    va_start(ap, fmt);
    printf("[%s] %" PRIu64 " ", level_label(level), now_ms());
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

[[noreturn]] void
cs_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[FATAL] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}
