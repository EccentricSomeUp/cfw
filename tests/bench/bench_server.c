/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Loopback HTTP/1.1 keep-alive benchmark for cfw_server_t.
 *
 * Runs one server in a worker thread and drives sequential
 * request/response exchanges over a single keep-alive connection,
 * reporting requests/second and response bytes/second. This is a
 * manual measurement tool, not a pass/fail test; numbers are only
 * meaningful when comparing builds on the same machine.
 *
 * Usage: cfw_bench [requests] [warmup]
 *   requests  measured exchanges (default 20000)
 *   warmup    unmeasured exchanges first (default 1000)
 *
 * POSIX only (uses the private socket layer like the unit tests).
 */

#include "private/net.h"

#include <cfw/cfw.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define CFW_BENCH_BODY_SIZE 1024
#define CFW_BENCH_REQUEST                                                 \
    "GET /bench HTTP/1.1\r\n"                                             \
    "Host: bench\r\n"                                                     \
    "\r\n"

typedef struct {
    cfw_server_t *server;
} cfw_bench_worker;

static char cfw_bench_body[CFW_BENCH_BODY_SIZE];

static cfw_status_t cfw_bench_handler(cfw_request_t *req,
                                      cfw_response_t *res, void *user)
{
    (void)req;
    (void)user;
    res->status = CFW_HTTP_STATUS_OK;
    res->body.data = (const uint8_t *)cfw_bench_body;
    res->body.size = sizeof(cfw_bench_body);
    return CFW_OK;
}

static int cfw_bench_worker_main(void *arg)
{
    cfw_bench_worker *w = arg;
    cfw_server_run(w->server);
    return 0;
}

/* Sends one request and reads exactly one full response (head +
 * declared body). Returns response bytes on success, -1 on error. */
static long cfw_bench_exchange(cfw_fd_t *client, size_t *response_bytes)
{
    char buffer[8192];
    size_t used = 0;
    size_t head_end = 0;
    size_t content_length = 0;
    int have_cl = 0;
    struct timeval tv;

    tv.tv_sec = 10;
    tv.tv_usec = 0;
    (void)setsockopt((int)cfw_fd_get(client), SOL_SOCKET, SO_RCVTIMEO,
                     &tv, sizeof(tv));

    for (;;) {
        ssize_t n;
        size_t i;

        if (used + 1 >= sizeof(buffer)) {
            return -1;
        }
        n = recv((int)cfw_fd_get(client), buffer + used,
                 sizeof(buffer) - used - 1, 0);
        if (n <= 0) {
            return -1;
        }
        used += (size_t)n;
        buffer[used] = '\0';

        if (head_end == 0) {
            for (i = 0; i + 3 < used; i++) {
                if (buffer[i] == '\r' && buffer[i + 1] == '\n' &&
                    buffer[i + 2] == '\r' && buffer[i + 3] == '\n') {
                    char *cl = strstr(buffer, "Content-Length:");
                    head_end = i + 4;
                    if (cl != NULL) {
                        content_length =
                            (size_t)strtoul(cl + 15, NULL, 10);
                        have_cl = 1;
                    }
                    break;
                }
            }
        }
        if (head_end != 0 && have_cl &&
            used - head_end >= content_length) {
            *response_bytes = used;
            return (long)used;
        }
    }
}

