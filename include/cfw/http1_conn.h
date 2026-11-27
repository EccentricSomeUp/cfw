/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_HTTP1_CONN_H
#define CFW_HTTP1_CONN_H

#include <cfw/buffer.h>
#include <cfw/fd.h>
#include <cfw/http1.h>
#include <cfw/status.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * HTTP/1.1 connection: binds an accepted nonblocking socket to the
 * cfw_http1_server_t message reader and an outbound response buffer.
 * POSIX transport only for now; on Windows all operations report
 * CFW_ERROR_UNAVAILABLE.
 *
 * Ownership: cfw_http1_conn_init takes ownership of *fd (the wrapper is
 * invalidated). cfw_http1_conn_free or cfw_http1_conn_close releases it.
 *
 * Readiness contract for use with cfw_pollset_wait:
 * - Poll CFW_POLL_IN while the connection is open and the request reader
 *   may need bytes.
 * - Poll CFW_POLL_OUT while cfw_http1_conn_pending_out() > 0.
 * Both may be set simultaneously; the caller may service either order.
 */

/* Optional transport override (ADR 0058): when set, socket reads and
 * writes go through these callbacks instead of raw recv/send on the
 * connection's fd. The fd remains the pollable descriptor and is
 * still owned and closed by the connection; the transport only
 * transforms the byte stream (e.g. TLS via cfw/tls.h). The transport
 * struct and its user pointer must outlive the connection. */
typedef struct cfw_http1_transport {
    void *user;
    /* Reads up to size bytes; CFW_OK with *done > 0 delivers bytes,
     * CFW_OK with *done == 0 signals orderly EOF, CFW_ERROR_WOULDBLOCK
     * means retry later, anything else is a fatal transport error. */
    cfw_status_t (*read)(void *user, void *buf, size_t size,
                         size_t *done);
    /* Writes up to size bytes; same contract as read but *done == 0
     * with CFW_OK is treated as a fatal error. */
    cfw_status_t (*write)(void *user, const void *data, size_t size,
                          size_t *done);
} cfw_http1_transport_t;

/* Options must be zero-initialized (memset) before use so that optional
 * fields such as transport default to off. */
typedef struct cfw_http1_conn_options {
    /* Request-reader limits (head/body/pending). */
    cfw_http1_server_options_t http;
    /* Maximum queued response bytes; must be > 0. queue() fails rather
     * than exceed it — flush first. */
    size_t max_out_size;
    /* Optional transport override; NULL uses raw socket I/O. */
    const cfw_http1_transport_t *transport;
} cfw_http1_conn_options_t;

typedef struct cfw_http1_conn {
    cfw_fd_t fd;
    cfw_http1_server_t server;
    cfw_buffer_t out;
    size_t out_pos;
    int close_after_write;
    int input_closed;
    int failed;
    size_t max_out_size;
    const cfw_http1_transport_t *transport;
} cfw_http1_conn_t;

/* Takes ownership of *fd, which must be a valid nonblocking socket.
 * On success *fd is invalidated. */
CFW_API cfw_status_t cfw_http1_conn_init(cfw_http1_conn_t *conn,
                                         cfw_fd_t *fd,
                                         const cfw_http1_conn_options_t *options);
CFW_API void cfw_http1_conn_free(cfw_http1_conn_t *conn);

/* Produces the next request event, reading from the socket as needed.
 * Same event contract as cfw_http1_server_next_event, plus:
 * - CFW_ERROR_WOULDBLOCK when no event is available and the socket would
 *   block (or input closed);
 * - CFW_ERROR_IO on socket errors (the connection is then failed). */
CFW_API cfw_status_t cfw_http1_conn_next_event(cfw_http1_conn_t *conn,
                                               cfw_http1_event_t *event);

/* Queues response bytes. Fails with CFW_ERROR_INVALID_ARGUMENT when the
 * queued total would exceed max_out_size — call cfw_http1_conn_flush
 * first. */
CFW_API cfw_status_t cfw_http1_conn_queue(cfw_http1_conn_t *conn,
                                          const void *data, size_t size);

/* Writes queued bytes without blocking. Returns CFW_OK when the queue is
 * empty, CFW_ERROR_WOULDBLOCK when bytes remain, CFW_ERROR_IO on socket
 * errors. When the queue drains on a connection marked
 * close-after-write, the socket is closed and CFW_OK is returned. */
CFW_API cfw_status_t cfw_http1_conn_flush(cfw_http1_conn_t *conn);

CFW_API size_t cfw_http1_conn_pending_out(const cfw_http1_conn_t *conn);
CFW_API int cfw_http1_conn_is_closed(const cfw_http1_conn_t *conn);

/* Marks the connection to close once the output queue drains. Called
 * automatically on MESSAGE_DONE for non-keep-alive requests and on
 * protocol errors. */
CFW_API void cfw_http1_conn_close_after_write(cfw_http1_conn_t *conn);

/* Closes the socket immediately, discarding queued output. */
CFW_API void cfw_http1_conn_close(cfw_http1_conn_t *conn);

#ifdef __cplusplus
}
#endif

#endif /* CFW_HTTP1_CONN_H */
