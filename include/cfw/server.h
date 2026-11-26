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

/* Request-body upload hooks (ADR 0033). When upload_begin is set,
 * request bodies are delivered incrementally to the hooks instead of
 * being buffered whole; cfw_request_t.body is then empty and
 * cfw_request_t.upload carries the handle returned by begin.
 *
 * upload_begin runs once per request whose framing declares a body,
 * after head validation; it must return CFW_OK and set *upload to a
 * non-NULL application token. Any failure aborts the connection.
 * upload_write receives each decoded body piece as a borrowed span
 * valid only for the call; returning an error aborts the connection.
 * upload_end runs exactly once per begun upload: completed=1 after the
 * full body arrives (before dispatch) or completed=0 on abort. It is
 * optional; upload_begin and upload_write must be set together. */
typedef cfw_status_t (*cfw_upload_begin_fn)(void *user,
                                            const cfw_http_request_head_t *head,
                                            void **upload);
typedef cfw_status_t (*cfw_upload_write_fn)(void *upload,
                                            const cfw_span_t *piece);
typedef void (*cfw_upload_end_fn)(void *upload, int completed);

/* Protocol upgrade support (ADR 0034). When the `upgrade` hook is set,
 * a handler may take over the connection by returning status 101
 * Switching Protocols for a request that carried an `Upgrade` header.
 * The server then stops HTTP framing and delivers readiness events to
 * the hook; the application drives the new protocol through the
 * cfw_upgraded_* functions. See docs/decisions/0034-protocol-upgrades.md.
 *
 * `events` is a bitmask of CFW_UPGRADE_EV_* flags. The hook is invoked
 * once with CFW_UPGRADE_EV_READY immediately after takeover (the 101
 * response head is queued but may not be flushed yet — upgraded sends
 * queue behind it, preserving wire order), then on each readiness
 * event. Return CFW_OK to keep the connection; any error closes it.
 * All callbacks run on the server's event-loop thread and must not
 * block. */
typedef struct cfw_server_conn cfw_upgraded_conn_t;

enum {
    /* Initial invocation right after takeover; no readiness implied. */
    CFW_UPGRADE_EV_READY = 0,
    /* Socket is readable (or peer sent EOF — check via recv). */
    CFW_UPGRADE_EV_READ = 1u << 0,
    /* Previously queued upgraded output fully drained; the app may
     * queue more via cfw_upgraded_send. */
    CFW_UPGRADE_EV_WRITE = 1u << 1,
    /* Peer performed an orderly shutdown (FIN received). */
    CFW_UPGRADE_EV_HUP = 1u << 2,
    /* Socket error condition reported by the pollset. */
    CFW_UPGRADE_EV_ERR = 1u << 3,
    /* Final invocation: the upgraded connection is being torn down
     * (peer gone, cfw_upgraded_close drained, error, idle deadline, or
     * server shutdown). Delivered exactly once, always after any
     * READY/data events. Use it to release per-connection state (e.g.
     * cfw_ws_destroy). cfw_upgraded_recv/send/close are invalid at
     * this point; the return value is ignored. */
    CFW_UPGRADE_EV_CLOSE = 1u << 4
};

typedef cfw_status_t (*cfw_upgrade_fn)(cfw_upgraded_conn_t *conn,
                                       uint32_t events, void *user);

/* Receives raw bytes on an upgraded connection. Bytes already buffered
 * past the HTTP request are returned first, then socket data.
 * Returns CFW_OK with *n == 0 on orderly peer shutdown — after that
 * the connection closes once queued output drains, so any final bytes
 * must be sent in the same hook invocation. Returns CFW_OK with *n > 0
 * on data, CFW_ERROR_WOULDBLOCK when no data is available, or
 * CFW_ERROR_IO on socket errors (the connection is then dead and the
 * handle must not be used again). Only valid inside the upgrade hook. */
CFW_API cfw_status_t cfw_upgraded_recv(cfw_upgraded_conn_t *conn,
                                       void *buffer, size_t capacity,
                                       size_t *received);

/* Queues raw bytes for an upgraded connection. All-or-nothing against
 * the connection's max_out_size bound: returns CFW_ERROR_WOULDBLOCK
 * when the whole span does not fit the pending queue — retry when
 * CFW_UPGRADE_EV_WRITE is delivered. Returns CFW_ERROR_IO once the
 * connection is closing or closed. Only valid inside the upgrade hook. */
