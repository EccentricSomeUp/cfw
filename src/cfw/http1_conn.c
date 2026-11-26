/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "private/platform_detect.h"

#include <cfw/http1_conn.h>

#if CFW_PLATFORM_WINDOWS

cfw_status_t cfw_http1_conn_init(cfw_http1_conn_t *conn, cfw_fd_t *fd,
                                 const cfw_http1_conn_options_t *options)
{
    (void)conn;
    (void)fd;
    (void)options;
    return CFW_ERROR_UNAVAILABLE;
}

void cfw_http1_conn_free(cfw_http1_conn_t *conn)
{
    (void)conn;
}

cfw_status_t cfw_http1_conn_next_event(cfw_http1_conn_t *conn,
                                       cfw_http1_event_t *event)
{
    (void)conn;
    (void)event;
    return CFW_ERROR_UNAVAILABLE;
}

cfw_status_t cfw_http1_conn_queue(cfw_http1_conn_t *conn, const void *data,
                                  size_t size)
{
    (void)conn;
    (void)data;
    (void)size;
    return CFW_ERROR_UNAVAILABLE;
}

cfw_status_t cfw_http1_conn_flush(cfw_http1_conn_t *conn)
{
    (void)conn;
    return CFW_ERROR_UNAVAILABLE;
}

size_t cfw_http1_conn_pending_out(const cfw_http1_conn_t *conn)
{
    (void)conn;
    return 0;
}

int cfw_http1_conn_is_closed(const cfw_http1_conn_t *conn)
{
    (void)conn;
    return 1;
}

void cfw_http1_conn_close_after_write(cfw_http1_conn_t *conn)
{
    (void)conn;
}

void cfw_http1_conn_close(cfw_http1_conn_t *conn)
{
    (void)conn;
}

#else /* POSIX */

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CFW_HTTP1_CONN_READ_SIZE 16384u

static void cfw_http1_conn_do_close(cfw_http1_conn_t *conn)
{
    cfw_fd_close(&conn->fd);
    conn->out_pos = 0;
    conn->input_closed = 1;
}

