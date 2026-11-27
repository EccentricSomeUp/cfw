/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Loopback HTTP/1.1 keep-alive benchmark for cfw_server_t.
 *
 * Runs one or more servers in worker threads and drives concurrent
 * request/response exchanges over keep-alive connections, reporting
 * aggregate requests/second and response bytes/second.
 *
 * With workers > 1 the servers share one listen address via
 * SO_REUSEPORT (ADR 0056) and each client thread is pinned to its own
 * connection — the kernel distributes accepts across the group.
 *
 * This is a manual measurement tool, not a pass/fail test; numbers are
 * only meaningful when comparing builds on the same machine.
 *
 * Usage: cfw_bench [requests] [warmup] [workers] [tls]
 *   requests  measured exchanges per client (default 20000)
 *   warmup    unmeasured exchanges per client first (default 1000)
 *   workers   server threads / client threads (default 1; >1 requires
 *             SO_REUSEPORT)
 *   tls       "tls" runs the same measurement over HTTPS; requires a
 *             CFW_ENABLE_TLS build and CFW_BENCH_TLS_CERT /
 *             CFW_BENCH_TLS_KEY (default server.crt/server.key).
 *             Handshakes happen once per client before warmup, so the
 *             numbers reflect steady-state TLS throughput.
 *
 * POSIX only (uses the private socket layer like the unit tests).
 */

#include <cfw/net.h>

#include <cfw/cfw.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define CFW_BENCH_BODY_SIZE 1024
#define CFW_BENCH_MAX_WORKERS 32
#define CFW_BENCH_REQUEST                                                 \
    "GET /bench HTTP/1.1\r\n"                                             \
    "Host: bench\r\n"                                                     \
    "\r\n"

typedef struct {
    cfw_mutex_t mutex;
    cfw_cond_t cond;
    int ready;
    int go;
    int failed;
} cfw_bench_gate;

typedef struct {
    cfw_server_t *server;
} cfw_bench_server_worker;

typedef struct {
    uint16_t port;
    long requests;
    long warmup;
    cfw_bench_gate *gate;
    int index;
    long done_reqs;
    long done_bytes;
    int64_t elapsed_ns;
    int failed;
} cfw_bench_client;

static char cfw_bench_body[CFW_BENCH_BODY_SIZE];
#if CFW_HAVE_TLS
static int cfw_bench_use_tls = 0;
static const char *cfw_bench_tls_cert = "server.crt";
static const char *cfw_bench_tls_key = "server.key";
#endif

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

static int cfw_bench_server_main(void *arg)
{
    cfw_bench_server_worker *w = arg;
    cfw_server_run(w->server);
    return 0;
}

#if CFW_HAVE_TLS
/* Sends the bench request through the TLS conn (the socket is
 * blocking, so writes complete without polling). */
static int cfw_bench_tls_send(cfw_tls_conn_t *conn)
{
    const char *p = CFW_BENCH_REQUEST;
    size_t left = sizeof(CFW_BENCH_REQUEST) - 1;

    while (left > 0) {
        size_t done = 0;
        cfw_status_t st = cfw_tls_write(conn, p, left, &done);
        if (st == CFW_OK) {
            if (done == 0) {
                return -1;
            }
            p += done;
            left -= done;
        } else if (st != CFW_ERROR_WOULDBLOCK) {
            return -1;
        }
    }
    return 0;
}
#endif

/* Sends one request and reads exactly one full response (head +
 * declared body). `tls` is NULL for cleartext. Returns response
 * bytes on success, -1 on error. */
