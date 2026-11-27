/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "private/platform_detect.h"

#include <cfw/server.h>
#include <cfw/strview.h>

#if CFW_PLATFORM_WINDOWS

cfw_status_t cfw_server_init(cfw_server_t *server,
                             const cfw_server_options_t *options)
{
    (void)server;
    (void)options;
    return CFW_ERROR_UNAVAILABLE;
}

void cfw_server_free(cfw_server_t *server) { (void)server; }

cfw_status_t cfw_server_run(cfw_server_t *server)
{
    (void)server;
    return CFW_ERROR_UNAVAILABLE;
}

cfw_status_t cfw_server_step(cfw_server_t *server,
                             const cfw_timepoint_t *wait_deadline)
{
    (void)server;
    (void)wait_deadline;
    return CFW_ERROR_UNAVAILABLE;
}

void cfw_server_stop(cfw_server_t *server) { (void)server; }
int cfw_server_is_stopping(const cfw_server_t *server)
{
    (void)server;
    return 0;
}
size_t cfw_server_connection_count(const cfw_server_t *server)
{
    (void)server;
    return 0;
}
uint16_t cfw_server_port(const cfw_server_t *server)
{
    (void)server;
    return 0;
}
uint64_t cfw_server_accepted_total(const cfw_server_t *server)
{
    (void)server;
    return 0;
}
uint64_t cfw_server_requests_total(const cfw_server_t *server)
{
    (void)server;
    return 0;
}
uint64_t cfw_server_errors_total(const cfw_server_t *server)
{
    (void)server;
    return 0;
}

cfw_status_t cfw_upgraded_recv(cfw_upgraded_conn_t *conn, void *buffer,
                               size_t capacity, size_t *received)
{
    (void)conn;
    (void)buffer;
    (void)capacity;
    (void)received;
    return CFW_ERROR_UNAVAILABLE;
}

cfw_status_t cfw_upgraded_send(cfw_upgraded_conn_t *conn, const void *data,
                               size_t size)
{
    (void)conn;
    (void)data;
    (void)size;
    return CFW_ERROR_UNAVAILABLE;
}

void cfw_upgraded_close(cfw_upgraded_conn_t *conn) { (void)conn; }

size_t cfw_upgraded_out_free(cfw_upgraded_conn_t *conn)
{
    (void)conn;
    return 0;
}

void *cfw_upgraded_user(cfw_upgraded_conn_t *conn)
{
    (void)conn;
    return NULL;
}

void cfw_upgraded_set_user(cfw_upgraded_conn_t *conn, void *user)
{
    (void)conn;
    (void)user;
}

#else /* POSIX */

#include <cfw/net.h>
#include <cfw/pollset.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CFW_SERVER_MAX_EVENTS 64u
#define CFW_SERVER_WAKE_TOKEN ((void *)(intptr_t)1)

struct cfw_server_conn {
    cfw_http1_conn_t http;
    cfw_http_header_t *headers;
    cfw_route_param_t *params;
    cfw_buffer_t body;
    cfw_buffer_t head_scratch;
    cfw_http_request_head_t head;
    cfw_response_stream_fn stream_fn;
    cfw_response_stream_end_fn stream_end;
    void *stream_user;
    cfw_span_t stream_chunk;
    size_t stream_pos;
    uint64_t stream_size;
    uint64_t stream_sent;
    int stream_eof;
    int stream_chunked;
    int stream_hold_close;
    int have_head;
    int rejecting;
    int dead;
    /* Protocol upgrade state (ADR 0034): set once a 101 response has
     * been queued for a request that carried an Upgrade header and the
     * server upgrade hook is configured. upgrade_pending holds input
     * bytes the HTTP reader had already buffered past the request;
     * upgrade_pending_pos is the drain cursor. upgrade_closing suppresses
     * further app callbacks while queued output finishes. */
    int upgraded;
    int upgrade_hup_seen;
    int upgrade_closing;
    int upgrade_close_sent;
    cfw_buffer_t upgrade_pending;
    size_t upgrade_pending_pos;
    void *upgrade_user;
    /* Peer address formatted "ip:port" at accept time; exposed to
     * handlers via cfw_request_t.remote_addr. */
    char remote_addr_buf[72];
    cfw_strview_t remote_addr;
    /* Active request-body upload token (ADR 0033); NULL when the body
     * is buffered or no upload is in progress. Non-NULL means
     * upload_end has not yet run for this request. */
    void *upload;
    cfw_timepoint_t deadline;
    int deadline_set;
    /* TLS state (ADR 0058): non-NULL when the server tls_ctx option
     * is set. The TLS session borrows conn->http's fd; the http
     * connection remains the fd owner. transport is the embedded
     * callback bundle handed to cfw_http1_conn at init. */
    cfw_tls_conn_t *tls;
    cfw_http1_transport_t transport;
    /* Mixed-mode listener (ADR 0061): nonzero while the connection's
     * protocol is undetected — the first POLL_IN peeks one byte and
     * either attaches TLS (0x16 ClientHello) or falls through to
     * cleartext HTTP. Only meaningful with tls_detect + tls_ctx. */
    int tls_detect;
    /* Request audit state (ADR 0055): armed at each parsed head and
     * emitted exactly once — when the final response is queued, an
     * upgrade activates, a stream finishes, or the conn closes with a
     * record pending. audit_status holds the last response status
     * determined (0 until a response head is queued). */
    int audit_pending;
    int audit_stream;
    int audit_status;
    uint64_t audit_request_bytes;
    uint64_t audit_response_bytes;
    cfw_timepoint_t audit_start;
    /* Last interest mask registered with the pollset; lets
     * cfw_server_update_conn_interest skip redundant epoll_ctl calls. */
    uint32_t poll_events;
    intptr_t handle;
    cfw_server_conn_t *next;
};

static void cfw_server_stream_pump(cfw_server_t *server,
                                   cfw_server_conn_t *conn);
static int cfw_server_head_has_upgrade(const cfw_http_request_head_t *head);
static void cfw_server_activate_upgrade(cfw_server_t *server,
                                        cfw_server_conn_t *conn);

static void cfw_server_conn_deadline(cfw_server_conn_t *conn,
                                     cfw_duration_t timeout)
{
    if (timeout.nanoseconds == 0) {
        conn->deadline_set = 0;
        return;
    }
    conn->deadline_set =
        cfw_deadline_from_now(&conn->deadline, timeout) == CFW_OK;
}

/* Emits the pending per-request audit record (ADR 0055) exactly
 * once. conn->head still describes the audited request: emission
 * always precedes parsing of any subsequent pipelined head. */
static void cfw_server_emit_request_done(cfw_server_t *server,
                                         cfw_server_conn_t *conn,
                                         int upgraded)
{
    cfw_request_record_t record;
    cfw_duration_t duration;
    cfw_timepoint_t now;

    if (!conn->audit_pending) {
        return;
    }
    conn->audit_pending = 0;
    conn->audit_stream = 0;
    if (server->options.request_done == NULL) {
        return;
    }
    memset(&record, 0, sizeof(record));
    record.method = conn->head.request_line.method;
    record.target = conn->head.request_line.target;
    record.version = conn->head.request_line.version;
    record.status = conn->audit_status;
    record.request_bytes = conn->audit_request_bytes;
    record.response_bytes = conn->audit_response_bytes;
    record.duration = (cfw_duration_t){0};
    if (cfw_time_now(&now) == CFW_OK) {
        (void)cfw_timepoint_diff(&now, &conn->audit_start, &duration);
        record.duration = duration;
    }
    record.remote_addr = &conn->remote_addr;
    record.upgraded = upgraded;
    server->options.request_done(&record, server->options.request_done_user);
}