cfw_status_t cfw_http1_conn_init(cfw_http1_conn_t *conn, cfw_fd_t *fd,
                                 const cfw_http1_conn_options_t *options)
{
    cfw_status_t status;

    if (conn == NULL || fd == NULL || options == NULL ||
        options->max_out_size == 0 || !cfw_fd_valid(fd)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    memset(conn, 0, sizeof(*conn));
    cfw_fd_init(&conn->fd);

    status = cfw_http1_server_init(&conn->server, &options->http);
    if (status != CFW_OK) {
        return status;
    }
    status = cfw_buffer_init(&conn->out);
    if (status != CFW_OK) {
        cfw_http1_server_free(&conn->server);
        return status;
    }

    conn->fd.handle = cfw_fd_release(fd);
    conn->max_out_size = options->max_out_size;
    return CFW_OK;
}

void cfw_http1_conn_free(cfw_http1_conn_t *conn)
{
    if (conn == NULL) {
        return;
    }
    cfw_fd_close(&conn->fd);
    cfw_buffer_free(&conn->out);
    cfw_http1_server_free(&conn->server);
    memset(conn, 0, sizeof(*conn));
    cfw_fd_init(&conn->fd);
}

cfw_status_t cfw_http1_conn_next_event(cfw_http1_conn_t *conn,
                                       cfw_http1_event_t *event)
{
    char buffer[CFW_HTTP1_CONN_READ_SIZE];
    cfw_status_t status;

    if (conn == NULL || event == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (!cfw_fd_valid(&conn->fd) || conn->failed) {
        return CFW_ERROR_WOULDBLOCK;
    }

    for (;;) {
        status = cfw_http1_server_next_event(&conn->server, event);
        if (status == CFW_OK) {
            if (event->kind == CFW_HTTP1_EV_MESSAGE_DONE &&
                !conn->server.keep_alive) {
                conn->close_after_write = 1;
            }
            if (event->kind == CFW_HTTP1_EV_ERROR) {
                conn->close_after_write = 1;
            }
            return CFW_OK;
        }
        if (status != CFW_ERROR_WOULDBLOCK) {
            return status;
        }
        if (conn->input_closed) {
            return CFW_ERROR_WOULDBLOCK;
        }

        {
            ssize_t n = recv((int)conn->fd.handle, buffer, sizeof(buffer), 0);
            cfw_span_t span;
            if (n > 0) {
                cfw_span_init(&span, buffer, (size_t)n);
                status = cfw_http1_server_feed(&conn->server, &span);
                if (status != CFW_OK) {
                    conn->failed = 1;
                    conn->close_after_write = 1;
                    return status;
                }
                continue;
            }
            if (n == 0) {
                conn->input_closed = 1;
                if (conn->close_after_write &&
                    cfw_http1_conn_pending_out(conn) == 0) {
                    cfw_http1_conn_do_close(conn);
                }
                return CFW_ERROR_WOULDBLOCK;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return CFW_ERROR_WOULDBLOCK;
            }
            conn->failed = 1;
            return CFW_ERROR_IO;
        }
    }
}

cfw_status_t cfw_http1_conn_queue(cfw_http1_conn_t *conn, const void *data,
                                  size_t size)
{
    size_t logical;
    cfw_status_t status;

    if (conn == NULL || (size != 0 && data == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (!cfw_fd_valid(&conn->fd)) {
        return CFW_ERROR_WOULDBLOCK;
    }
    if (size == 0) {
        return CFW_OK;
    }

    logical = conn->out.size - conn->out_pos;
    if (size > conn->max_out_size ||
        logical > conn->max_out_size - size) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (conn->out_pos != 0) {
        memmove(conn->out.data, conn->out.data + conn->out_pos, logical);
        conn->out.size = logical;
        conn->out_pos = 0;
    }
    status = cfw_buffer_append(&conn->out, data, size);
    if (status != CFW_OK) {
        return status;
    }
    return CFW_OK;
}

cfw_status_t cfw_http1_conn_flush(cfw_http1_conn_t *conn)
{
    if (conn == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (!cfw_fd_valid(&conn->fd)) {
        return CFW_OK;
    }

    while (conn->out_pos < conn->out.size) {
        ssize_t n;
#if CFW_HAVE_MSG_NOSIGNAL
        n = send((int)conn->fd.handle, conn->out.data + conn->out_pos,
                 conn->out.size - conn->out_pos, MSG_NOSIGNAL);
#else
        n = send((int)conn->fd.handle, conn->out.data + conn->out_pos,
                 conn->out.size - conn->out_pos, 0);
#endif
        if (n > 0) {
            conn->out_pos += (size_t)n;
            continue;
        }
        if (n == 0) {
            return CFW_ERROR_IO;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return CFW_ERROR_WOULDBLOCK;
        }
        return CFW_ERROR_IO;
    }

    conn->out.size = 0;
    conn->out_pos = 0;
    if (conn->close_after_write) {
        cfw_http1_conn_do_close(conn);
    }
    return CFW_OK;
}

size_t cfw_http1_conn_pending_out(const cfw_http1_conn_t *conn)
{
    if (conn == NULL) {
        return 0;
    }
    return conn->out.size - conn->out_pos;
}

int cfw_http1_conn_is_closed(const cfw_http1_conn_t *conn)
{
    return conn == NULL || !cfw_fd_valid(&conn->fd);
}

void cfw_http1_conn_close_after_write(cfw_http1_conn_t *conn)
{
    if (conn != NULL) {
        conn->close_after_write = 1;
    }
}

void cfw_http1_conn_close(cfw_http1_conn_t *conn)
{
    if (conn == NULL) {
        return;
    }
    cfw_http1_conn_do_close(conn);
}

#endif /* POSIX */
