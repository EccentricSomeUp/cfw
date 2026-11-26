/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_SERVER_H
#define CFW_SERVER_H

#include <cfw/buffer.h>
#include <cfw/dispatch.h>
#include <cfw/fd.h>
#include <cfw/http1_conn.h>
#include <cfw/log.h>
#include <cfw/status.h>
#include <cfw/time.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Top-level HTTP server (ADR 0029). Owns a listening socket, the event
 * pollset, per-connection HTTP/1.1 readers, and drives the dispatch
 * pipeline. Single-threaded: one server per thread; concurrent servers
 * require external synchronization only for shared dispatch/router
 * state (dispatch is read-only during requests).
 *
 * POSIX transport only for now; on Windows all operations report
 * CFW_ERROR_UNAVAILABLE.
 *
 * Usage:
 *   cfw_server_t server;
 *   cfw_server_init(&server, &options);
 *   cfw_server_run(&server);          // blocks until cfw_server_stop
 *   // or embed:
 *   while (!done) cfw_server_step(&server, deadline);
 *   cfw_server_free(&server);
 */

typedef struct cfw_server_conn cfw_server_conn_t;
typedef struct cfw_pollset cfw_pollset_t; /* private, opaque here */

typedef struct cfw_server_options {
    /* Listen address: numeric IP text or NULL for any-address of the
     * selected family. No DNS resolution in v1. */
    const char *host;
    /* Nonzero binds/listens on IPv6 (host NULL → "::"); zero is IPv4. */
    int ipv6;
    uint16_t port;
    int backlog;
    /* Dispatch pipeline (router + middleware + handlers). Required;
     * must outlive the server. */
    cfw_dispatch_t *dispatch;
    /* HTTP/1.1 reader limits per connection. */
    cfw_http1_server_options_t http;
    /* Maximum queued response bytes per connection. */
    size_t max_out_size;
    /* Maximum simultaneous connections; accepts beyond this close
     * immediately. Must be > 0. */
    size_t max_connections;
    /* Idle keep-alive timeout between requests; zero disables. */
    cfw_duration_t keep_alive_timeout;
    /* Per-request head+body deadline; zero disables. */
    cfw_duration_t request_timeout;
    /* Per-request scratch sizes for header fields and route params. */
    size_t max_headers;
    size_t max_params;
    /* Optional logger for lifecycle events; may be NULL. */
    cfw_logger_t *logger;
} cfw_server_options_t;

typedef struct cfw_server {
    cfw_fd_t listener;
    cfw_fd_t wakeup[2];
    cfw_pollset_t *pollset;
    cfw_server_conn_t *conns;
    size_t conn_count;
    size_t conn_capacity_hint;
    cfw_server_options_t options;
    int stopping;
    int listening;
    int listener_registered;
    /* Diagnostics counters. */
    uint64_t accepted_total;
    uint64_t requests_total;
    uint64_t errors_total;
} cfw_server_t;

/* Creates the listener, pollset, and wake channel. On success the
 * server is not yet accepting (call cfw_server_run or step); the
 * listening socket is already bound so cfw_server_port reports the
 * actual port when options.port was 0. */
CFW_API cfw_status_t cfw_server_init(cfw_server_t *server,
                                     const cfw_server_options_t *options);
CFW_API void cfw_server_free(cfw_server_t *server);

/* Blocks in the event loop until cfw_server_stop is called or a fatal
 * error occurs. Returns CFW_OK on clean stop. */
CFW_API cfw_status_t cfw_server_run(cfw_server_t *server);

/* Performs one event-loop iteration: waits for readiness up to
 * wait_deadline (NULL waits until the nearest connection deadline or
 * indefinitely when none exists), services events, and sweeps expired
 * connections. Safe to call after cfw_server_stop to drain remaining
 * connections. */
CFW_API cfw_status_t cfw_server_step(cfw_server_t *server,
                                     const cfw_timepoint_t *wait_deadline);

/* Requests shutdown: the listener is unregistered, the loop is woken,
 * and run() returns. Idempotent. */
CFW_API void cfw_server_stop(cfw_server_t *server);

CFW_API int cfw_server_is_stopping(const cfw_server_t *server);
CFW_API size_t cfw_server_connection_count(const cfw_server_t *server);
CFW_API uint16_t cfw_server_port(const cfw_server_t *server);
CFW_API uint64_t cfw_server_accepted_total(const cfw_server_t *server);
CFW_API uint64_t cfw_server_requests_total(const cfw_server_t *server);
CFW_API uint64_t cfw_server_errors_total(const cfw_server_t *server);

#ifdef __cplusplus
}
#endif

#endif /* CFW_SERVER_H */