static void cfw_server_close_conn(cfw_server_t *server,
                                  cfw_server_conn_t *conn)
{
    /* An upload still open here means the request aborted (timeout,
     * protocol error handled elsewhere already cleared it, or server
     * teardown): report it incomplete exactly once. */
    if (conn->upload != NULL && server->options.upload_end != NULL) {
        server->options.upload_end(conn->upload, 0);
    }
    conn->upload = NULL;
    /* An active stream here means the connection died mid-response:
     * run the app's stream_end hook exactly once so per-stream
     * resources (open files, buffers) are released. */
    if (conn->stream_fn != NULL && conn->stream_end != NULL) {
        conn->stream_end(conn->stream_user);
        conn->stream_end = NULL;
    }
    /* Deliver the final upgrade event so apps can release
     * per-connection protocol state. Runs before any conn teardown;
     * upgraded I/O is already invalid here (conn is dead or the
     * server is shutting down). */
    if (conn->upgraded && !conn->upgrade_close_sent &&
        server->options.upgrade != NULL) {
        conn->upgrade_close_sent = 1;
        (void)server->options.upgrade(conn, CFW_UPGRADE_EV_CLOSE,
                                      server->options.upgrade_user);
    }
    /* A record still pending means the request ended without a
     * finalized response (abort, timeout, teardown): emit it with
     * whatever status/bytes were determined. */
    cfw_server_emit_request_done(server, conn, conn->upgraded);
    cfw_pollset_remove(server->pollset, conn->handle);
    /* Best-effort close_notify before teardown; the fd is still open
     * (owned by conn->http, freed below). Failure is ignored — the
     * socket is closing regardless. */
    if (conn->tls != NULL) {
        (void)cfw_tls_shutdown(conn->tls);
        cfw_tls_conn_destroy(conn->tls);
        conn->tls = NULL;
    }
    cfw_http1_conn_free(&conn->http);
    cfw_buffer_free(&conn->body);
    cfw_buffer_free(&conn->head_scratch);
    cfw_buffer_free(&conn->upgrade_pending);
    free(conn->headers);
    free(conn->params);
    free(conn);
    server->conn_count--;
}

static void cfw_server_sweep(cfw_server_t *server)
{
    cfw_server_conn_t **link = &server->conns;
    cfw_timepoint_t now;
    int have_now = 0;

    while (*link != NULL) {
        cfw_server_conn_t *conn = *link;
        int expired = 0;

        if (conn->dead) {
            *link = conn->next;
            cfw_server_close_conn(server, conn);
            continue;
        }
        if (conn->deadline_set) {
            if (!have_now) {
                have_now = cfw_time_now(&now) == CFW_OK;
            }
            /* expired when now > deadline (compare result > 0). */
            if (have_now &&
                cfw_timepoint_compare(&now, &conn->deadline,
                                      &expired) == CFW_OK &&
                expired > 0) {
                *link = conn->next;
                cfw_server_close_conn(server, conn);
                continue;
            }
        }
        link = &conn->next;
    }
}

static void cfw_server_update_listener(cfw_server_t *server)
{
    int want = server->listening && !server->stopping &&
               server->conn_count < server->options.max_connections;
    intptr_t handle = cfw_fd_get(&server->listener);

    if (!cfw_fd_valid(&server->listener) || server->pollset == NULL) {
        return;
    }
    if (want && !server->listener_registered) {
        cfw_pollset_add(server->pollset, handle, CFW_POLL_IN, NULL);
        server->listener_registered = 1;
    } else if (!want && server->listener_registered) {
        cfw_pollset_remove(server->pollset, handle);
        server->listener_registered = 0;
    }
    /* A registered listener always wants CFW_POLL_IN, so no modify is
     * needed while `want` stays true. */
}

/* TLS transport callbacks (ADR 0058): cfw_tls_read/cfw_tls_write
 * already match the cfw_http1_transport_t signature modulo the
 * leading conn pointer, so these are thin adapters. */
static cfw_status_t cfw_server_tls_read(void *user, void *buf,
                                        size_t size, size_t *done)
{
    return cfw_tls_read((cfw_tls_conn_t *)user, buf, size, done);
}

static cfw_status_t cfw_server_tls_write(void *user, const void *data,
                                         size_t size, size_t *done)
{
    return cfw_tls_write((cfw_tls_conn_t *)user, data, size, done);
}

/* Attaches a TLS session to an initialized connection (ADR 0058):
 * the session borrows conn->http's fd, which remains the owner. The
 * transport bundle lives inside conn so the pointer stored in
 * conn->http.transport stays valid for the conn's life. */
static cfw_status_t cfw_server_conn_attach_tls(cfw_server_t *server,
                                               cfw_server_conn_t *conn)
{
    cfw_status_t st = cfw_tls_conn_create(
        server->options.tls_ctx, cfw_fd_get(&conn->http.fd), &conn->tls);
    if (st != CFW_OK) {
        return st;
    }
    conn->transport.user = conn->tls;
    conn->transport.read = cfw_server_tls_read;
    conn->transport.write = cfw_server_tls_write;
    conn->http.transport = &conn->transport;
    return CFW_OK;
}

static void cfw_server_update_conn_interest(cfw_server_t *server,
                                            cfw_server_conn_t *conn)
{
    uint32_t events = 0;
    if (conn->dead) {
        return;
    }
    /* The http1 layer may have closed the fd (close-after-write drain);
     * the cached handle still identifies the pollset registration, which
     * close_conn removes. Reap now so a recycled fd number cannot be
     * hijacked by a modify on a stale registration. */
    if (!cfw_fd_valid(&conn->http.fd)) {
        conn->dead = 1;
        return;
    }
    if (!conn->http.input_closed) {
        events |= CFW_POLL_IN;
    }
    if (cfw_http1_conn_pending_out(&conn->http) > 0) {
        events |= CFW_POLL_OUT;
    }
    if (conn->tls != NULL && !cfw_tls_handshake_done(conn->tls)) {
        /* Handshake progress is driven by the want flags: reads are
         * always possible, writes only when the session asked for
         * them — arming POLLOUT unconditionally would spin on a
         * writable socket while the peer stalls. */
        events |= CFW_POLL_IN;
        if (cfw_tls_conn_want_write(conn->tls)) {
            events |= CFW_POLL_OUT;
        }
    }
    if (events == 0) {
        conn->dead = 1;
        return;
    }
    if (events == conn->poll_events) {
        return;
    }
    if (cfw_pollset_modify(server->pollset, conn->handle, events,
                           conn) == CFW_OK) {
        conn->poll_events = events;
    }
}

static cfw_server_conn_t *cfw_server_find_conn(cfw_server_t *server,
                                               intptr_t handle)
{
    cfw_server_conn_t *conn;
    for (conn = server->conns; conn != NULL; conn = conn->next) {
        if (conn->handle == handle) {
            return conn;
        }
    }
    return NULL;
}

/* Adds an automatic Date header unless the response already has one.
 * Non-fatal on clock or scratch failure. `date` must be caller storage
 * that outlives the response-head format. */
static void cfw_server_add_date_header(cfw_response_t *res, char *date)
{
    int64_t unix_seconds;
    size_t hi;
    int have_date = 0;

    for (hi = 0; hi < res->header_count; hi++) {
        cfw_strview_t dn;
        int eq;
        cfw_strview_from_cstr(&dn, "Date");
        if (cfw_strview_equal_nocase(&res->headers[hi].name, &dn,
                                     &eq) == CFW_OK &&
            eq) {
            have_date = 1;
            break;
        }
    }
    if (have_date || cfw_time_wall_now(&unix_seconds) != CFW_OK ||
        cfw_http_date_format(unix_seconds, date, 30) != CFW_OK) {
        return;
    }
    {
        cfw_strview_t hn, hv;
        cfw_strview_from_cstr(&hn, "Date");
        cfw_strview_init(&hv, date, 29);
        cfw_response_header_add(res, &hn, &hv);
    }
}

static void cfw_server_send_error(cfw_server_t *server,
                                  cfw_server_conn_t *conn,
                                  cfw_http_status_t status,
                                  const char *log_detail)
{
    cfw_response_t res;
    cfw_request_t req_view;
    char head_buf[512];
    size_t written = 0;

    char date[30];
    memset(&res, 0, sizeof(res));
    res.status = status;
    res.headers = conn->headers;
    res.header_capacity = server->options.max_headers;
    cfw_server_add_date_header(&res, date);
    memset(&req_view, 0, sizeof(req_view));
    if (conn->have_head) {
        req_view.method = conn->head.request_line.method;
        req_view.version = conn->head.request_line.version;
    } else {
        req_view.version.major = 1;
        req_view.version.minor = 1;
    }
    if (cfw_dispatch_response_head_format(&req_view, &res, head_buf,
                                          sizeof(head_buf), &written) ==
        CFW_OK) {
        cfw_http1_conn_queue(&conn->http, head_buf, written);
    }
    server->errors_total++;
    if (conn->audit_pending) {
        conn->audit_status = (int)status;
        conn->audit_response_bytes += written;
        if (!conn->audit_stream) {
            cfw_server_emit_request_done(server, conn, 0);
        }
    }
    if (server->options.logger != NULL) {
        cfw_log(server->options.logger, CFW_LOG_WARN, "request error: %s",
                log_detail);
    }
    cfw_server_update_conn_interest(server, conn);
}