int main(int argc, char **argv)
{
    long requests = argc > 1 ? atol(argv[1]) : 20000;
    long warmup = argc > 2 ? atol(argv[2]) : 1000;
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t dopt;
    cfw_server_options_t sopt;
    cfw_strview_t pattern;
    cfw_server_t server;
    cfw_bench_worker worker;
    cfw_thread_t thread;
    cfw_fd_t client;
    cfw_sockaddr_t addr;
    long i;
    long total_bytes = 0;
    cfw_timepoint_t start;
    cfw_timepoint_t end;
    cfw_duration_t elapsed;
    int64_t nanos = 0;

    if (requests <= 0) {
        fprintf(stderr, "requests must be positive\n");
        return 1;
    }
    memset(cfw_bench_body, 'x', sizeof(cfw_bench_body));

    arena = cfw_arena_create(1u << 20);
    if (arena == NULL || cfw_router_init(&router, arena) != CFW_OK) {
        return 1;
    }
    memset(&dopt, 0, sizeof(dopt));
    dopt.router = &router;
    dopt.handler = cfw_bench_handler;
    dopt.max_middleware = 4;
    if (cfw_dispatch_init(&dispatch, arena, &dopt) != CFW_OK) {
        cfw_arena_destroy(arena);
        return 1;
    }
    if (cfw_strview_from_cstr(&pattern, "/bench") != CFW_OK ||
        cfw_dispatch_add_route(&dispatch, CFW_HTTP_METHOD_GET, &pattern,
                               cfw_bench_handler, NULL) != CFW_OK) {
        cfw_arena_destroy(arena);
        return 1;
    }

    memset(&sopt, 0, sizeof(sopt));
    sopt.host = "127.0.0.1";
    sopt.port = 0;
    sopt.dispatch = &dispatch;
    sopt.http.max_head_size = 8192;
    sopt.http.max_body_size = 65536;
    sopt.http.max_pending_size = 65536;
    sopt.max_out_size = 1u << 20;
    sopt.max_connections = 8;
    sopt.max_headers = 32;
    sopt.max_params = 16;
    if (cfw_server_init(&server, &sopt) != CFW_OK) {
        cfw_arena_destroy(arena);
        return 1;
    }

    worker.server = &server;
    if (cfw_thread_create(&thread, cfw_bench_worker_main, &worker) !=
        CFW_OK) {
        cfw_server_free(&server);
        cfw_arena_destroy(arena);
        return 1;
    }

    /* The listener is already bound after cfw_server_init, so the
     * client can connect immediately; the kernel queues the connect
     * until the worker's event loop accepts it. */
    if (cfw_addr_from_str(&addr, "127.0.0.1",
                          cfw_server_port(&server)) != CFW_OK ||
        cfw_net_socket(AF_INET, SOCK_STREAM, 0, &client) != CFW_OK ||
        cfw_net_set_nonblocking(&client, 0) != CFW_OK ||
        connect((int)cfw_fd_get(&client),
                (const struct sockaddr *)&addr.storage,
                addr.len) != 0) {
        fprintf(stderr, "client connect failed\n");
        cfw_server_stop(&server);
        cfw_thread_join(&thread, NULL);
        cfw_server_free(&server);
        cfw_arena_destroy(arena);
        return 1;
    }

    /* Warmup: fill parser/server caches and steady-state queues. */
    for (i = 0; i < warmup; i++) {
        size_t rb = 0;
        if (send((int)cfw_fd_get(&client), CFW_BENCH_REQUEST,
                 sizeof(CFW_BENCH_REQUEST) - 1, 0) !=
                (ssize_t)(sizeof(CFW_BENCH_REQUEST) - 1) ||
            cfw_bench_exchange(&client, &rb) < 0) {
            fprintf(stderr, "warmup exchange %ld failed\n", i);
            goto fail;
        }
    }

    if (cfw_time_now(&start) != CFW_OK) {
        goto fail;
    }
    for (i = 0; i < requests; i++) {
        size_t rb = 0;
        if (send((int)cfw_fd_get(&client), CFW_BENCH_REQUEST,
                 sizeof(CFW_BENCH_REQUEST) - 1, 0) !=
                (ssize_t)(sizeof(CFW_BENCH_REQUEST) - 1) ||
            cfw_bench_exchange(&client, &rb) < 0) {
            fprintf(stderr, "exchange %ld failed\n", i);
            goto fail;
        }
        total_bytes += (long)rb;
    }
    if (cfw_time_now(&end) != CFW_OK ||
        cfw_timepoint_diff(&end, &start, &elapsed) != CFW_OK) {
        goto fail;
    }
    nanos = elapsed.nanoseconds;
    if (nanos <= 0) {
        nanos = 1;
    }

    printf("requests:      %ld\n", requests);
    printf("seconds:       %.3f\n", (double)nanos / 1e9);
    printf("req/s:         %.0f\n",
           (double)requests * 1e9 / (double)nanos);
    printf("response MiB/s: %.2f\n",
           (double)total_bytes / (1024.0 * 1024.0) * 1e9 / (double)nanos);

    cfw_fd_close(&client);
    cfw_server_stop(&server);
    cfw_thread_join(&thread, NULL);
    cfw_server_free(&server);
    cfw_arena_destroy(arena);
    return 0;

fail:
    cfw_fd_close(&client);
    cfw_server_stop(&server);
    cfw_thread_join(&thread, NULL);
    cfw_server_free(&server);
    cfw_arena_destroy(arena);
    return 1;
}