CFW_API cfw_status_t cfw_upgraded_send(cfw_upgraded_conn_t *conn,
                                       const void *data, size_t size);

/* Begins a graceful close: queued output is flushed, then the socket
 * closes and no further upgrade events are delivered. Immediate when
 * nothing is queued. Only valid inside the upgrade hook. */
CFW_API void cfw_upgraded_close(cfw_upgraded_conn_t *conn);

/* Returns the send-queue space currently available on an upgraded
 * connection (max_out_size minus queued-but-unflushed bytes); 0 when
 * the connection is not upgraded or is closed. Protocol modules use
 * this to pre-flight multi-part sends transactionally. Only valid
 * inside the upgrade hook. */
CFW_API size_t cfw_upgraded_out_free(cfw_upgraded_conn_t *conn);

/* Per-connection application state for upgraded connections; the
 * hook's `user` argument is shared across all upgraded connections.
 * Pass NULL to cfw_upgraded_set_user to clear. */
CFW_API void *cfw_upgraded_user(cfw_upgraded_conn_t *conn);
CFW_API void cfw_upgraded_set_user(cfw_upgraded_conn_t *conn, void *user);

/* Per-request completion record (ADR 0055). Delivered exactly once
 * per fully parsed request head — after the final response is queued,
 * when an upgraded connection activates, when a response stream
 * finishes, or when the connection closes with a record still pending
 * (mid-stream abort, request timeout, teardown). `status` is the final
 * response status (cfw_http_status_t value), or 0 when the connection
 * ended before any response status was produced.
 *
 * `method`, `target`, and `remote_addr` are borrowed views valid only
 * for the duration of the callback; copy anything retained. The hook
 * runs on the server's event-loop thread, must not block, and must
 * not call back into the same server. */
typedef struct cfw_request_record {
    cfw_http_method_t method;
    cfw_strview_t target;
    cfw_http_version_t version;
    int status;
    /* Decoded request-body bytes received for this request. */
    uint64_t request_bytes;
    /* Bytes of the final response handed to the HTTP/1.1 writer:
     * serialized head plus body payload. Chunked-transfer framing
     * overhead and interim 100 Continue responses are excluded; for
     * streamed responses this counts payload bytes emitted up to the
     * record. */
    uint64_t response_bytes;
    /* Monotonic time from request-head receipt to record emission;
     * zero when the clock is unavailable. */
    cfw_duration_t duration;
    const cfw_strview_t *remote_addr;
    /* Nonzero when the transaction ended in a 101 protocol takeover. */
    int upgraded;
} cfw_request_record_t;

typedef void (*cfw_request_done_fn)(const cfw_request_record_t *record,
                                    void *user);

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
    /* After cfw_server_stop, cfw_server_run keeps serving existing
     * connections until they drain or this deadline passes; zero
     * returns immediately (connections then close on cfw_server_free
     * or continued cfw_server_step calls). */
    cfw_duration_t drain_timeout;
    /* Per-request head+body deadline; zero disables. */
    cfw_duration_t request_timeout;
    /* Per-request scratch sizes for header fields and route params. */
    size_t max_headers;
    size_t max_params;
    /* Optional logger for lifecycle events; may be NULL. */
    cfw_logger_t *logger;
    /* Optional request-body upload hooks; all share upload_user.
     * upload_begin and upload_write must both be non-NULL to enable
     * streaming uploads; upload_end may be NULL. */
    cfw_upload_begin_fn upload_begin;
    cfw_upload_write_fn upload_write;
    cfw_upload_end_fn upload_end;
    void *upload_user;
    /* Optional protocol-upgrade hook (ADR 0034). When set, handlers
     * may return 101 for requests carrying an Upgrade header to take
     * over the connection; upgrade and upgrade_user are then invoked
     * for the lifetime of the upgraded protocol. NULL disables
     * upgrades (101 is sent as an ordinary response). */
    cfw_upgrade_fn upgrade;
    void *upgrade_user;
    /* Optional per-request completion hook (ADR 0055); may be NULL.
     * Invoked once per parsed request head with the final status,
     * byte counts, duration, and peer address. See
     * cfw_request_record_t for field semantics and lifetimes. */
    cfw_request_done_fn request_done;
    void *request_done_user;
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