/* Removes every occurrence of a response header by name
 * (case-insensitive), compacting the array in place. Used to keep
 * handler-supplied framing headers from conflicting with the framing
 * the server emits. */
static void cfw_server_response_strip(cfw_response_t *res,
                                      const char *name)
{
    cfw_strview_t target;
    size_t i;
    size_t w = 0;

    cfw_strview_from_cstr(&target, name);
    for (i = 0; i < res->header_count; i++) {
        int eq = 0;
        if (cfw_strview_equal_nocase(&res->headers[i].name, &target,
                                     &eq) == CFW_OK &&
            eq) {
            continue;
        }
        res->headers[w++] = res->headers[i];
    }
    res->header_count = w;
}

static void cfw_server_handle_done(cfw_server_t *server,
                                   cfw_server_conn_t *conn)
{
    cfw_span_t body;
    cfw_response_t res;
    cfw_request_t req_view;
    cfw_dispatch_request_info_t info;
    char date[30];
    size_t written = 0;
    cfw_status_t st;
    int upgrade;

    server->requests_total++;
    conn->have_head = 0;

    memset(&body, 0, sizeof(body));
    body.data = conn->body.data;
    body.size = conn->body.size;

    memset(&res, 0, sizeof(res));
    res.headers = conn->headers;
    res.header_capacity = server->options.max_headers;

    /* A completed upload is finalized before dispatch so the handler
     * sees a consistent sink; the token then belongs to the app. */
    memset(&info, 0, sizeof(info));
    info.remote_addr = &conn->remote_addr;
    info.tls_conn = conn->tls;
    if (conn->upload != NULL) {
        if (server->options.upload_end != NULL) {
            server->options.upload_end(conn->upload, 1);
        }
        info.upload = conn->upload;
        conn->upload = NULL;
    }

    st = cfw_dispatch_request_ex(server->options.dispatch, &conn->head,
                                 &body, conn->headers,
                                 server->options.max_headers, conn->params,
                                 server->options.max_params, &res, &info);
    if (st != CFW_OK) {
        conn->dead = 1;
        return;
    }

    /* The server owns response framing: a handler-supplied
     * Transfer-Encoding cannot be honored (only identity bodies and
     * server-driven chunked streaming exist) and would corrupt the byte
     * stream, so it is always stripped. */
    cfw_server_response_strip(&res, "transfer-encoding");
    /* Content-Length is likewise server-managed whenever the response
     * may carry a body: a wrong value would corrupt the byte stream
     * (client hangs or reads into the next response). HEAD mirrors the
     * GET decision. No-body statuses (1xx/204/304) keep caller headers. */
    if (cfw_http_response_has_body(conn->head.request_line.method,
                                   res.status) ||
        (conn->head.request_line.method == CFW_HTTP_METHOD_HEAD &&
         cfw_http_response_has_body(CFW_HTTP_METHOD_GET, res.status))) {
        cfw_server_response_strip(&res, "content-length");
    }

    /* Protocol upgrade (ADR 0034): a 101 response to a request that
     * carried an Upgrade header takes the connection over when the
     * server upgrade hook is configured. Server-managed Connection and
     * streaming logic is skipped, and close_after_write is cleared —
     * the upgraded connection outlives HTTP keep-alive semantics. */
    upgrade = res.status == CFW_HTTP_STATUS_SWITCHING_PROTOCOLS &&
              server->options.upgrade != NULL &&
              cfw_server_head_has_upgrade(&conn->head);
    if (upgrade) {
        conn->http.close_after_write = 0;
    }

    /* Date header: added automatically unless the handler supplied
     * one. Wall-clock failure or a full header scratch is non-fatal. */
    cfw_server_add_date_header(&res, date);

    /* Streaming body: suppress the single-span body and frame via
     * chunked TE (HTTP/1.1) or close-delimited (older). While the
     * stream is active the connection must not auto-close on buffer
     * drain, so close_after_write is saved and cleared; it is restored
     * (or forced for close-delimited) when the stream ends. */
    if (!upgrade && res.stream != NULL &&
        cfw_http_response_has_body(conn->head.request_line.method,
                                   res.status)) {
        conn->stream_fn = res.stream;
        conn->stream_end = res.stream_end;
        conn->stream_user = res.stream_user;
        conn->stream_size = res.stream_size;
        conn->stream_sent = 0;
        conn->stream_chunk = (cfw_span_t){0};
        conn->stream_pos = 0;
        conn->stream_eof = 0;
        conn->stream_hold_close = conn->http.close_after_write;
        conn->http.close_after_write = 0;
        memset(&res.body, 0, sizeof(res.body));
        conn->stream_chunked = 0;
        /* Known-length streams use identity framing with a
         * server-emitted Content-Length on every HTTP version;
         * unknown-length streams keep chunked (1.1+) or
         * close-delimited framing. */
        if (conn->stream_size == CFW_STREAM_SIZE_UNKNOWN &&
            (conn->head.request_line.version.major > 1 ||
             (conn->head.request_line.version.major == 1 &&
              conn->head.request_line.version.minor >= 1))) {
            cfw_strview_t hn, hv;
            cfw_strview_from_cstr(&hn, "Transfer-Encoding");
            cfw_strview_from_cstr(&hv, "chunked");
            if (cfw_response_header_add(&res, &hn, &hv) == CFW_OK) {
                conn->stream_chunked = 1;
            }
        }
    } else {
        conn->stream_fn = NULL;
        conn->stream_end = NULL;
    }

    /* Signal connection persistence explicitly: HTTP/1.0 clients
     * default to close, so keep-alive must be announced; HTTP/1.1+
     * defaults to keep-alive, so only "close" needs signaling. A
     * close-delimited stream always ends in close. Header add failure
     * (full scratch) is non-fatal. Upgraded connections get no
     * automatic Connection header — the handler owns protocol headers. */
    if (!upgrade) {
        int is_http10 = conn->head.request_line.version.major < 1 ||
                        (conn->head.request_line.version.major == 1 &&
                         conn->head.request_line.version.minor == 0);
        int will_close = conn->http.close_after_write ||
                         (conn->stream_fn != NULL &&
                          (conn->stream_hold_close ||
                           (conn->stream_size == CFW_STREAM_SIZE_UNKNOWN &&
                            !conn->stream_chunked)));
        const char *token = will_close ? "close"
                                       : (is_http10 ? "keep-alive" : NULL);
        if (token != NULL) {
            cfw_strview_t hn, hv;
            cfw_strview_from_cstr(&hn, "Connection");
            cfw_strview_init(&hv, token, strlen(token));
            cfw_response_header_add(&res, &hn, &hv);
        }
    }

    memset(&req_view, 0, sizeof(req_view));
    req_view.method = conn->head.request_line.method;
    req_view.version = conn->head.request_line.version;

    /* Format into the per-connection scratch; grow once if needed. */
    if (conn->head_scratch.capacity == 0) {
        cfw_buffer_reserve(&conn->head_scratch,
                           server->options.http.max_head_size);
    }
    st = cfw_dispatch_response_head_format(
        &req_view, &res, (char *)conn->head_scratch.data,
        conn->head_scratch.capacity, &written);
    if (st == CFW_ERROR_INVALID_ARGUMENT &&
        conn->head_scratch.capacity < 65536) {
        cfw_buffer_reserve(&conn->head_scratch,
                           conn->head_scratch.capacity * 2);
        st = cfw_dispatch_response_head_format(
            &req_view, &res, (char *)conn->head_scratch.data,
            conn->head_scratch.capacity, &written);
    }
    if (st != CFW_OK ||
        cfw_http1_conn_queue(&conn->http, conn->head_scratch.data,
                             written) != CFW_OK) {
        conn->dead = 1;
        return;
    }
    if (cfw_http_response_has_body(conn->head.request_line.method,
                                   res.status) &&
        res.body.size != 0 &&
        cfw_http1_conn_queue(&conn->http, res.body.data,
                             res.body.size) != CFW_OK) {
        conn->dead = 1;
        return;
    }

    /* Audit accounting (ADR 0055): count exactly what was queued —
     * the serialized head plus the body payload when one was emitted
     * (streams carry res.body.size == 0 here and are counted by the
     * pump instead). */
    if (conn->audit_pending) {
        conn->audit_status = (int)res.status;
        conn->audit_response_bytes += written;
        if (res.body.size != 0 &&
            cfw_http_response_has_body(conn->head.request_line.method,
                                       res.status)) {
            conn->audit_response_bytes += res.body.size;
        }
    }

    cfw_buffer_clear(&conn->body);
    if (conn->stream_fn != NULL) {
        cfw_server_stream_pump(server, conn);
    }

    /* Always re-arm: the request deadline must not fire while the
     * response drains. keep_alive_timeout doubles as the drain deadline
     * for closing connections and as the idle deadline for upgraded
     * connections (0 disables, matching keep-alive). */
    cfw_server_conn_deadline(conn, server->options.keep_alive_timeout);
    if (upgrade) {
        cfw_server_activate_upgrade(server, conn);
    }
    /* Non-stream responses are complete now; streamed ones emit when
     * the pump finishes (or the conn closes mid-stream). A stream
     * that already finished inside the pump has cleared audit_pending,
     * making this a no-op. */
    if (conn->stream_fn != NULL) {
        conn->audit_stream = 1;
    } else {
        cfw_server_emit_request_done(server, conn, upgrade);
    }
}