static long cfw_bench_exchange(cfw_fd_t *client, cfw_tls_conn_t *tls,
                               size_t *response_bytes)
{
    char buffer[8192];
    size_t used = 0;
    size_t head_end = 0;
    size_t content_length = 0;
    int have_cl = 0;
    struct timeval tv;

#if !CFW_HAVE_TLS
    (void)tls;
#endif
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
#if CFW_HAVE_TLS
        if (tls != NULL) {
            size_t done = 0;
            cfw_status_t st = cfw_tls_read(tls, buffer + used,
                                           sizeof(buffer) - used - 1,
                                           &done);
            if (st == CFW_OK) {
                if (done == 0) {
                    return -1;
                }
                n = (ssize_t)done;
            } else if (st == CFW_ERROR_WOULDBLOCK) {
                continue;
            } else {
                return -1;
            }
        } else
#endif
        {
            n = recv((int)cfw_fd_get(client), buffer + used,
                     sizeof(buffer) - used - 1, 0);
            if (n <= 0) {
                return -1;
            }
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

static int cfw_bench_client_connect(cfw_bench_client *c, cfw_fd_t *out)
{
    cfw_addr_t addr;
    if (cfw_addr_from_str(&addr, "127.0.0.1", c->port) != CFW_OK ||
        cfw_net_socket(AF_INET, SOCK_STREAM, 0, out) != CFW_OK ||
        cfw_net_set_nonblocking(out, 0) != CFW_OK ||
        connect((int)cfw_fd_get(out),
                (const struct sockaddr *)&addr.storage,
                addr.length) != 0) {
        return -1;
    }
    return 0;
}

static int cfw_bench_client_main(void *arg)
{
    cfw_bench_client *c = arg;
    cfw_fd_t client;
    long i;
    cfw_timepoint_t start;
    cfw_timepoint_t end;
    cfw_duration_t elapsed;
    int abort = 0;
#if CFW_HAVE_TLS
    cfw_tls_ctx_t *tls_ctx = NULL;
    cfw_tls_conn_t *tls = NULL;
#endif

    cfw_fd_init(&client);
    if (cfw_bench_client_connect(c, &client) != 0) {
        c->failed = 1;
        goto out;
    }
#if CFW_HAVE_TLS
    if (cfw_bench_use_tls) {
        cfw_tls_ctx_options_t topt;
        memset(&topt, 0, sizeof(topt));
        topt.role = CFW_TLS_CLIENT;
        if (cfw_tls_ctx_create(&topt, &tls_ctx) != CFW_OK ||
            cfw_tls_conn_create(tls_ctx, cfw_fd_get(&client),
                                &tls) != CFW_OK) {
            c->failed = 1;
            goto out_close;
        }
        for (i = 0; i < 100000; i++) {
            cfw_status_t st = cfw_tls_handshake(tls);
            if (st == CFW_OK) {
                break;
            }
            if (st != CFW_ERROR_WOULDBLOCK) {
                c->failed = 1;
                goto out_close;
            }
        }
        if (i >= 100000 || !cfw_tls_handshake_done(tls)) {
            c->failed = 1;
            goto out_close;
        }
    }
#endif
    for (i = 0; i < c->warmup; i++) {
        size_t rb = 0;
#if CFW_HAVE_TLS
        if (cfw_bench_use_tls && tls != NULL) {
            if (cfw_bench_tls_send(tls) != 0 ||
                cfw_bench_exchange(&client, tls, &rb) < 0) {
                c->failed = 1;
                goto out_close;
            }
        } else
#endif
        if (send((int)cfw_fd_get(&client), CFW_BENCH_REQUEST,
                 sizeof(CFW_BENCH_REQUEST) - 1, 0) !=
                (ssize_t)(sizeof(CFW_BENCH_REQUEST) - 1) ||
            cfw_bench_exchange(&client, NULL, &rb) < 0) {
            c->failed = 1;
            goto out_close;
        }
    }

    /* Startup gate: all clients begin measuring together. */
    cfw_mutex_lock(&c->gate->mutex);
    c->gate->ready++;
    cfw_cond_broadcast(&c->gate->cond);
    while (!c->gate->go && !c->gate->failed) {
        (void)cfw_cond_wait(&c->gate->cond, &c->gate->mutex, NULL);
    }
    abort = c->gate->failed;
    cfw_mutex_unlock(&c->gate->mutex);
    if (abort) {
        c->failed = 1;
        goto out_close;
    }

    if (cfw_time_now(&start) != CFW_OK) {
        c->failed = 1;
        goto out_close;
    }
    for (i = 0; i < c->requests; i++) {
        size_t rb = 0;
#if CFW_HAVE_TLS
        if (cfw_bench_use_tls && tls != NULL) {
            if (cfw_bench_tls_send(tls) != 0 ||
                cfw_bench_exchange(&client, tls, &rb) < 0) {
                c->failed = 1;
                break;
            }
        } else
#endif
        if (send((int)cfw_fd_get(&client), CFW_BENCH_REQUEST,
                 sizeof(CFW_BENCH_REQUEST) - 1, 0) !=
                (ssize_t)(sizeof(CFW_BENCH_REQUEST) - 1) ||
            cfw_bench_exchange(&client, NULL, &rb) < 0) {
            c->failed = 1;
            break;
        }
        c->done_reqs++;
        c->done_bytes += (long)rb;
    }
    if (cfw_time_now(&end) == CFW_OK &&
        cfw_timepoint_diff(&end, &start, &elapsed) == CFW_OK) {
        c->elapsed_ns = elapsed.nanoseconds;
    }

out_close:
#if CFW_HAVE_TLS
    if (tls != NULL) {
        cfw_tls_conn_destroy(tls);
    }
    if (tls_ctx != NULL) {
        cfw_tls_ctx_destroy(tls_ctx);
    }
#endif
    if (cfw_fd_valid(&client)) {
        cfw_fd_close(&client);
    }
out:
    /* Wake the gate if we failed before reaching it. */
    cfw_mutex_lock(&c->gate->mutex);
    if (c->failed) {
        c->gate->failed = 1;
    }
    cfw_cond_broadcast(&c->gate->cond);
    cfw_mutex_unlock(&c->gate->mutex);
    return c->failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    long requests = argc > 1 ? atol(argv[1]) : 20000;
    long warmup = argc > 2 ? atol(argv[2]) : 1000;
    int workers = argc > 3 ? atoi(argv[3]) : 1;
#if CFW_HAVE_TLS
    cfw_tls_ctx_t *tls_ctx = NULL;
#endif
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t dopt;
    cfw_server_options_t sopt;
    cfw_strview_t pattern;
    cfw_server_t servers[CFW_BENCH_MAX_WORKERS];
    cfw_bench_server_worker server_workers[CFW_BENCH_MAX_WORKERS];
    cfw_bench_client clients[CFW_BENCH_MAX_WORKERS];
    cfw_thread_t server_threads[CFW_BENCH_MAX_WORKERS];
    cfw_thread_t client_threads[CFW_BENCH_MAX_WORKERS];
    cfw_bench_gate gate;
    uint16_t port = 0;
    long total_reqs = 0;
    long total_bytes = 0;
    int64_t wall_ns = 0;
    int i;
    int spawned_servers = 0;
    int spawned_clients = 0;
    int servers_inited = 0;

    if (requests <= 0 || workers < 1 || workers > CFW_BENCH_MAX_WORKERS) {
        fprintf(stderr, "usage: cfw_bench [requests] [warmup] [workers] "
                        "[tls]\n"
                        "  requests > 0, workers in 1..%d\n",
                CFW_BENCH_MAX_WORKERS);
        return 1;
    }
#if CFW_HAVE_TLS
    if (argc > 4 && strcmp(argv[4], "tls") == 0) {
        cfw_tls_ctx_options_t topt;
        const char *cert = getenv("CFW_BENCH_TLS_CERT");
        const char *key = getenv("CFW_BENCH_TLS_KEY");
        cfw_bench_use_tls = 1;
        cfw_bench_tls_cert = cert != NULL ? cert : "server.crt";
        cfw_bench_tls_key = key != NULL ? key : "server.key";
        memset(&topt, 0, sizeof(topt));
        topt.role = CFW_TLS_SERVER;
        topt.cert_file = cfw_bench_tls_cert;
        topt.key_file = cfw_bench_tls_key;
        if (cfw_tls_ctx_create(&topt, &tls_ctx) != CFW_OK) {
            fprintf(stderr, "cfw_bench: cannot load TLS cert/key "
                            "(%s, %s); set CFW_BENCH_TLS_CERT/KEY\n",
                    cfw_bench_tls_cert, cfw_bench_tls_key);
            return 1;
        }
    } else if (argc > 4) {
        fprintf(stderr, "cfw_bench: unknown mode '%s'\n", argv[4]);
        return 1;
    }
#else
    if (argc > 4) {
        fprintf(stderr, "cfw_bench: TLS mode requires a "
                        "CFW_ENABLE_TLS build\n");
        return 1;
    }
#endif
    memset(cfw_bench_body, 'x', sizeof(cfw_bench_body));
    memset(&gate, 0, sizeof(gate));
    if (cfw_mutex_init(&gate.mutex) != CFW_OK ||
        cfw_cond_init(&gate.cond) != CFW_OK) {
        return 1;
    }

    arena = cfw_arena_create(1u << 20);
    if (arena == NULL || cfw_router_init(&router, arena) != CFW_OK) {
        goto fail_gate;
    }
    memset(&dopt, 0, sizeof(dopt));
    dopt.router = &router;
    dopt.handler = cfw_bench_handler;
    dopt.max_middleware = 4;
    if (cfw_dispatch_init(&dispatch, arena, &dopt) != CFW_OK) {
        goto fail_arena;
    }
    if (cfw_strview_from_cstr(&pattern, "/bench") != CFW_OK ||
        cfw_dispatch_add_route(&dispatch, CFW_HTTP_METHOD_GET, &pattern,
                               cfw_bench_handler, NULL) != CFW_OK) {
        goto fail_arena;
    }

    /* Servers: worker 0 binds an ephemeral port first so the rest can
     * share it; reuse_port is only needed (and only meaningful) when
     * workers > 1. */
    for (i = 0; i < workers; i++) {
        memset(&sopt, 0, sizeof(sopt));
        sopt.host = "127.0.0.1";
        sopt.port = port;
        sopt.dispatch = &dispatch;
        sopt.http.max_head_size = 8192;
        sopt.http.max_body_size = 65536;
        sopt.http.max_pending_size = 65536;
        sopt.max_out_size = 1u << 20;
        sopt.max_connections = 64;
        sopt.max_headers = 32;
        sopt.max_params = 16;
        sopt.reuse_port = workers > 1 ? 1 : 0;
#if CFW_HAVE_TLS
        sopt.tls_ctx = tls_ctx;
#endif
        if (cfw_server_init(&servers[i], &sopt) != CFW_OK) {
            fprintf(stderr, "server %d init failed\n", i);
            goto fail_servers;
        }
        if (i == 0) {
            port = cfw_server_port(&servers[0]);
            if (port == 0) {
                goto fail_servers;
            }
        }
        servers_inited++;
        server_workers[i].server = &servers[i];
        if (cfw_thread_create(&server_threads[i],
                              cfw_bench_server_main,
                              &server_workers[i]) != CFW_OK) {
            fprintf(stderr, "server thread %d create failed\n", i);
            goto fail_servers;
        }
        spawned_servers++;
    }

    memset(clients, 0, sizeof(clients));
    for (i = 0; i < workers; i++) {
        clients[i].port = port;
        clients[i].requests = requests;
        clients[i].warmup = warmup;
        clients[i].gate = &gate;
        clients[i].index = i;
        if (cfw_thread_create(&client_threads[i], cfw_bench_client_main,
                              &clients[i]) != CFW_OK) {
            fprintf(stderr, "client thread %d create failed\n", i);
            goto fail_clients;
        }
        spawned_clients++;
    }

    /* Release the measurement gate once every client is ready. */
    cfw_mutex_lock(&gate.mutex);
    while (!gate.failed && gate.ready < spawned_clients) {
        (void)cfw_cond_wait(&gate.cond, &gate.mutex, NULL);
    }
    gate.go = 1;
    gate.failed = gate.failed || spawned_clients == 0;
    cfw_cond_broadcast(&gate.cond);
    cfw_mutex_unlock(&gate.mutex);

    for (i = 0; i < spawned_clients; i++) {
        int code = 0;
        cfw_thread_join(&client_threads[i], &code);
        if (code != 0) {
            gate.failed = 1;
        }
    }

    if (gate.failed) {
        fprintf(stderr, "benchmark failed\n");
        goto fail_servers;
    }

    for (i = 0; i < spawned_clients; i++) {
        total_reqs += clients[i].done_reqs;
        total_bytes += clients[i].done_bytes;
        if (clients[i].elapsed_ns > wall_ns) {
            wall_ns = clients[i].elapsed_ns;
        }
    }
    if (wall_ns <= 0) {
        wall_ns = 1;
    }

    printf("workers:       %d\n", workers);
#if CFW_HAVE_TLS
    printf("tls:           %s\n", cfw_bench_use_tls ? "on" : "off");
#endif
    printf("requests:      %ld (total %ld)\n", requests, total_reqs);
    printf("seconds:       %.3f\n", (double)wall_ns / 1e9);
    printf("req/s:         %.0f\n",
           (double)total_reqs * 1e9 / (double)wall_ns);
    printf("response MiB/s: %.2f\n",
           (double)total_bytes / (1024.0 * 1024.0) * 1e9 /
               (double)wall_ns);

    for (i = 0; i < spawned_servers; i++) {
        cfw_server_stop(&servers[i]);
    }
    for (i = 0; i < spawned_servers; i++) {
        int code = 0;
        cfw_thread_join(&server_threads[i], &code);
    }
    for (i = 0; i < servers_inited; i++) {
        cfw_server_free(&servers[i]);
    }
    cfw_mutex_destroy(&gate.mutex);
    cfw_cond_destroy(&gate.cond);
    cfw_arena_destroy(arena);
#if CFW_HAVE_TLS
    if (tls_ctx != NULL) {
        cfw_tls_ctx_destroy(tls_ctx);
    }
#endif
    return 0;

fail_clients:
    gate.failed = 1;
    cfw_mutex_lock(&gate.mutex);
    gate.go = 1;
    cfw_cond_broadcast(&gate.cond);
    cfw_mutex_unlock(&gate.mutex);
    for (i = 0; i < spawned_clients; i++) {
        int code = 0;
        cfw_thread_join(&client_threads[i], &code);
    }
fail_servers:
    for (i = 0; i < spawned_servers; i++) {
        cfw_server_stop(&servers[i]);
    }
    for (i = 0; i < spawned_servers; i++) {
        int code = 0;
        cfw_thread_join(&server_threads[i], &code);
    }
    for (i = 0; i < servers_inited; i++) {
        cfw_server_free(&servers[i]);
    }
fail_arena:
    cfw_arena_destroy(arena);
fail_gate:
    cfw_cond_destroy(&gate.cond);
    cfw_mutex_destroy(&gate.mutex);
#if CFW_HAVE_TLS
    if (tls_ctx != NULL) {
        cfw_tls_ctx_destroy(tls_ctx);
    }
#endif
    return 1;
}
