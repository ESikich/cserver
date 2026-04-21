/*
 * config.c -- Configuration defaults and command-line/INI parsing.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cserve.h"

void
cs_config_defaults(server_config_t *cfg)
{
    *cfg = (server_config_t){
        .host                 = "0.0.0.0",
        .port                 = 8080,
        .docroot              = "./www",
        .max_connections      = 4096,
        .request_timeout_ms   = 10000,
        .keepalive_timeout_ms = 30000,
        .max_body_bytes       = 1048576,
        .log_level            = LOG_INFO,
        .route_count          = 0,
    };
}

/* ------------------------------------------------------------------ */
/* INI parser                                                          */
/* ------------------------------------------------------------------ */

/* Trim leading and trailing whitespace in-place; returns trimmed ptr. */
static char *
trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1)))
        *--end = '\0';
    return s;
}

static void
apply_ini_key(server_config_t *cfg, const char *section,
              const char *key, const char *val, int lineno)
{
    if (strcmp(section, "server") == 0) {
        if (strcmp(key, "host") == 0) {
            snprintf(cfg->host, sizeof(cfg->host), "%s", val);
        } else if (strcmp(key, "port") == 0) {
            int p = atoi(val);
            if (p <= 0 || p > 65535)
                cs_fatal("config line %d: invalid port '%s'",
                         lineno, val);
            cfg->port = (uint16_t)p;
        } else if (strcmp(key, "root") == 0) {
            snprintf(cfg->docroot, sizeof(cfg->docroot), "%s", val);
        } else if (strcmp(key, "log") == 0) {
            if (strcmp(val, "off")   == 0) cfg->log_level = LOG_OFF;
            else if (strcmp(val, "error") == 0)
                cfg->log_level = LOG_ERROR;
            else if (strcmp(val, "info")  == 0)
                cfg->log_level = LOG_INFO;
            else if (strcmp(val, "debug") == 0)
                cfg->log_level = LOG_DEBUG;
            else
                cs_fatal("config line %d: unknown log level '%s'",
                         lineno, val);
        } else if (strcmp(key, "max_conn") == 0) {
            int n = atoi(val);
            if (n <= 0)
                cs_fatal("config line %d: invalid max_conn '%s'",
                         lineno, val);
            cfg->max_connections = n;
        } else {
            cs_log(LOG_DEBUG,
                   "config line %d: unknown key '%s' in [%s]",
                   lineno, key, section);
        }

    } else if (strcmp(section, "limits") == 0) {
        if (strcmp(key, "request_timeout_ms") == 0) {
            int v = atoi(val);
            if (v <= 0)
                cs_fatal("config line %d: invalid value '%s'",
                         lineno, val);
            cfg->request_timeout_ms = v;
        } else if (strcmp(key, "keepalive_timeout_ms") == 0) {
            int v = atoi(val);
            if (v <= 0)
                cs_fatal("config line %d: invalid value '%s'",
                         lineno, val);
            cfg->keepalive_timeout_ms = v;
        } else if (strcmp(key, "max_body_bytes") == 0) {
            long long v = atoll(val);
            if (v <= 0)
                cs_fatal("config line %d: invalid value '%s'",
                         lineno, val);
            cfg->max_body_bytes = (size_t)v;
        } else {
            cs_log(LOG_DEBUG,
                   "config line %d: unknown key '%s' in [%s]",
                   lineno, key, section);
        }

    } else {
        cs_log(LOG_DEBUG,
               "config line %d: unknown section [%s]",
               lineno, section);
    }
}

static void
cs_config_parse_ini(server_config_t *cfg, const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
        cs_fatal("cannot open config file '%s'", path);

    char line[512];
    char section[64] = "";
    int lineno = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        lineno++;
        char *p = trim(line);

        if (*p == '\0' || *p == '#' || *p == ';')
            continue;

        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end == NULL)
                cs_fatal("config line %d: malformed section", lineno);
            *end = '\0';
            snprintf(section, sizeof(section), "%s", p + 1);
            continue;
        }

        char *eq = strchr(p, '=');
        if (eq == NULL)
            cs_fatal("config line %d: expected 'key = value'",
                     lineno);
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);

        /* Strip inline comment */
        char *comment = strchr(val, '#');
        if (comment != NULL) {
            *comment = '\0';
            val = trim(val);
        }

        if (*section == '\0')
            cs_fatal("config line %d: key outside any section",
                     lineno);

        apply_ini_key(cfg, section, key, val, lineno);
    }

    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Command-line parser                                                 */
/* ------------------------------------------------------------------ */

void
cs_config_parse_args(server_config_t *cfg, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            snprintf(cfg->host, sizeof(cfg->host),
                     "%s", argv[++i]);

        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            int p = atoi(argv[++i]);
            if (p <= 0 || p > 65535)
                cs_fatal("invalid port: %s", argv[i]);
            cfg->port = (uint16_t)p;

        } else if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            snprintf(cfg->docroot, sizeof(cfg->docroot),
                     "%s", argv[++i]);

        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            ++i;
            if (strcmp(argv[i], "off") == 0)
                cfg->log_level = LOG_OFF;
            else if (strcmp(argv[i], "error") == 0)
                cfg->log_level = LOG_ERROR;
            else if (strcmp(argv[i], "info") == 0)
                cfg->log_level = LOG_INFO;
            else if (strcmp(argv[i], "debug") == 0)
                cfg->log_level = LOG_DEBUG;
            else
                cs_fatal("unknown log level: %s", argv[i]);

        } else if (strcmp(argv[i], "--max-conn") == 0
                   && i + 1 < argc) {
            int n = atoi(argv[++i]);
            if (n <= 0)
                cs_fatal("invalid max-conn: %s", argv[i]);
            cfg->max_connections = n;

        } else if (strcmp(argv[i], "--config") == 0
                   && i + 1 < argc) {
            cs_config_parse_ini(cfg, argv[++i]);

        } else {
            cs_fatal("unknown argument: %s", argv[i]);
        }
    }
}