/* Classifies the request's Expect header(s):
 * 0 = absent, 1 = every expectation is "100-continue",
 * 2 = at least one unrecognized expectation (-> 417 per RFC 9110).
 * Values are comma-separated token lists per RFC 9110 section 10.1.1. */
static int cfw_server_expect_kind(const cfw_http_request_head_t *head)
{
    cfw_strview_t block = head->headers;
    size_t cursor = 0;
    int saw_expect = 0;
    int unknown = 0;

    for (;;) {
        cfw_http_header_t field;
        int done = 0;
        int is_expect = 0;
        cfw_strview_t expect_name;
        size_t pos;

        if (cfw_http_headers_next(&block, &cursor, &field, &done) !=
                CFW_OK ||
            done) {
            break;
        }
        cfw_strview_from_cstr(&expect_name, "expect");
        if (cfw_strview_equal_nocase(&field.name, &expect_name,
                                     &is_expect) != CFW_OK ||
            !is_expect) {
            continue;
        }
        saw_expect = 1;

        /* Split the value on commas; trim SP/HTAB around each token and
         * require "100-continue" (case-insensitive). */
        pos = 0;
        while (pos <= field.value.size) {
            size_t ts;
            size_t te;
            cfw_strview_t token;
            cfw_strview_t want;
            int eq = 0;

            ts = pos;
            while (pos < field.value.size &&
                   field.value.data[pos] != ',') {
                pos++;
            }
            te = pos;
            if (pos < field.value.size) {
                pos++; /* skip the comma */
            }
            while (ts < te &&
                   (field.value.data[ts] == ' ' ||
                    field.value.data[ts] == '\t')) {
                ts++;
            }
            while (te > ts &&
                   (field.value.data[te - 1] == ' ' ||
                    field.value.data[te - 1] == '\t')) {
                te--;
            }
            cfw_strview_init(&token, field.value.data + ts, te - ts);
            cfw_strview_from_cstr(&want, "100-continue");
            if (cfw_strview_equal_nocase(&token, &want, &eq) != CFW_OK ||
                !eq) {
                unknown = 1;
            }
            if (pos >= field.value.size) {
                break;
            }
        }
    }
    if (!saw_expect) {
        return 0;
    }
    return unknown ? 2 : 1;
}

/* Queues an interim "100 Continue" response. Called at HEAD time when
 * cfw_server_expect_kind classified every Expect token as
 * "100-continue", so clients that wait for the interim before sending
 * a body are not stalled. Queue failure is non-fatal: the final
 * response still goes out. */
static void cfw_server_expect_continue(cfw_server_t *server,
                                       cfw_server_conn_t *conn)
{
    static const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
    (void)server;
    cfw_http1_conn_queue(&conn->http, interim, sizeof(interim) - 1);
}


#define CFW_SERVER_STREAM_PIECE 16384u

/* Emits the next piece of the active stream. Chunked responses frame
 * each piece as its own chunk (legal per RFC 9112); HTTP/1.0 responses
 * are close-delimited raw bytes. Returns when the output queue is full
 * (backpressure), the stream ends, or the connection dies. */
static void cfw_server_stream_pump(cfw_server_t *server,
                                   cfw_server_conn_t *conn)
{
    while (conn->stream_fn != NULL && !conn->dead) {
        cfw_span_t piece;
        size_t free_bytes;
        size_t overhead;
        size_t n;

        /* A declared-length stream is complete once stream_size
         * bytes have been queued; the response does not require the
         * stream to signal eof, and source bytes beyond the declared
         * length are simply never emitted. */
        if (conn->stream_size != CFW_STREAM_SIZE_UNKNOWN &&
            conn->stream_sent == conn->stream_size) {
            conn->stream_eof = 1;
        }

        /* Fetch the next chunk when the current one is exhausted. */
        if (!conn->stream_eof &&
            conn->stream_pos >= conn->stream_chunk.size) {
            int eof = 0;
            cfw_span_t next;
            memset(&next, 0, sizeof(next));
            if (conn->stream_fn(conn->stream_user, &next, &eof) != CFW_OK) {
                conn->dead = 1;
                return;
            }
            if (eof) {
                conn->stream_eof = 1;
            } else {
                if (next.size == 0 || next.data == NULL) {
                    /* Contract violation: non-empty progress required. */
                    conn->dead = 1;
                    return;
                }
                conn->stream_chunk = next;
                conn->stream_pos = 0;
            }
        }

        if (conn->stream_eof) {
            /* A declared-length stream must have yielded exactly
             * stream_size bytes; a mismatch would corrupt framing,
             * so the connection is closed instead of guessing. */
            if (conn->stream_size != CFW_STREAM_SIZE_UNKNOWN &&
                conn->stream_sent != conn->stream_size) {
                conn->dead = 1;
                return;
            }
            /* Terminal chunk for chunked; close-delimited needs
             * none; known-length identity keeps the connection. */
            if (conn->stream_chunked) {
                static const char terminal[] = "0\r\n\r\n";
                if (cfw_http1_conn_queue(&conn->http, terminal,
                                         sizeof(terminal) - 1) != CFW_OK) {
                    return; /* retry on next pump */
                }
                conn->http.close_after_write = conn->stream_hold_close;
            } else if (conn->stream_size == CFW_STREAM_SIZE_UNKNOWN) {
                conn->http.close_after_write = 1;
            } else {
                conn->http.close_after_write = conn->stream_hold_close;
            }
            conn->stream_fn = NULL;
            if (conn->stream_end != NULL) {
                conn->stream_end(conn->stream_user);
                conn->stream_end = NULL;
            }
            /* Stream fully emitted: the audit record is complete. */
            cfw_server_emit_request_done(server, conn, 0);
            return;
        }

        /* Bound the piece by queue free space and the piece cap. */
        free_bytes = conn->http.max_out_size -
                     cfw_http1_conn_pending_out(&conn->http);
        overhead = conn->stream_chunked ? 24u : 0u;
        if (free_bytes <= overhead) {
            if (cfw_http1_conn_flush(&conn->http) == CFW_ERROR_IO) {
                conn->dead = 1;
                return;
            }
            free_bytes = conn->http.max_out_size -
                         cfw_http1_conn_pending_out(&conn->http);
            if (free_bytes <= overhead) {
                return; /* still full: wait for POLL_OUT */
            }
        }
        n = conn->stream_chunk.size - conn->stream_pos;
        if (n > CFW_SERVER_STREAM_PIECE) {
            n = CFW_SERVER_STREAM_PIECE;
        }
        if (n > free_bytes - overhead) {
            n = free_bytes - overhead;
        }
        if (conn->stream_size != CFW_STREAM_SIZE_UNKNOWN) {
            uint64_t remaining = conn->stream_size - conn->stream_sent;
            if ((uint64_t)n > remaining) {
                n = (size_t)remaining;
            }
        }
        if (n == 0) {
            return;
        }

        piece.data = conn->stream_chunk.data + conn->stream_pos;
        piece.size = n;
        if (conn->stream_chunked) {
            char frame[40];
            size_t fn = 0;
            int i;
            static const char hexd[] = "0123456789abcdef";
            uint64_t v = n;
            char tmp[16];
            size_t tn = 0;
            do {
                tmp[tn++] = hexd[v & 15];
                v >>= 4;
            } while (v != 0);
            for (i = (int)tn - 1; i >= 0; i--) {
                frame[fn++] = tmp[i];
            }
            frame[fn++] = '\r';
            frame[fn++] = '\n';
            /* Piece size was bounded by free space minus overhead, so
             * all three queues fit; any failure means the fd is gone. */
            if (cfw_http1_conn_queue(&conn->http, frame, fn) != CFW_OK ||
                cfw_http1_conn_queue(&conn->http, piece.data, piece.size) !=
                    CFW_OK ||
                cfw_http1_conn_queue(&conn->http, "\r\n", 2) != CFW_OK) {
                conn->dead = 1;
                return;
            }
        } else {
            if (cfw_http1_conn_queue(&conn->http, piece.data, piece.size) !=
                CFW_OK) {
                conn->dead = 1;
                return;
            }
        }
        conn->stream_pos += n;
        conn->stream_sent += n;
        conn->audit_response_bytes += n;
        /* Progress was made: re-arm the drain deadline so the timeout
         * measures stream *inactivity* (a stalled client is still
         * killed), not total stream duration. */
        cfw_server_conn_deadline(conn,
                                 server->options.keep_alive_timeout);

        if (cfw_http1_conn_flush(&conn->http) == CFW_ERROR_IO) {
            conn->dead = 1;
            return;
        }
    }
}

