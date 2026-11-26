/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/server.h>

#if defined(_WIN32)

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

#else /* !_WIN32 */

#include "private/net.h"
#include "private/pollset.h"

#include <stdint.h>
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
    int have_head;
    int dead;
    cfw_timepoint_t deadline;
    int deadline_set;
    cfw_server_conn_t *next;
};

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

static void cfw_server_close_conn(cfw_server_t *server,
                                  cfw_server_conn_t *conn)
{
    cfw_pollset_remove(server->pollset, cfw_fd_get(&conn->http.fd));
    cfw_http1_conn_free(&conn->http);
    cfw_buffer_free(&conn->body);
    cfw_buffer_free(&conn->head_scratch);
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
    } else if (want) {
        cfw_pollset_modify(server->pollset, handle, CFW_POLL_IN, NULL);
    }
}

static void cfw_server_update_conn_interest(cfw_server_t *server,
                                            cfw_server_conn_t *conn)
{
    uint32_t events = 0;
    if (conn->dead) {
        return;
    }
    if (!conn->http.input_closed) {
        events |= CFW_POLL_IN;
    }
    if (cfw_http1_conn_pending_out(&conn->http) > 0) {
        events |= CFW_POLL_OUT;
    }
    if (events == 0) {
        conn->dead = 1;
        return;
    }
    cfw_pollset_modify(server->pollset, cfw_fd_get(&conn->http.fd), events,
                       conn);
}

static cfw_server_conn_t *cfw_server_find_conn(cfw_server_t *server,
                                               intptr_t handle)
{
    cfw_server_conn_t *conn;
    for (conn = server->conns; conn != NULL; conn = conn->next) {
        if (cfw_fd_get(&conn->http.fd) == handle) {
            return conn;
        }
    }
    return NULL;
}

static void cfw_server_send_error(cfw_server_t *server,
                                  cfw_server_conn_t *conn,
                                  cfw_http1_error_kind_t kind)
{
    cfw_http_status_t status = kind == CFW_HTTP1_ERR_BODY_TOO_LARGE
                                   ? CFW_HTTP_STATUS_PAYLOAD_TOO_LARGE
                                   : CFW_HTTP_STATUS_BAD_REQUEST;
    cfw_response_t res;
    cfw_request_t req_view;
    char head_buf[512];
    size_t written = 0;

    memset(&res, 0, sizeof(res));
    res.status = status;
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
    if (server->options.logger != NULL) {
        cfw_log(server->options.logger, CFW_LOG_WARN,
                "request error: %s", kind == CFW_HTTP1_ERR_BODY_TOO_LARGE
                                          ? "body too large"
                                          : "protocol error");
    }
    cfw_server_update_conn_interest(server, conn);
}

static void cfw_server_handle_done(cfw_server_t *server,
                                   cfw_server_conn_t *conn)
{
    cfw_span_t body;
    cfw_response_t res;
    cfw_request_t req_view;
    size_t written = 0;
    cfw_status_t st;

    server->requests_total++;
    conn->have_head = 0;

    memset(&body, 0, sizeof(body));
    body.data = conn->body.data;
    body.size = conn->body.size;

    memset(&res, 0, sizeof(res));
    res.headers = conn->headers;
    res.header_capacity = server->options.max_headers;

    st = cfw_dispatch_request(server->options.dispatch, &conn->head,
                              &body, conn->headers,
                              server->options.max_headers, conn->params,
                              server->options.max_params, &res);
    if (st != CFW_OK) {
        conn->dead = 1;
        return;
    }

    /* Signal close to the client when the connection will not be
     * reused; header_add failure (full scratch) is non-fatal. */
    if (conn->http.close_after_write) {
        cfw_strview_t hn, hv;
        cfw_strview_from_cstr(&hn, "Connection");
        cfw_strview_from_cstr(&hv, "close");
        cfw_response_header_add(&res, &hn, &hv);
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

    cfw_buffer_clear(&conn->body);

    /* Always re-arm: the request deadline must not fire while the
     * response drains. keep_alive_timeout doubles as the drain deadline
     * for closing connections (0 disables, matching keep-alive). */
    cfw_server_conn_deadline(conn, server->options.keep_alive_timeout);
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
            cfw_buffer_clear(&conn->body);
            cfw_server_conn_deadline(conn,
                                     server->options.request_timeout);
            break;
        case CFW_HTTP1_EV_BODY:
            if (cfw_buffer_append(&conn->body, event.body.data,
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
            cfw_server_send_error(server, conn, event.error_kind);
            break;
        }
        if (conn->dead) {
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
    if (events & (CFW_POLL_IN | CFW_POLL_ERR | CFW_POLL_HUP)) {
        if (!conn->http.input_closed) {
            cfw_server_service_read(server, conn);
        }
    }
    if (!conn->dead && (events & CFW_POLL_OUT)) {
        if (cfw_http1_conn_flush(&conn->http) == CFW_ERROR_IO) {
            conn->dead = 1;
        }
    }
    cfw_server_update_conn_interest(server, conn);
}

static void cfw_server_accept(cfw_server_t *server)
{
    cfw_http1_conn_options_t conn_options;

    conn_options.http = server->options.http;
    conn_options.max_out_size = server->options.max_out_size;

    for (;;) {
        cfw_fd_t fd;
        cfw_sockaddr_t peer;
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
        st = cfw_http1_conn_init(&conn->http, &fd, &conn_options);
        if (st != CFW_OK) {
            free(conn);
            continue;
        }
        conn->headers = malloc(server->options.max_headers *
                               sizeof(cfw_http_header_t));
        conn->params = malloc(server->options.max_params *
                              sizeof(cfw_route_param_t));
        if ((server->options.max_headers != 0 &&
             conn->headers == NULL) ||
            (server->options.max_params != 0 && conn->params == NULL) ||
            cfw_buffer_init(&conn->body) != CFW_OK ||
            cfw_buffer_init(&conn->head_scratch) != CFW_OK) {
            cfw_http1_conn_free(&conn->http);
            free(conn->headers);
            free(conn->params);
            free(conn);
            continue;
        }

        conn->next = server->conns;
        server->conns = conn;
        server->conn_count++;
        if (server->options.logger != NULL) {
            cfw_log(server->options.logger, CFW_LOG_DEBUG,
                    "connection accepted (%zu active)", server->conn_count);
        }
        cfw_pollset_add(server->pollset, cfw_fd_get(&conn->http.fd),
                        CFW_POLL_IN, conn);
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
            while (recv((int)handle, drain, sizeof(drain), MSG_DONTWAIT) >
                   0) {
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
    return CFW_OK;
}

cfw_status_t cfw_server_init(cfw_server_t *server,
                             const cfw_server_options_t *options)
{
    cfw_sockaddr_t addr;
    cfw_status_t st;
    int sv[2];

    if (server == NULL || options == NULL || options->dispatch == NULL ||
        options->max_connections == 0 || options->max_out_size == 0 ||
        options->http.max_head_size == 0 ||
        options->http.max_pending_size == 0 ||
        options->max_headers >
            SIZE_MAX / sizeof(cfw_http_header_t) ||
        options->max_params > SIZE_MAX / sizeof(cfw_route_param_t)) {
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

    /* Wake channel for prompt stop. */
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) != 0) {
        cfw_pollset_destroy(server->pollset);
        server->pollset = NULL;
        return CFW_ERROR_IO;
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
    if (options->host != NULL) {
        st = cfw_addr_from_str(&addr, options->host, options->port);
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
    cfw_sockaddr_t addr;
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

#endif /* !_WIN32 */
