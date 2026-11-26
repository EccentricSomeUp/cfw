/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Threaded cfw server: N worker threads, each running its own
 * cfw_server_t event loop, all sharing one listen address via
 * SO_REUSEPORT (ADR 0056). The kernel distributes accepted
 * connections across the instances; the dispatch pipeline is shared
 * read-only, so handlers and middleware must not mutate shared state
 * without external synchronization (cfw/sync.h provides the
 * primitives — this example uses them for the startup rendezvous).
 *
 * Usage: threaded_server [workers] [port]
 * Requires a platform with SO_REUSEPORT; elsewhere cfw_server_init
 * reports CFW_ERROR_UNAVAILABLE and the example exits.
 */

#include <cfw/cfw.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Startup rendezvous shared by all workers (never copied). */
typedef struct {
    cfw_mutex_t mutex;
    cfw_cond_t cond;
    int ready;
    int failed;
} cfw_worker_sync;

typedef struct {
    cfw_dispatch_t *dispatch;
    const char *host;
    uint16_t port;
    cfw_worker_sync *sync;
    cfw_server_t server;
    int server_up;
} cfw_worker_shared;

static cfw_status_t hello(cfw_request_t *req, cfw_response_t *res,
                          void *user)
{
    static const char body[] = "Hello from a cfw worker!";
    (void)req;
    (void)user;
    res->status = CFW_HTTP_STATUS_OK;
    res->body.data = (const uint8_t *)body;
    res->body.size = sizeof(body) - 1;
    return CFW_OK;
}

static void worker_report(cfw_worker_shared *w, int ok)
{
    cfw_mutex_lock(&w->sync->mutex);
    if (ok) {
        w->sync->ready++;
    } else {
        w->sync->failed = 1;
    }
    cfw_cond_broadcast(&w->sync->cond);
    cfw_mutex_unlock(&w->sync->mutex);
}

static int worker_main(void *arg)
{
    cfw_worker_shared *w = arg;
    cfw_server_options_t sopt;

    memset(&sopt, 0, sizeof(sopt));
    sopt.host = w->host;
    sopt.port = w->port;
    sopt.dispatch = w->dispatch;
    sopt.http.max_head_size = 8192;
    sopt.http.max_pending_size = 65536;
    sopt.max_out_size = 8192;
    sopt.max_connections = 128;
    sopt.max_headers = 32;
    sopt.max_params = 16;
    sopt.reuse_port = 1;
    if (cfw_server_init(&w->server, &sopt) != CFW_OK) {
        worker_report(w, 0);
        return 1;
    }
    w->server_up = 1;
    worker_report(w, 1);
    /* If another worker already failed startup, self-stop so the
     * main thread's join cannot hang on a late binder. */
    cfw_mutex_lock(&w->sync->mutex);
    {
        int abort = w->sync->failed;
        cfw_mutex_unlock(&w->sync->mutex);
        if (abort) {
            cfw_server_stop(&w->server);
        }
    }
    cfw_server_run(&w->server); /* blocks until cfw_server_stop */
    cfw_server_free(&w->server);
    return 0;
}

int main(int argc, char **argv)
{
    enum { MAX_WORKERS = 32 };
    int workers = argc > 1 ? atoi(argv[1]) : 4;
    uint16_t port = argc > 2 ? (uint16_t)atoi(argv[2]) : 8080;
    const char *host = "127.0.0.1";
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t dopt;
    cfw_strview_t pattern;
    cfw_worker_sync sync;
    cfw_worker_shared shared[MAX_WORKERS];
    cfw_thread_t threads[MAX_WORKERS];
    int i;
    int spawned = 0;
    int failed = 0;

    if (workers < 1 || workers > MAX_WORKERS) {
        fprintf(stderr, "workers must be 1..%d\n", MAX_WORKERS);
        return 1;
    }

    arena = cfw_arena_create(1u << 20);
    if (arena == NULL || cfw_router_init(&router, arena) != CFW_OK) {
        return 1;
    }
    memset(&dopt, 0, sizeof(dopt));
    dopt.router = &router;
    dopt.handler = hello;
    dopt.max_middleware = 4;
    if (cfw_dispatch_init(&dispatch, arena, &dopt) != CFW_OK) {
        cfw_arena_destroy(arena);
        return 1;
    }
    if (cfw_strview_from_cstr(&pattern, "/hello") != CFW_OK ||
        cfw_dispatch_add_route(&dispatch, CFW_HTTP_METHOD_GET, &pattern,
                               hello, NULL) != CFW_OK) {
        cfw_arena_destroy(arena);
        return 1;
    }

    memset(&sync, 0, sizeof(sync));
    if (cfw_mutex_init(&sync.mutex) != CFW_OK ||
        cfw_cond_init(&sync.cond) != CFW_OK) {
        cfw_arena_destroy(arena);
        return 1;
    }
    memset(shared, 0, sizeof(shared));

    for (i = 0; i < workers; i++) {
        shared[i].dispatch = &dispatch;
        shared[i].host = host;
        shared[i].port = port;
        shared[i].sync = &sync;
        if (cfw_thread_create(&threads[i], worker_main, &shared[i]) !=
            CFW_OK) {
            fprintf(stderr, "cfw_thread_create failed at worker %d\n", i);
            break;
        }
        spawned++;
    }

    /* Wait until every spawned worker has bound (or one failed). */
    cfw_mutex_lock(&sync.mutex);
    while (!sync.failed && sync.ready < spawned) {
        (void)cfw_cond_wait(&sync.cond, &sync.mutex, NULL);
    }
    failed = sync.failed;
    cfw_mutex_unlock(&sync.mutex);

    if (failed || spawned == 0) {
        fprintf(stderr,
                "worker startup failed (SO_REUSEPORT unsupported?)\n");
        for (i = 0; i < spawned; i++) {
            if (shared[i].server_up) {
                cfw_server_stop(&shared[i].server);
            }
        }
        for (i = 0; i < spawned; i++) {
            int code = 0;
            cfw_thread_join(&threads[i], &code);
        }
        cfw_cond_destroy(&sync.cond);
        cfw_mutex_destroy(&sync.mutex);
        cfw_arena_destroy(arena);
        return 1;
    }

    fprintf(stderr,
            "cfw threaded server: %d worker(s) on http://%s:%u/hello\n",
            spawned, host, (unsigned)port);
    fflush(stderr);

    /* Park on the first worker; it runs until cfw_server_stop. The
     * remaining workers keep serving regardless (same contract as
     * the minimal example — graceful stop is application policy). */
    {
        int code = 0;
        cfw_thread_join(&threads[0], &code);
    }
    return 0;
}