/* RFC 9112 section 3.2: an HTTP/1.1 request must carry a Host field
 * with a non-empty value. Returns 1 when a valid Host is present. */
static int cfw_server_head_has_host(const cfw_http_request_head_t *head)
{
    cfw_strview_t block;
    size_t cursor = 0;
    int done = 0;

    if (head == NULL) {
        return 0;
    }
    block = head->headers;
    for (;;) {
        cfw_http_header_t header;
        cfw_strview_t host;
        int eq;
        cfw_status_t st;

        memset(&header, 0, sizeof(header));
        st = cfw_http_headers_next(&block, &cursor, &header, &done);
        if (st != CFW_OK || done) {
            return 0;
        }
        cfw_strview_from_cstr(&host, "Host");
        st = cfw_strview_equal_nocase(&header.name, &host, &eq);
        if (st == CFW_OK && eq && header.value.size != 0) {
            return 1;
        }
    }
}

/* Returns 1 when the request head carries an Upgrade header field
 * (any value; the handler validates the protocol token). This is only
 * a takeover guard for ADR 0034 — semantics are the app's job. */
static int cfw_server_head_has_upgrade(const cfw_http_request_head_t *head)
{
    cfw_strview_t block;
    cfw_strview_t upgrade_name;
    size_t cursor = 0;
    int done = 0;

    if (head == NULL) {
        return 0;
    }
    cfw_strview_from_cstr(&upgrade_name, "upgrade");
    block = head->headers;
    for (;;) {
        cfw_http_header_t header;
        int eq = 0;
        if (cfw_http_headers_next(&block, &cursor, &header, &done) !=
                CFW_OK ||
            done) {
            return 0;
        }
        if (cfw_strview_equal_nocase(&header.name, &upgrade_name, &eq) ==
                CFW_OK &&
            eq) {
            return 1;
        }
    }
}

/* Takes over an upgraded connection (ADR 0034): stages any input bytes
 * the HTTP reader already buffered past the request, marks the conn
 * upgraded, and invokes the app hook once with CFW_UPGRADE_EV_READY.
 * Staging failure drops the connection rather than corrupting the new
 * protocol's byte stream. */
static void cfw_server_activate_upgrade(cfw_server_t *server,
                                        cfw_server_conn_t *conn)
{
    cfw_http1_server_t *reader = &conn->http.server;
    size_t avail = reader->pending.size - reader->pending_pos;

    if (avail != 0 &&
        cfw_buffer_append(&conn->upgrade_pending,
                          reader->pending.data + reader->pending_pos,
                          avail) != CFW_OK) {
        conn->dead = 1;
        return;
    }
    conn->upgraded = 1;
    if (server->options.upgrade(conn, CFW_UPGRADE_EV_READY,
                                server->options.upgrade_user) != CFW_OK) {
        conn->dead = 1;
    }
}

/* Services an upgraded connection: delivers readiness events to the
 * app hook and keeps the bounded output queue draining. App callbacks
 * stop once cfw_upgraded_close begins the graceful close. */
static void cfw_server_service_upgraded(cfw_server_t *server,
                                        cfw_server_conn_t *conn,
                                        uint32_t events)
{
    uint32_t mask = 0;
    int had_pending;

    if (conn->upgrade_closing) {
        if ((events & CFW_POLL_OUT) &&
            cfw_http1_conn_flush(&conn->http) == CFW_ERROR_IO) {
            conn->dead = 1;
        }
        return;
    }

    had_pending = cfw_http1_conn_pending_out(&conn->http) != 0;
    if (events & CFW_POLL_IN) {
        mask |= CFW_UPGRADE_EV_READ;
    }
    if (events & CFW_POLL_ERR) {
        mask |= CFW_UPGRADE_EV_ERR;
    }
    if ((events & CFW_POLL_HUP) && !conn->upgrade_hup_seen) {
        conn->upgrade_hup_seen = 1;
        mask |= CFW_UPGRADE_EV_HUP;
    }
    if ((events & CFW_POLL_OUT) &&
        cfw_http1_conn_flush(&conn->http) == CFW_ERROR_IO) {
        conn->dead = 1;
    }
    if (!conn->dead && had_pending &&
        cfw_http1_conn_pending_out(&conn->http) == 0) {
        mask |= CFW_UPGRADE_EV_WRITE;
    }
    if (conn->dead || mask == 0) {
        return;
    }
    /* Activity refreshes the idle deadline. */
    cfw_server_conn_deadline(conn, server->options.keep_alive_timeout);
    if (server->options.upgrade(conn, mask,
                                server->options.upgrade_user) != CFW_OK) {
        conn->dead = 1;
    }
}

static void cfw_server_service_read(cfw_server_t *server,
                                    cfw_server_conn_t *conn)
{
    for (;;) {
        cfw_http1_event_t event;
        cfw_status_t st = cfw_http1_conn_next_event(&conn->http, &event);

        if (st == CFW_ERROR_WOULDBLOCK) {
            break;
        }
        if (st != CFW_OK) {
            conn->dead = 1;
            break;
        }

        switch (event.kind) {
        case CFW_HTTP1_EV_HEAD:
            conn->head = event.head;
            conn->have_head = 1;
            /* Arm the audit record for this request (ADR 0055). */
            conn->audit_pending = 1;
            conn->audit_stream = 0;
            conn->audit_status = 0;
            conn->audit_request_bytes = 0;
            conn->audit_response_bytes = 0;
            (void)cfw_time_now(&conn->audit_start);
            cfw_buffer_clear(&conn->body);
            /* Defensive: a previous request's upload should already be
             * ended; if one leaked, report it incomplete. */
            if (conn->upload != NULL) {
                if (server->options.upload_end != NULL) {
                    server->options.upload_end(conn->upload, 0);
                }
                conn->upload = NULL;
            }
            if ((conn->head.request_line.version.major > 1 ||
                 (conn->head.request_line.version.major == 1 &&
                  conn->head.request_line.version.minor >= 1)) &&
                !cfw_server_head_has_host(&conn->head)) {
                cfw_server_send_error(server, conn,
                                      CFW_HTTP_STATUS_BAD_REQUEST,
                                      "missing host");
                cfw_http1_conn_close_after_write(&conn->http);
                conn->rejecting = 1;
                cfw_server_conn_deadline(
                    conn, server->options.keep_alive_timeout);
                break;
            }
            {
                int expect = cfw_server_expect_kind(&conn->head);
                if (expect == 2) {
                    cfw_server_send_error(server, conn,
                                          CFW_HTTP_STATUS_EXPECTATION_FAILED,
                                          "unsupported expectation");
                    cfw_http1_conn_close_after_write(&conn->http);
                    conn->rejecting = 1;
                    cfw_server_conn_deadline(
                        conn, server->options.keep_alive_timeout);
                    break;
                }
                if (expect == 1) {
                    cfw_server_expect_continue(server, conn);
                }
            }
            /* Upload hooks: begin once per request whose framing
             * declares a non-empty body, after all head validation so
             * rejected requests never allocate an upload. */
            if (conn->upload == NULL &&
                server->options.upload_begin != NULL) {
                cfw_http_message_framing_t fr;
                memset(&fr, 0, sizeof(fr));
                if (cfw_http_request_framing(&conn->head, &fr) == CFW_OK &&
                    fr.framing != CFW_HTTP_BODY_NONE &&
                    (fr.framing != CFW_HTTP_BODY_CONTENT_LENGTH ||
                     fr.content_length > 0)) {
                    void *up = NULL;
                    if (server->options.upload_begin(
                            server->options.upload_user, &conn->head,
                            &up) != CFW_OK ||
                        up == NULL) {
                        cfw_server_send_error(
                            server, conn,
                            CFW_HTTP_STATUS_INTERNAL_SERVER_ERROR,
                            "upload begin failed");
                        cfw_http1_conn_close_after_write(&conn->http);
                        conn->rejecting = 1;
                        cfw_server_conn_deadline(
                            conn, server->options.keep_alive_timeout);
                        break;
                    }
                    conn->upload = up;
                }
            }
            cfw_server_conn_deadline(conn,
                                     server->options.request_timeout);
            break;
        case CFW_HTTP1_EV_BODY:
            conn->audit_request_bytes += event.body.size;
            if (conn->upload != NULL) {
                if (server->options.upload_write(conn->upload,
                                                 &event.body) != CFW_OK) {
                    if (server->options.upload_end != NULL) {
                        server->options.upload_end(conn->upload, 0);
                    }
                    conn->upload = NULL;
                    cfw_server_send_error(
                        server, conn,
                        CFW_HTTP_STATUS_INTERNAL_SERVER_ERROR,
                        "upload write failed");
                    cfw_http1_conn_close_after_write(&conn->http);
                    conn->rejecting = 1;
                    cfw_server_conn_deadline(
                        conn, server->options.keep_alive_timeout);
                }
            } else if (cfw_buffer_append(&conn->body, event.body.data,
                                         event.body.size) != CFW_OK) {
                conn->dead = 1;
            }
            break;
        case CFW_HTTP1_EV_MESSAGE_DONE:
            if (conn->have_head) {
                cfw_server_handle_done(server, conn);
            }
            break;
        case CFW_HTTP1_EV_ERROR:
            if (conn->upload != NULL) {
                if (server->options.upload_end != NULL) {
                    server->options.upload_end(conn->upload, 0);
                }
                conn->upload = NULL;
            }
            cfw_server_send_error(
                server, conn,
                event.error_kind == CFW_HTTP1_ERR_BODY_TOO_LARGE
                    ? CFW_HTTP_STATUS_PAYLOAD_TOO_LARGE
                    : CFW_HTTP_STATUS_BAD_REQUEST,
                event.error_kind == CFW_HTTP1_ERR_BODY_TOO_LARGE
                    ? "body too large"
                    : "protocol error");
            break;
        }
        if (conn->dead || conn->stream_fn != NULL || conn->rejecting ||
            conn->upgraded) {
            break;
        }
    }
}

