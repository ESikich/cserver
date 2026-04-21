/*
 * conn.c -- Connection pool, lifecycle, and arena allocator.
 */

#include <stdlib.h>
#include <string.h>

#include "cserve.h"

static conn_t *g_pool         = NULL;
static conn_t *g_free_list    = NULL;
static int     g_pool_size    = 0;
static int     g_active_count = 0;

void
cs_conn_pool_init(int max_connections)
{
    g_pool = malloc((size_t)max_connections * sizeof(conn_t));
    if (g_pool == NULL)
        cs_fatal("failed to allocate connection pool (%d slots)",
                 max_connections);

    g_pool_size = max_connections;
    g_free_list = NULL;
    for (int i = max_connections - 1; i >= 0; i--) {
        g_pool[i].fd        = -1;
        g_pool[i].next_free = g_free_list;
        g_free_list         = &g_pool[i];
    }

    cs_log(LOG_DEBUG, "conn pool: %d slots, %zu bytes total",
           max_connections,
           (size_t)max_connections * sizeof(conn_t));
}

conn_t *
cs_conn_alloc(void)
{
    if (g_free_list == NULL)
        return NULL;

    conn_t *conn    = g_free_list;
    g_free_list     = conn->next_free;
    g_active_count++;

    memset(conn, 0, sizeof(*conn));
    conn->fd               = -1;
    conn->state            = CONN_READING_REQUEST;
    conn->response.file_fd = -1;

    return conn;
}

void
cs_conn_free(conn_t *conn)
{
    conn->fd        = -1;
    conn->next_free = g_free_list;
    g_free_list     = conn;
    g_active_count--;
}

int
cs_conn_active_count(void)
{
    return g_active_count;
}

void
cs_conn_pool_foreach(void (*fn)(conn_t *, void *), void *arg)
{
    for (int i = 0; i < g_pool_size; i++) {
        if (g_pool[i].fd >= 0)
            fn(&g_pool[i], arg);
    }
}

[[nodiscard]] void *
cs_arena_alloc(conn_t *conn, size_t size)
{
    size_t aligned = (size + 7) & ~(size_t)7;
    if (conn->arena_pos + aligned > ARENA_SIZE)
        return NULL;
    void *ptr = conn->arena + conn->arena_pos;
    conn->arena_pos += aligned;
    return ptr;
}