static void cfw_server_service_conn(cfw_server_t *server,
                                    cfw_server_conn_t *conn, uint32_t events)
{
    if (conn == NULL || conn->dead) {
        return;
    }
    if (conn->upgraded) {
        cfw_server_service_upgraded(server, conn, events);
        cfw_server_update_conn_interest(server, conn);
        return;
    }
    /* Mixed-listener protocol sniff (ADR 0061): the first readable
     * event peeks one byte — a TLS handshake record (content type
     * 0x16) attaches TLS, anything else stays cleartext. MSG_PEEK
     * consumes nothing, so the chosen protocol sees all bytes. */
    if (conn->tls_detect) {
        if (events & (CFW_POLL_IN | CFW_POLL_ERR | CFW_POLL_HUP)) {
            unsigned char b = 0;
            ssize_t n = recv((int)conn->http.fd.handle, &b, 1,
                             MSG_PEEK | MSG_DONTWAIT);
            if (n == 1) {
                if (b == 0x16u &&
                    cfw_server_conn_attach_tls(server, conn) != CFW_OK) {
                    conn->dead = 1;
                    return;
                }
                conn->tls_detect = 0;
            } else if (n == 0) {
                conn->dead = 1;
                return;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                conn->dead = 1;
                return;
            }
            /* EAGAIN: spurious wakeup — keep waiting for the first
             * byte with POLL_IN still armed below. */
        }
        if (conn->tls_detect) {
            cfw_server_update_conn_interest(server, conn);
            return;
        }
    }
    /* TLS handshake gate (ADR 0058): no HTTP bytes are parsed until
     * the handshake completes. Retries happen on every event in
     * either direction; a fatal TLS error kills the connection. */
    if (conn->tls != NULL && !cfw_tls_handshake_done(conn->tls)) {
        cfw_status_t hs = cfw_tls_handshake(conn->tls);
        if (hs == CFW_ERROR_IO) {
            conn->dead = 1;
        } else if (hs != CFW_OK) {
            /* WOULDBLOCK: wait for the indicated direction. */
            cfw_server_update_conn_interest(server, conn);
            return;
        }
        if (conn->dead) {
            return;
        }
    }
    if (!conn->rejecting && conn->stream_fn == NULL &&
        (events & (CFW_POLL_IN | CFW_POLL_ERR | CFW_POLL_HUP))) {
        if (!conn->http.input_closed) {
            cfw_server_service_read(server, conn);
        }
    }
    if (!conn->dead && (events & CFW_POLL_OUT)) {
        if (cfw_http1_conn_flush(&conn->http) == CFW_ERROR_IO) {
            conn->dead = 1;
        }
    }
    if (!conn->dead && conn->stream_fn != NULL) {
        cfw_server_stream_pump(server, conn);
    }
    /* Opportunistic flush: most responses fit the socket buffer, so
     * writing now avoids an extra epoll round-trip per request. A
     * WOULDBLOCK result simply leaves POLLOUT armed below. */
    if (!conn->dead &&
        cfw_http1_conn_pending_out(&conn->http) > 0 &&
        cfw_http1_conn_flush(&conn->http) == CFW_ERROR_IO) {
        conn->dead = 1;
    }
    cfw_server_update_conn_interest(server, conn);
}

/* Formats the peer address as "ip:port" ("[v6]:port" for IPv6) into
 * conn->remote_addr_buf and points conn->remote_addr at it. Failure
 * leaves remote_addr empty — non-fatal. */
static void cfw_server_format_peer(cfw_server_conn_t *conn,
                                   const cfw_addr_t *peer)
{
    char ip[48];
    uint16_t port = 0;
    int is6;
    int n;

    conn->remote_addr.data = NULL;
    conn->remote_addr.size = 0;
    is6 = cfw_addr_is_ipv6(peer);
    if (cfw_addr_to_str(peer, ip, sizeof(ip)) != CFW_OK ||
        cfw_addr_port(peer, &port) != CFW_OK) {
        return;
    }
    n = is6 ? snprintf(conn->remote_addr_buf,
                       sizeof(conn->remote_addr_buf), "[%s]:%u", ip,
                       (unsigned)port)
            : snprintf(conn->remote_addr_buf,
                       sizeof(conn->remote_addr_buf), "%s:%u", ip,
                       (unsigned)port);
    if (n <= 0 || (size_t)n >= sizeof(conn->remote_addr_buf)) {
        conn->remote_addr_buf[0] = '\0';
        return;
    }
    conn->remote_addr.data = conn->remote_addr_buf;
    conn->remote_addr.size = (size_t)n;
}

static void cfw_server_accept(cfw_server_t *server)
{
    cfw_http1_conn_options_t conn_options;

    conn_options.http = server->options.http;
    conn_options.max_out_size = server->options.max_out_size;

    for (;;) {
        cfw_fd_t fd;
        cfw_addr_t peer;
        cfw_server_conn_t *conn;
        cfw_status_t st;

        cfw_fd_init(&fd);
        st = cfw_net_accept(&server->listener, &peer, &fd);
        if (st == CFW_ERROR_WOULDBLOCK) {
            break;
        }
        if (st != CFW_OK) {
            break;
        }
        server->accepted_total++;

        if (server->conn_count >= server->options.max_connections ||
            server->stopping) {
            cfw_fd_close(&fd);
            continue;
        }

        conn = calloc(1, sizeof(*conn));
        if (conn == NULL) {
            cfw_fd_close(&fd);
            continue;
        }
        /* The http1 connection is always initialized first and owns
         * the fd; TLS (when configured) is attached afterwards and
         * borrows the descriptor (ADR 0058). With tls_detect the
         * protocol is sniffed on the first readable event instead
         * (ADR 0061). */
        conn_options.transport = NULL;
        st = cfw_http1_conn_init(&conn->http, &fd, &conn_options);
        if (st != CFW_OK) {
            /* init failed before taking fd ownership. */
            cfw_fd_close(&fd);
            free(conn);
            continue;
        }
        if (server->options.tls_ctx != NULL) {
            if (server->options.tls_detect) {
                conn->tls_detect = 1;
            } else if (cfw_server_conn_attach_tls(server, conn) !=
                       CFW_OK) {
                cfw_http1_conn_free(&conn->http);
                free(conn);
                continue;
            }
        }
        conn->headers = malloc(server->options.max_headers *
                               sizeof(cfw_http_header_t));
        conn->params = malloc(server->options.max_params *
                              sizeof(cfw_route_param_t));
        if ((server->options.max_headers != 0 &&
             conn->headers == NULL) ||
            (server->options.max_params != 0 && conn->params == NULL) ||
            cfw_buffer_init(&conn->body) != CFW_OK ||
            cfw_buffer_init(&conn->head_scratch) != CFW_OK ||
            cfw_buffer_init(&conn->upgrade_pending) != CFW_OK) {
            cfw_http1_conn_free(&conn->http);
            if (conn->tls != NULL) {
                cfw_tls_conn_destroy(conn->tls);
            }
            free(conn->headers);
            free(conn->params);
            free(conn);
            continue;
        }

        cfw_server_format_peer(conn, &peer);

        conn->handle = cfw_fd_get(&conn->http.fd);
        conn->next = server->conns;
        server->conns = conn;
        server->conn_count++;
        if (server->options.logger != NULL) {
            cfw_log(server->options.logger, CFW_LOG_DEBUG,
                    "connection accepted (%zu active)", server->conn_count);
        }
        cfw_pollset_add(server->pollset, conn->handle, CFW_POLL_IN,
                        conn);
        conn->poll_events = CFW_POLL_IN;
        cfw_server_conn_deadline(conn, server->options.request_timeout);
        cfw_server_update_listener(server);
    }
}

static cfw_status_t cfw_server_compute_deadline(
    cfw_server_t *server, const cfw_timepoint_t *wait_deadline,
    cfw_timepoint_t *effective)
{
    cfw_server_conn_t *conn;
    int have = 0;

    *effective = (cfw_timepoint_t){0};
    if (wait_deadline != NULL) {
        *effective = *wait_deadline;
        have = 1;
    }
    for (conn = server->conns; conn != NULL; conn = conn->next) {
        int cmp = 1;
        if (!conn->deadline_set || conn->dead) {
            continue;
        }
        if (!have) {
            *effective = conn->deadline;
            have = 1;
            continue;
        }
        if (cfw_timepoint_compare(&conn->deadline, effective, &cmp) ==
                CFW_OK &&
            cmp < 0) {
            *effective = conn->deadline;
        }
    }
    return have ? CFW_OK : CFW_ERROR_WOULDBLOCK;
}

cfw_status_t cfw_server_step(cfw_server_t *server,
                             const cfw_timepoint_t *wait_deadline)
{
    cfw_poll_event_t events[CFW_SERVER_MAX_EVENTS];
    cfw_timepoint_t deadline;
    cfw_timepoint_t *deadline_ptr = NULL;
    size_t count = 0;
    size_t i;
    cfw_status_t st;

    if (server == NULL || server->pollset == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    cfw_server_sweep(server);
    if (cfw_server_compute_deadline(server, wait_deadline, &deadline) ==
        CFW_OK) {
        deadline_ptr = &deadline;
    }

    st = cfw_pollset_wait(server->pollset, deadline_ptr, events,
                          CFW_SERVER_MAX_EVENTS, &count);
    if (st == CFW_ERROR_TIMEOUT) {
        /* Deadline reached with no events: still sweep expired
         * connections, then report a normal iteration. */
        cfw_server_sweep(server);
        return CFW_OK;
    }
    if (st != CFW_OK) {
        return st;
    }

    cfw_server_sweep(server);

    for (i = 0; i < count; i++) {
        intptr_t handle = events[i].handle;

        if (handle == cfw_fd_get(&server->wakeup[0])) {
            char drain[64];
            int drain_flags = 0;
#if CFW_HAVE_MSG_DONTWAIT
            drain_flags = MSG_DONTWAIT;
#endif
            while (recv((int)handle, drain, sizeof(drain),
                        drain_flags) > 0) {
            }
            continue;
        }
        if (handle == cfw_fd_get(&server->listener)) {
            cfw_server_accept(server);
            continue;
        }
        cfw_server_service_conn(server,
                                cfw_server_find_conn(server, handle),
                                events[i].events);
    }

    cfw_server_sweep(server);
    cfw_server_update_listener(server);
    return CFW_OK;
}

cfw_status_t cfw_server_run(cfw_server_t *server)
{
    cfw_status_t st;

    if (server == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    while (!server->stopping) {
        st = cfw_server_step(server, NULL);
        if (st != CFW_OK) {
            return st;
        }
    }

    /* Drain phase: serve remaining connections until they finish or
     * drain_timeout expires. New accepts are already disabled. */
    if (server->options.drain_timeout.nanoseconds != 0 &&
        server->conn_count > 0) {
        cfw_timepoint_t drain_deadline;
        int expired = 0;
        if (cfw_deadline_from_now(&drain_deadline,
                                  server->options.drain_timeout) ==
            CFW_OK) {
            while (server->conn_count > 0) {
                st = cfw_server_step(server, &drain_deadline);
                if (st != CFW_OK) {
                    return st;
                }
                if (cfw_deadline_expired(&drain_deadline, &expired) ==
                        CFW_OK &&
                    expired) {
                    break;
                }
            }
        }
    }
    return CFW_OK;
}

cfw_status_t cfw_server_init(cfw_server_t *server,
                             const cfw_server_options_t *options)
{
    cfw_addr_t addr;
    cfw_status_t st;
    int sv[2];

    if (server == NULL || options == NULL || options->dispatch == NULL ||
        options->max_connections == 0 || options->max_out_size == 0 ||
        options->http.max_head_size == 0 ||
        options->http.max_pending_size == 0 ||
        options->max_headers >
            SIZE_MAX / sizeof(cfw_http_header_t) ||
        options->max_params > SIZE_MAX / sizeof(cfw_route_param_t) ||
        (options->upload_begin == NULL) !=
            (options->upload_write == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (options->tls_ctx != NULL && !CFW_HAVE_TLS) {
        return CFW_ERROR_UNAVAILABLE;
    }
    if (options->tls_detect && options->tls_ctx == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    memset(server, 0, sizeof(*server));
    cfw_fd_init(&server->listener);
    cfw_fd_init(&server->wakeup[0]);
    cfw_fd_init(&server->wakeup[1]);
    server->options = *options;

    st = cfw_pollset_create(&server->pollset);
    if (st != CFW_OK) {
        return st;
    }

    /* Wake channel for prompt stop. SOCK_NONBLOCK/SOCK_CLOEXEC are not
     * portable (absent on macOS/BSD), so the pair is created blocking
     * and configured with fcntl afterwards. */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        cfw_pollset_destroy(server->pollset);
        server->pollset = NULL;
        return CFW_ERROR_IO;
    }
    {
        int i;
        for (i = 0; i < 2; i++) {
            int fl = fcntl(sv[i], F_GETFL, 0);
            int fdfl = fcntl(sv[i], F_GETFD, 0);
            if (fl < 0 || fdfl < 0 ||
                fcntl(sv[i], F_SETFL, fl | O_NONBLOCK) != 0 ||
                fcntl(sv[i], F_SETFD, fdfl | FD_CLOEXEC) != 0) {
                close(sv[0]);
                close(sv[1]);
                cfw_pollset_destroy(server->pollset);
                server->pollset = NULL;
                return CFW_ERROR_IO;
            }
#if !CFW_HAVE_MSG_NOSIGNAL && CFW_HAVE_SO_NOSIGPIPE
            {
                int one = 1;
                (void)setsockopt(sv[i], SOL_SOCKET, SO_NOSIGPIPE, &one,
                                 sizeof(one));
            }
#endif
        }
    }
    server->wakeup[0].handle = sv[0];
    server->wakeup[1].handle = sv[1];
    cfw_pollset_add(server->pollset, sv[0], CFW_POLL_IN,
                    CFW_SERVER_WAKE_TOKEN);

    /* Listener. */
    st = cfw_net_socket(options->ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0,
                        &server->listener);
    if (st != CFW_OK) {
        goto fail;
    }
    /* SO_REUSEPORT (ADR 0056) must be set before bind; every instance
     * sharing the port must opt in or the bind fails. */
    if (options->reuse_port) {
#if CFW_HAVE_SO_REUSEPORT
        int one = 1;
        if (setsockopt((int)cfw_fd_get(&server->listener), SOL_SOCKET,
                       SO_REUSEPORT, &one, sizeof(one)) != 0) {
            st = CFW_ERROR_IO;
            goto fail;
        }
#else
        st = CFW_ERROR_UNAVAILABLE;
        goto fail;
#endif
    }
    if (options->host != NULL) {
        st = cfw_addr_resolve(&addr, options->host, options->port,
                              options->ipv6 ? CFW_ADDR_FAMILY_IPV6
                                            : CFW_ADDR_FAMILY_IPV4);
    } else {
        st = cfw_addr_any(options->port, options->ipv6, &addr);
    }
    if (st != CFW_OK || cfw_net_bind(&server->listener, &addr) != CFW_OK ||
        cfw_net_listen(&server->listener,
                       options->backlog > 0 ? options->backlog : 128) !=
            CFW_OK) {
        st = CFW_ERROR_IO;
        goto fail;
    }
    server->listening = 1;

    cfw_server_update_listener(server);
    return CFW_OK;

fail:
    cfw_fd_close(&server->wakeup[0]);
    cfw_fd_close(&server->wakeup[1]);
    cfw_fd_close(&server->listener);
    cfw_pollset_destroy(server->pollset);
    server->pollset = NULL;
    return st;
}

void cfw_server_stop(cfw_server_t *server)
{
    char byte = 1;

    if (server == NULL || server->stopping) {
        return;
    }
    server->stopping = 1;
    if (server->options.logger != NULL) {
        cfw_log(server->options.logger, CFW_LOG_INFO,
                "server stopping");
    }
    cfw_server_update_listener(server);
    if (cfw_fd_valid(&server->wakeup[1])) {
        (void)send((int)cfw_fd_get(&server->wakeup[1]), &byte, 1, 0);
    }
}

void cfw_server_free(cfw_server_t *server)
{
    if (server == NULL) {
        return;
    }
    cfw_server_stop(server);
    while (server->conns != NULL) {
        cfw_server_conn_t *conn = server->conns;
        server->conns = conn->next;
        cfw_server_close_conn(server, conn);
    }
    if (server->pollset != NULL) {
        cfw_pollset_destroy(server->pollset);
        server->pollset = NULL;
    }
    cfw_fd_close(&server->listener);
    cfw_fd_close(&server->wakeup[0]);
    cfw_fd_close(&server->wakeup[1]);
}

int cfw_server_is_stopping(const cfw_server_t *server)
{
    return server != NULL && server->stopping;
}

size_t cfw_server_connection_count(const cfw_server_t *server)
{
    return server != NULL ? server->conn_count : 0;
}

uint16_t cfw_server_port(const cfw_server_t *server)
{
    cfw_addr_t addr;
    cfw_fd_t listener;
    uint16_t port = 0;

    if (server == NULL || !cfw_fd_valid(&server->listener)) {
        return 0;
    }
    /* Copy the handle; cfw_net_local_addr does not take const fds. */
    listener = server->listener;
    if (cfw_net_local_addr(&listener, &addr) == CFW_OK) {
        cfw_addr_port(&addr, &port);
    }
    return port;
}

uint64_t cfw_server_accepted_total(const cfw_server_t *server)
{
    return server != NULL ? server->accepted_total : 0;
}

uint64_t cfw_server_requests_total(const cfw_server_t *server)
{
    return server != NULL ? server->requests_total : 0;
}

uint64_t cfw_server_errors_total(const cfw_server_t *server)
{
    return server != NULL ? server->errors_total : 0;
}

/* ------------------------------------------------------------------ */
/* Upgraded connections (ADR 0034)                                      */
/* ------------------------------------------------------------------ */

cfw_status_t cfw_upgraded_recv(cfw_upgraded_conn_t *conn, void *buffer,
                               size_t capacity, size_t *received)
{
    size_t done = 0;
    ssize_t n;

    if (conn == NULL || received == NULL ||
        (capacity != 0 && buffer == NULL) || !conn->upgraded ||
        conn->dead || !cfw_fd_valid(&conn->http.fd)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    *received = 0;
    if (capacity == 0) {
        return CFW_OK;
    }

    /* Staged bytes buffered past the request go first. */
    if (conn->upgrade_pending_pos < conn->upgrade_pending.size) {
        size_t avail =
            conn->upgrade_pending.size - conn->upgrade_pending_pos;
        size_t k = avail < capacity ? avail : capacity;
        memcpy(buffer,
               conn->upgrade_pending.data + conn->upgrade_pending_pos, k);
        conn->upgrade_pending_pos += k;
        done = k;
        if (done == capacity) {
            *received = done;
            return CFW_OK;
        }
    }
    if (conn->http.input_closed) {
        *received = done;
        return CFW_OK;
    }

    /* Route through the connection transport (ADR 0058) so upgraded
     * protocols such as WebSocket also work over TLS: the transport
     * returns the same outcomes as recv — bytes, EOF (done == 0),
     * WOULDBLOCK, or a fatal error. */
    if (conn->http.transport != NULL &&
        conn->http.transport->read != NULL) {
        size_t got = 0;
        cfw_status_t tst = conn->http.transport->read(
            conn->http.transport->user, (char *)buffer + done,
            capacity - done, &got);
        if (tst == CFW_OK) {
            if (got > 0) {
                *received = done + got;
                return CFW_OK;
            }
            conn->http.input_closed = 1;
            *received = done;
            return CFW_OK;
        }
        if (tst == CFW_ERROR_WOULDBLOCK) {
            if (done != 0) {
                *received = done;
                return CFW_OK;
            }
            return CFW_ERROR_WOULDBLOCK;
        }
        conn->dead = 1;
        return CFW_ERROR_IO;
    }

    n = recv((int)conn->http.fd.handle, (char *)buffer + done,
             capacity - done, 0);
    if (n > 0) {
        *received = done + (size_t)n;
        return CFW_OK;
    }
    if (n == 0) {
        conn->http.input_closed = 1;
        *received = done;
        return CFW_OK;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (done != 0) {
            *received = done;
            return CFW_OK;
        }
        return CFW_ERROR_WOULDBLOCK;
    }
    /* Socket error: the connection cannot continue; report it dead so
     * the event loop reaps it. The handle must not be used again. */
    conn->dead = 1;
    return CFW_ERROR_IO;
}

cfw_status_t cfw_upgraded_send(cfw_upgraded_conn_t *conn, const void *data,
                               size_t size)
{
    size_t pending;
    cfw_status_t st;

    if (conn == NULL || !conn->upgraded || conn->dead ||
        !cfw_fd_valid(&conn->http.fd)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (conn->upgrade_closing || conn->http.close_after_write) {
        return CFW_ERROR_IO;
    }
    if (size == 0) {
        return data == NULL ? CFW_ERROR_INVALID_ARGUMENT : CFW_OK;
    }
    if (data == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    /* All-or-nothing against the same bound HTTP responses use; the
     * app retries when CFW_UPGRADE_EV_WRITE is delivered. */
    pending = cfw_http1_conn_pending_out(&conn->http);
    if (size > conn->http.max_out_size ||
        pending > conn->http.max_out_size - size) {
        return CFW_ERROR_WOULDBLOCK;
    }
    st = cfw_http1_conn_queue(&conn->http, data, size);
    if (st != CFW_OK) {
        return st;
    }
    st = cfw_http1_conn_flush(&conn->http);
    if (st == CFW_ERROR_IO) {
        conn->dead = 1;
        return CFW_ERROR_IO;
    }
    return CFW_OK;
}

void cfw_upgraded_close(cfw_upgraded_conn_t *conn)
{
    if (conn == NULL || !conn->upgraded || conn->dead) {
        return;
    }
    conn->upgrade_closing = 1;
    cfw_http1_conn_close_after_write(&conn->http);
    if (cfw_http1_conn_pending_out(&conn->http) == 0) {
        conn->dead = 1;
    }
}

size_t cfw_upgraded_out_free(cfw_upgraded_conn_t *conn)
{
    size_t pending;

    if (conn == NULL || !conn->upgraded || conn->dead ||
        !cfw_fd_valid(&conn->http.fd)) {
        return 0;
    }
    pending = cfw_http1_conn_pending_out(&conn->http);
    if (pending >= conn->http.max_out_size) {
        return 0;
    }
    return conn->http.max_out_size - pending;
}

void *cfw_upgraded_user(cfw_upgraded_conn_t *conn)
{
    return conn != NULL ? conn->upgrade_user : NULL;
}

void cfw_upgraded_set_user(cfw_upgraded_conn_t *conn, void *user)
{
    if (conn != NULL) {
        conn->upgrade_user = user;
    }
}

#endif /* POSIX */
