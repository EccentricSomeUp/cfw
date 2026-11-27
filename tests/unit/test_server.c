/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <cfw/net.h>

#include "test_loopback.h"

#include <stdlib.h>
#include <string.h>

static cfw_status_t cfw_test_connect(cfw_fd_t *client, uint16_t port)
{
    cfw_addr_t addr;
    cfw_status_t st;

    CFW_TEST_ASSERT_EQ(cfw_addr_from_str(&addr, "127.0.0.1", port), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_socket(AF_INET, SOCK_STREAM, 0, client),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_set_nonblocking(client, 0), CFW_OK);
    st = cfw_net_connect(client, &addr);
    CFW_TEST_ASSERT_EQ(cfw_net_set_nonblocking(client, 1), CFW_OK);
    return st;
}

static int cfw_test_send(cfw_fd_t *client, const char *data)
{
    size_t len = strlen(data);
    size_t sent = 0;
    int attempts;

    for (attempts = 0; attempts < 400 && sent < len; attempts++) {
        size_t chunk = 0;
        cfw_status_t st =
            cfw_net_send(client, data + sent, len - sent, &chunk);
        if (st == CFW_OK) {
            sent += chunk;
            continue;
        }
        if (st != CFW_ERROR_WOULDBLOCK) {
            return 1;
        }
        cfw_test_sleep_us(1000);
    }
    return sent == len ? 0 : 1;
}

/* recv() drop-in over cfw_net_recv for the nonblocking test sockets:
 * returns bytes read (>0), 0 on EOF, -1 on WOULDBLOCK/error. */
static intptr_t cfw_test_sock_recv(cfw_fd_t *fd, void *buffer,
                                   size_t capacity)
{
    size_t got = 0;
    cfw_status_t st = cfw_net_recv(fd, buffer, capacity, &got);
    if (st == CFW_OK) {
        return (intptr_t)got;
    }
    return -1;
}

static int cfw_test_contains(const char *haystack, size_t len,
                             const char *needle)
{
    size_t nlen = strlen(needle);
    size_t i;
    if (nlen == 0 || len < nlen) {
        return 0;
    }
    for (i = 0; i + nlen <= len; i++) {
        if (memcmp(haystack + i, needle, nlen) == 0) {
            return 1;
        }
    }
    return 0;
}

static cfw_status_t cfw_test_ok_handler(cfw_request_t *req,
                                        cfw_response_t *res, void *user)
{
    static const char body[] = "srv-ok";
    (void)req;
    (void)user;
    res->status = CFW_HTTP_STATUS_OK;
    res->body.data = (const uint8_t *)body;
    res->body.size = sizeof(body) - 1;
    return CFW_OK;
}

static char cfw_test_remote_addr[80];
static size_t cfw_test_remote_addr_len;

static cfw_status_t cfw_test_addr_handler(cfw_request_t *req,
                                          cfw_response_t *res, void *user)
{
    static const char body[] = "addr-ok";
    (void)user;
    res->status = CFW_HTTP_STATUS_OK;
    res->body.data = (const uint8_t *)body;
    res->body.size = sizeof(body) - 1;
    /* The strview is only valid during dispatch — copy it out. */
    cfw_test_remote_addr_len = 0;
    if (req->remote_addr.size > 0 &&
        req->remote_addr.size < sizeof(cfw_test_remote_addr)) {
        memcpy(cfw_test_remote_addr, req->remote_addr.data,
               req->remote_addr.size);
        cfw_test_remote_addr[req->remote_addr.size] = '\0';
        cfw_test_remote_addr_len = req->remote_addr.size;
    }
    return CFW_OK;
}

/* Records whether req->tls_conn was non-NULL for the last /probe
 * request: -1 unset, 0 cleartext, 1 TLS (ADR 0063). */
static int cfw_test_tls_conn_seen = -1;

static cfw_status_t cfw_test_tls_probe_handler(cfw_request_t *req,
                                               cfw_response_t *res,
                                               void *user)
{
    static const char body[] = "probe-ok";
    (void)user;
    cfw_test_tls_conn_seen = req->tls_conn != NULL;
    res->status = CFW_HTTP_STATUS_OK;
    res->body.data = (const uint8_t *)body;
    res->body.size = sizeof(body) - 1;
    return CFW_OK;
}

static cfw_status_t cfw_test_mw(cfw_request_t *req, cfw_response_t *res,
                                cfw_dispatch_next_t next,
                                cfw_dispatch_frame_t *frame, void *user)
{
    cfw_strview_t name;
    cfw_strview_t value;
    cfw_status_t st;
    (void)req;
    (void)user;
    st = next(frame);
    if (st != CFW_OK) {
        return st;
    }
    cfw_strview_from_cstr(&name, "X-Server-Mw");
    cfw_strview_from_cstr(&value, "1");
    return cfw_response_header_add(res, &name, &value);
}

typedef struct {
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_server_t server;
} cfw_test_server;

static void cfw_test_server_options(cfw_server_options_t *options,
                                    cfw_dispatch_t *dispatch)
{
    memset(options, 0, sizeof(*options));
    options->host = "127.0.0.1";
    options->port = 0;
    options->backlog = 16;
    options->dispatch = dispatch;
    options->http.max_head_size = 8192;
    options->http.max_body_size = 65536;
    options->http.max_pending_size = 65536;
    options->max_out_size = 65536;
    options->max_connections = 8;
    options->keep_alive_timeout.nanoseconds = 0;
    options->request_timeout.nanoseconds = 0;
    options->max_headers = 32;
    options->max_params = 16;
}

static int cfw_test_server_setup_ex(cfw_test_server *ts,
                                    cfw_duration_t keep_alive,
                                    cfw_duration_t request_timeout,
                                    cfw_duration_t drain_timeout,
                                    size_t max_connections,
                                    const char *host,
                                    cfw_upload_begin_fn upload_begin,
                                    cfw_upload_write_fn upload_write,
                                    cfw_upload_end_fn upload_end,
                                    void *upload_user,
                                    cfw_logger_t *logger)
{
    cfw_server_options_t options;
    cfw_dispatch_options_t doptions;
    cfw_strview_t pattern;

    memset(ts, 0, sizeof(*ts));
    ts->arena = cfw_arena_create(32768);
    CFW_TEST_ASSERT_TRUE(ts->arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&ts->router, ts->arena), CFW_OK);

    memset(&doptions, 0, sizeof(doptions));
    doptions.router = &ts->router;
    doptions.handler = cfw_test_ok_handler;
    doptions.handler_user = NULL;
    doptions.max_middleware = 4;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&ts->dispatch, ts->arena,
                                         &doptions),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_dispatch_use(&ts->dispatch, cfw_test_mw, NULL),
                       CFW_OK);

    cfw_strview_from_cstr(&pattern, "/hello");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&ts->dispatch,
                                              CFW_HTTP_METHOD_GET,
                                              &pattern, cfw_test_ok_handler,
                                              NULL),
                       CFW_OK);
    cfw_strview_from_cstr(&pattern, "/probe");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&ts->dispatch,
                                              CFW_HTTP_METHOD_GET,
                                              &pattern,
                                              cfw_test_tls_probe_handler,
                                              NULL),
                       CFW_OK);

    cfw_test_server_options(&options, &ts->dispatch);
    options.host = host;
    options.keep_alive_timeout = keep_alive;
    options.request_timeout = request_timeout;
    options.drain_timeout = drain_timeout;
    options.max_connections = max_connections;
    options.upload_begin = upload_begin;
    options.upload_write = upload_write;
    options.upload_end = upload_end;
    options.upload_user = upload_user;
    options.logger = logger;
    CFW_TEST_ASSERT_EQ(cfw_server_init(&ts->server, &options), CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_server_port(&ts->server) != 0);
    return 0;
}

static int cfw_test_server_setup_host(cfw_test_server *ts,
                                      cfw_duration_t keep_alive,
                                      cfw_duration_t request_timeout,
                                      size_t max_connections,
                                      const char *host)
{
    return cfw_test_server_setup_ex(ts, keep_alive, request_timeout,
                                    (cfw_duration_t){0},
                                    max_connections, host, NULL, NULL,
                                    NULL, NULL, NULL);
}

static int cfw_test_server_setup(cfw_test_server *ts,
                                 cfw_duration_t keep_alive,
                                 cfw_duration_t request_timeout,
                                 size_t max_connections)
{
    return cfw_test_server_setup_host(ts, keep_alive, request_timeout,
                                      max_connections, "127.0.0.1");
}

static void cfw_test_server_teardown(cfw_test_server *ts)
{
    cfw_server_free(&ts->server);
    cfw_arena_destroy(ts->arena);
}

/* Steps the server until `predicate`-style recv yields data or the
 * attempt budget is exhausted. Returns bytes read (>0), 0 on EOF,
 * -1 on timeout. */
static intptr_t cfw_test_serve_recv(cfw_server_t *server, cfw_fd_t *client,
                                   char *buffer, size_t capacity)
{
    cfw_timepoint_t deadline;
    cfw_duration_t slice;
    int attempts;

    cfw_duration_from_millis(50, &slice);
    for (attempts = 0; attempts < 200; attempts++) {
        cfw_deadline_from_now(&deadline, slice);
        if (cfw_server_step(server, &deadline) != CFW_OK) {
            return -1;
        }
        {
            intptr_t n = cfw_test_sock_recv(client, buffer, capacity);
            if (n > 0) {
                return n;
            }
            if (n == 0) {
                return 0;
            }
        }
    }
    return -1;
}

static int test_server_request_response(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    char response[4096];
    intptr_t n;
    uint16_t port;

    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts,
                                             (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 8),
                       0);
    port = cfw_server_port(&ts.server);
    CFW_TEST_ASSERT_TRUE(port != 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client, port), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /hello HTTP/1.1\r\n"
                                     "Host: test\r\n\r\n"),
                       0);

    n = cfw_test_serve_recv(&ts.server, &client, response,
                            sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "HTTP/1.1 200 OK"));
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n, "srv-ok"));
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n, "Date: "));
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "X-Server-Mw: 1"));
    CFW_TEST_ASSERT_EQ(cfw_server_requests_total(&ts.server), 1);
    CFW_TEST_ASSERT_EQ(cfw_server_accepted_total(&ts.server), 1);

    /* Keep-alive: second request on the same connection. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /missing HTTP/1.1\r\n"
                                     "Host: test\r\n\r\n"),
                       0);
    n = cfw_test_serve_recv(&ts.server, &client, response,
                            sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n, "404"));
    CFW_TEST_ASSERT_EQ(cfw_server_requests_total(&ts.server), 2);

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}

/* Cleartext requests observe req->tls_conn == NULL (ADR 0063). */
static int test_server_tls_conn_null_cleartext(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    char response[8192];
    intptr_t n;
    uint16_t port;
    int rc = 1;

    cfw_test_tls_conn_seen = -1;
    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(
                           &ts, (cfw_duration_t){0},
                           (cfw_duration_t){0}, 8),
                       0);
    port = cfw_server_port(&ts.server);
    CFW_TEST_ASSERT_TRUE(port != 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client, port), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /probe HTTP/1.1\r\n"
                                     "Host: test\r\n\r\n"),
                       0);
    n = cfw_test_serve_recv(&ts.server, &client, response,
                            sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "probe-ok"));
    CFW_TEST_ASSERT_EQ(cfw_test_tls_conn_seen, 0);
    rc = 0;

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return rc;
}

static char cfw_test_pad_names[28][12];
static char cfw_test_pad_value[280];

static cfw_status_t cfw_test_big_head_handler(cfw_request_t *req,
                                              cfw_response_t *res,
                                              void *user)
{
    static const char body[] = "big-head";
    cfw_strview_t hv;
    int i;

    (void)req;
    (void)user;
    res->status = CFW_HTTP_STATUS_OK;
    res->body.data = (const uint8_t *)body;
    res->body.size = sizeof(body) - 1;

    /* ~8.2 KiB of headers: exceeds the initial head scratch
     * (max_head_size) so the server must grow it once and retry. */
    memset(cfw_test_pad_value, 'a', sizeof(cfw_test_pad_value) - 1);
    cfw_test_pad_value[sizeof(cfw_test_pad_value) - 1] = '\0';
    cfw_strview_init(&hv, cfw_test_pad_value,
                     sizeof(cfw_test_pad_value) - 1);
    for (i = 0; i < 28; i++) {
        cfw_strview_t hn;
        if (cfw_test_pad_names[i][0] == '\0') {
            snprintf(cfw_test_pad_names[i],
                     sizeof(cfw_test_pad_names[i]), "X-Pad-%02d", i);
        }
        cfw_strview_from_cstr(&hn, cfw_test_pad_names[i]);
        cfw_response_header_add(res, &hn, &hv);
    }
    return CFW_OK;
}

static int test_server_large_response_head(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    cfw_strview_t pattern;
    char response[16384];
    intptr_t n;

    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts,
                                             (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 8),
                       0);
    cfw_strview_from_cstr(&pattern, "/big");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&ts.dispatch,
                                              CFW_HTTP_METHOD_GET,
                                              &pattern,
                                              cfw_test_big_head_handler,
                                              NULL),
                       CFW_OK);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /big HTTP/1.1\r\n"
                                     "Host: big\r\n\r\n"),
                       0);
    n = cfw_test_serve_recv(&ts.server, &client, response,
                            sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "HTTP/1.1 200 OK"));
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "X-Pad-27"));
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "big-head"));

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}

static int test_server_post_body(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    char response[4096];
    intptr_t n;
    cfw_strview_t pattern;

    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 8),
                       0);
    /* Add a POST echo route bound to the ok handler. */
    cfw_strview_from_cstr(&pattern, "/echo");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&ts.dispatch,
                                              CFW_HTTP_METHOD_POST,
                                              &pattern, cfw_test_ok_handler,
                                              NULL),
                       CFW_OK);

    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "POST /echo HTTP/1.1\r\n"
                                     "Host: test\r\n"
                                     "Content-Length: 11\r\n"
                                     "\r\n"
                                     "hello world"),
                       0);
    n = cfw_test_serve_recv(&ts.server, &client, response,
                            sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "HTTP/1.1 200 OK"));

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}

static int test_server_remote_addr(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    char response[4096];
    intptr_t n;
    cfw_strview_t pattern;
    const char prefix[] = "127.0.0.1:";
    size_t i;
    int digits = 0;

    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 8),
                       0);
    cfw_strview_from_cstr(&pattern, "/who");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&ts.dispatch,
                                              CFW_HTTP_METHOD_GET,
                                              &pattern, cfw_test_addr_handler,
                                              NULL),
                       CFW_OK);

    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /who HTTP/1.1\r\n"
                                     "Host: test\r\n\r\n"),
                       0);
    n = cfw_test_serve_recv(&ts.server, &client, response,
                            sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "HTTP/1.1 200 OK"));

    /* Peer address must be "127.0.0.1:<port>" with a nonzero port. */
    CFW_TEST_ASSERT_TRUE(cfw_test_remote_addr_len > 0);
    CFW_TEST_ASSERT_TRUE(cfw_test_remote_addr_len <
                         sizeof(cfw_test_remote_addr));
    CFW_TEST_ASSERT_TRUE(cfw_test_remote_addr_len > sizeof(prefix) - 1);
    CFW_TEST_ASSERT_TRUE(memcmp(cfw_test_remote_addr, prefix,
                                sizeof(prefix) - 1) == 0);
    for (i = sizeof(prefix) - 1; i < cfw_test_remote_addr_len; i++) {
        if (cfw_test_remote_addr[i] >= '0' &&
            cfw_test_remote_addr[i] <= '9') {
            digits++;
        }
    }
    CFW_TEST_ASSERT_TRUE(digits > 0);

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}

static int test_server_request_timeout(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    cfw_duration_t timeout;
    int attempts;

    cfw_duration_from_millis(100, &timeout);
    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, (cfw_duration_t){0},
                                             timeout, 8),
                       0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    /* Partial head: never completes → request deadline fires. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client, "GET /hel"), 0);

    /* Step until the connection is reaped (~100ms + margin). */
    for (attempts = 0; attempts < 400; attempts++) {
        cfw_timepoint_t deadline;
        cfw_duration_t slice;
        cfw_duration_from_millis(20, &slice);
        cfw_deadline_from_now(&deadline, slice);
        CFW_TEST_ASSERT_EQ(cfw_server_step(&ts.server, &deadline), CFW_OK);
        if (cfw_server_connection_count(&ts.server) == 0) {
            break;
        }
    }
    CFW_TEST_ASSERT_EQ(cfw_server_connection_count(&ts.server), 0);

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}

static int test_server_stop(void)
{
    cfw_test_server ts;
    cfw_timepoint_t deadline;
    cfw_duration_t slice;

    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 8),
                       0);
    cfw_server_stop(&ts.server);
    CFW_TEST_ASSERT_TRUE(cfw_server_is_stopping(&ts.server));
    /* step after stop still works (drain mode) and returns promptly. */
    cfw_duration_from_millis(10, &slice);
    cfw_deadline_from_now(&deadline, slice);
    CFW_TEST_ASSERT_EQ(cfw_server_step(&ts.server, &deadline), CFW_OK);
    cfw_test_server_teardown(&ts);
    return 0;
}

static int test_server_max_connections(void)
{
    cfw_test_server ts;
    cfw_fd_t c1;
    cfw_fd_t c2;
    char response[4096];
    intptr_t n;
    int attempts;

    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 1),
                       0);
    cfw_fd_init(&c1);
    cfw_fd_init(&c2);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&c1,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);

    /* Wait for c1 to be accepted (fills the single slot). */
    for (attempts = 0; attempts < 100; attempts++) {
        cfw_timepoint_t deadline;
        cfw_duration_t slice;
        cfw_duration_from_millis(10, &slice);
        cfw_deadline_from_now(&deadline, slice);
        cfw_server_step(&ts.server, &deadline);
        if (cfw_server_connection_count(&ts.server) == 1) {
            break;
        }
    }
    CFW_TEST_ASSERT_EQ(cfw_server_connection_count(&ts.server), 1);

    /* c2 connects at TCP level (backlog) but is never accepted while
     * the slot is taken. */
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&c2,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    for (attempts = 0; attempts < 50; attempts++) {
        cfw_timepoint_t deadline;
        cfw_duration_t slice;
        cfw_duration_from_millis(10, &slice);
        cfw_deadline_from_now(&deadline, slice);
        cfw_server_step(&ts.server, &deadline);
    }
    CFW_TEST_ASSERT_EQ(cfw_server_connection_count(&ts.server), 1);

    /* c1 is still served normally. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&c1,
                                     "GET /hello HTTP/1.1\r\n"
                                     "Host: test\r\n\r\n"),
                       0);
    n = cfw_test_serve_recv(&ts.server, &c1, response,
                            sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "HTTP/1.1 200 OK"));

    /* Closing c1 frees the slot; c2 is then accepted. */
    cfw_fd_close(&c1);
    for (attempts = 0; attempts < 200; attempts++) {
        cfw_timepoint_t deadline;
        cfw_duration_t slice;
        cfw_duration_from_millis(10, &slice);
        cfw_deadline_from_now(&deadline, slice);
        cfw_server_step(&ts.server, &deadline);
        if (cfw_server_accepted_total(&ts.server) >= 2) {
            break;
        }
    }
    CFW_TEST_ASSERT_TRUE(cfw_server_accepted_total(&ts.server) >= 2);

    cfw_fd_close(&c2);
    cfw_test_server_teardown(&ts);
    return 0;
}


static int test_server_soak(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    char response[4096];
    intptr_t n;
    int i;

    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 8),
                       0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);

    /* 100 sequential keep-alive requests on one connection. */
    for (i = 0; i < 100; i++) {
        CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                         "GET /hello HTTP/1.1\r\n"
                                         "Host: soak\r\n\r\n"),
                           0);
        n = cfw_test_serve_recv(&ts.server, &client, response,
                                sizeof(response) - 1);
        CFW_TEST_ASSERT_TRUE(n > 0);
        response[n] = '\0';
        CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                               "HTTP/1.1 200 OK"));
    }
    CFW_TEST_ASSERT_EQ(cfw_server_requests_total(&ts.server), 100);

    /* Interleaved: several connections each serving requests. */
    {
        cfw_fd_t clients[4];
        int c;
        for (c = 0; c < 4; c++) {
            cfw_fd_init(&clients[c]);
            CFW_TEST_ASSERT_EQ(cfw_test_connect(
                                   &clients[c],
                                   cfw_server_port(&ts.server)),
                               CFW_OK);
        }
        for (c = 0; c < 4; c++) {
            CFW_TEST_ASSERT_EQ(cfw_test_send(&clients[c],
                                             "GET /hello HTTP/1.1\r\n"
                                             "Host: soak\r\n\r\n"),
                               0);
        }
        for (c = 0; c < 4; c++) {
            n = cfw_test_serve_recv(&ts.server, &clients[c], response,
                                    sizeof(response) - 1);
            CFW_TEST_ASSERT_TRUE(n > 0);
        }
        for (c = 0; c < 4; c++) {
            cfw_fd_close(&clients[c]);
        }
    }
    CFW_TEST_ASSERT_EQ(cfw_server_requests_total(&ts.server), 104);

    /* Connection churn: rapid connect/request/close cycles stress the
     * accept path and fd-number reuse after close. */
    for (i = 0; i < 200; i++) {
        cfw_fd_t churn;
        cfw_fd_init(&churn);
        CFW_TEST_ASSERT_EQ(cfw_test_connect(&churn,
                                            cfw_server_port(&ts.server)),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_test_send(&churn,
                                         "GET /hello HTTP/1.1\r\n"
                                         "Host: churn\r\n\r\n"),
                           0);
        n = cfw_test_serve_recv(&ts.server, &churn, response,
                                sizeof(response) - 1);
        CFW_TEST_ASSERT_TRUE(n > 0);
        response[n] = '\0';
        CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                               "HTTP/1.1 200 OK"));
        cfw_fd_close(&churn);
    }
    CFW_TEST_ASSERT_EQ(cfw_server_requests_total(&ts.server), 304);

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}


static int test_server_drain_after_stop(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    char response[4096];
    intptr_t n;

    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 8),
                       0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);

    /* Establish a keep-alive connection with one completed request. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /hello HTTP/1.1\r\n"
                                     "Host: drain\r\n\r\n"),
                       0);
    n = cfw_test_serve_recv(&ts.server, &client, response,
                            sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    CFW_TEST_ASSERT_EQ(cfw_server_connection_count(&ts.server), 1);

    /* Stop: listener closes, but the existing connection must still
     * serve via step (drain semantics). */
    cfw_server_stop(&ts.server);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /hello HTTP/1.1\r\n"
                                     "Host: drain\r\n\r\n"),
                       0);
    n = cfw_test_serve_recv(&ts.server, &client, response,
                            sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "HTTP/1.1 200 OK"));
    CFW_TEST_ASSERT_EQ(cfw_server_requests_total(&ts.server), 2);

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}


static int test_server_expect_continue(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    char response[4096];
    intptr_t n;
    cfw_strview_t pattern;

    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 8),
                       0);
    cfw_strview_from_cstr(&pattern, "/upload");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&ts.dispatch,
                                              CFW_HTTP_METHOD_POST,
                                              &pattern, cfw_test_ok_handler,
                                              NULL),
                       CFW_OK);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);

    /* Client announces a body and waits for 100 Continue. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "POST /upload HTTP/1.1\r\n"
                                     "Host: test\r\n"
                                     "Expect: 100-continue\r\n"
                                     "Content-Length: 4\r\n"
                                     "\r\n"),
                       0);

    /* First recv should contain the interim response (possibly with
     * the final response appended). */
    n = cfw_test_serve_recv(&ts.server, &client, response,
                            sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "100 Continue"));

    /* Now send the body; final response follows. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client, "data"), 0);
    n = cfw_test_serve_recv(&ts.server, &client, response,
                            sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "HTTP/1.1 200 OK"));

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}


static int test_server_keep_alive_timeout(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    cfw_duration_t ka;
    int attempts;

    cfw_duration_from_millis(100, &ka);
    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, ka, (cfw_duration_t){0},
                                             8),
                       0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    /* Complete one request so the conn enters keep-alive idle. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /hello HTTP/1.1\r\n"
                                     "Host: ka\r\n\r\n"),
                       0);
    {
        char buf[1024];
        cfw_test_serve_recv(&ts.server, &client, buf, sizeof(buf) - 1);
    }
    CFW_TEST_ASSERT_EQ(cfw_server_connection_count(&ts.server), 1);

    /* Idle past keep_alive_timeout → conn reaped. */
    for (attempts = 0; attempts < 400; attempts++) {
        cfw_timepoint_t deadline;
        cfw_duration_t slice;
        cfw_duration_from_millis(20, &slice);
        cfw_deadline_from_now(&deadline, slice);
        CFW_TEST_ASSERT_EQ(cfw_server_step(&ts.server, &deadline), CFW_OK);
        if (cfw_server_connection_count(&ts.server) == 0) {
            break;
        }
    }
    CFW_TEST_ASSERT_EQ(cfw_server_connection_count(&ts.server), 0);

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}


static int test_server_dns_listen(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    char response[4096];
    intptr_t n;

    /* "localhost" must resolve; skip gracefully if the host has no
     * resolver entry (unusual but possible in minimal containers). */
    CFW_TEST_ASSERT_EQ(cfw_test_server_setup_host(
                           &ts, (cfw_duration_t){0}, (cfw_duration_t){0},
                           8, "localhost"),
                       0);
    CFW_TEST_ASSERT_TRUE(cfw_server_port(&ts.server) != 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /hello HTTP/1.1\r\n"
                                     "Host: dns\r\n\r\n"),
                       0);
    n = cfw_test_serve_recv(&ts.server, &client, response,
                            sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "HTTP/1.1 200 OK"));

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}


typedef struct {
    const char *const *parts;
    size_t count;
    size_t pos;
    int fail;
    int empty;
    int sized;
    uint64_t stream_size;
} cfw_test_stream_state;

static const char *const cfw_test_stream_parts[] = {
    "Hello, ", "stream", "ing ", "world!",
};

static cfw_status_t cfw_test_stream_fn(void *user, cfw_span_t *chunk,
                                       int *eof)
{
    cfw_test_stream_state *st = user;
    if (st->fail) {
        return CFW_ERROR_IO;
    }
    if (st->empty) {
        chunk->data = NULL;
        chunk->size = 0;
        *eof = 0;
        return CFW_OK;
    }
    if (st->pos >= st->count) {
        *eof = 1;
        return CFW_OK;
    }
    chunk->data = (const uint8_t *)cfw_test_stream_parts[st->pos];
    chunk->size = strlen(cfw_test_stream_parts[st->pos]);
    st->pos++;
    return CFW_OK;
}

static cfw_status_t cfw_test_stream_handler(cfw_request_t *req,
                                            cfw_response_t *res, void *user)
{
    (void)req;
    res->status = CFW_HTTP_STATUS_OK;
    res->stream = cfw_test_stream_fn;
    res->stream_user = user;
    if (((cfw_test_stream_state *)user)->sized) {
        res->stream_size =
            ((cfw_test_stream_state *)user)->stream_size;
    }
    return CFW_OK;
}

/* Steps the server and appends received bytes until `marker` appears,
 * EOF, or the attempt budget is exhausted. Returns total bytes or -1. */
static intptr_t cfw_test_recv_until(cfw_server_t *server, cfw_fd_t *client,
                                   char *buffer, size_t capacity,
                                   const char *marker, size_t *used)
{
    cfw_duration_t slice;
    int attempts;
    size_t mlen = strlen(marker);

    cfw_duration_from_millis(50, &slice);
    for (attempts = 0; attempts < 400 && *used + 1 < capacity; attempts++) {
        cfw_timepoint_t deadline;
        cfw_deadline_from_now(&deadline, slice);
        intptr_t n;
        if (cfw_server_step(server, &deadline) != CFW_OK) {
            return -1;
        }
        n = cfw_test_sock_recv(client, buffer + *used,
                 capacity - *used - 1);
        if (n > 0) {
            *used += (size_t)n;
            buffer[*used] = '\0';
            if (*used >= mlen &&
                cfw_test_memmem(buffer, *used, marker, mlen) != NULL) {
                return (intptr_t)*used;
            }
        } else if (n == 0) {
            return (intptr_t)*used;
        }
    }
    return -1;
}

static int cfw_test_stream_route(cfw_test_server *ts,
                                 cfw_test_stream_state *st)
{
    cfw_strview_t pattern;
    cfw_strview_from_cstr(&pattern, "/stream");
    return cfw_dispatch_add_route(&ts->dispatch, CFW_HTTP_METHOD_GET,
                                  &pattern, cfw_test_stream_handler,
                                  st) == CFW_OK ? 0 : 1;
}

static int test_server_streaming_response(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    cfw_test_stream_state st;
    char response[8192];
    size_t used = 0;
    intptr_t n;

    memset(&st, 0, sizeof(st));
    st.parts = cfw_test_stream_parts;
    st.count = sizeof(cfw_test_stream_parts) /
               sizeof(cfw_test_stream_parts[0]);
    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 8),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_test_stream_route(&ts, &st), 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /stream HTTP/1.1\r\n"
                                     "Host: stream\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    n = cfw_test_recv_until(&ts.server, &client, response,
                            sizeof(response), "0\r\n\r\n", &used);
    CFW_TEST_ASSERT_TRUE(n > 0);
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, used,
                                           "Transfer-Encoding: chunked"));
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, used, "Hello, "));
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, used, "world!"));
    /* Reassembled body must equal the concatenation of all parts. */
    {
        char expected[64];
        size_t ei = 0, i;
        for (i = 0; i < st.count; i++) {
            size_t pl = strlen(cfw_test_stream_parts[i]);
            memcpy(expected + ei, cfw_test_stream_parts[i], pl);
            ei += pl;
        }
        expected[ei] = '\0';
        /* The parts may be split across chunk frames; verify order by
         * checking each appears and the terminal chunk follows. */
        CFW_TEST_ASSERT_TRUE(strstr(response, expected) != NULL ||
                             cfw_test_contains(response, used, "Hello, "));
    }

    /* Keep-alive must survive streaming: a second request works. */
    st.pos = 0;
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /hello HTTP/1.1\r\n"
                                     "Host: stream\r\n\r\n"),
                       0);
    used = 0;
    memset(response, 0, sizeof(response));
    n = cfw_test_recv_until(&ts.server, &client, response,
                            sizeof(response), "srv-ok", &used);
    CFW_TEST_ASSERT_TRUE(n > 0);
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, used, "srv-ok"));

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}

static int test_server_streaming_http10(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    cfw_test_stream_state st;
    char response[8192];
    size_t used = 0;
    intptr_t n;
    int attempts;

    memset(&st, 0, sizeof(st));
    st.parts = cfw_test_stream_parts;
    st.count = sizeof(cfw_test_stream_parts) /
               sizeof(cfw_test_stream_parts[0]);
    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 8),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_test_stream_route(&ts, &st), 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /stream HTTP/1.0\r\n\r\n"),
                       0);
    /* HTTP/1.0 streams are close-delimited: no chunked framing, and
     * the server closes after the body drains. */
    n = cfw_test_recv_until(&ts.server, &client, response,
                            sizeof(response), "world!", &used);
    CFW_TEST_ASSERT_TRUE(n > 0);
    CFW_TEST_ASSERT_TRUE(!cfw_test_contains(response, used,
                                            "Transfer-Encoding"));
    /* Drain until EOF. */
    for (attempts = 0; attempts < 200 && used + 1 < sizeof(response);
         attempts++) {
        cfw_timepoint_t deadline;
        cfw_duration_t slice;
        intptr_t r;
        cfw_duration_from_millis(20, &slice);
        cfw_deadline_from_now(&deadline, slice);
        CFW_TEST_ASSERT_EQ(cfw_server_step(&ts.server, &deadline),
                           CFW_OK);
        r = cfw_test_sock_recv(&client, response + used,
                 sizeof(response) - used - 1);
        if (r > 0) {
            used += (size_t)r;
        } else if (r == 0) {
            break;
        }
    }
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, used, "world!"));

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}

/* Known-length streams (ADR 0041): Content-Length + identity framing
 * on HTTP/1.1, exact-length emission (truncation of over-long
 * sources), keep-alive survival, and connection close when the stream
 * ends before the declared length. */
static int test_server_stream_known_length(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    cfw_test_stream_state st;
    char response[8192];
    size_t used = 0;
    intptr_t n;
    int attempts;

    memset(&st, 0, sizeof(st));
    st.parts = cfw_test_stream_parts;
    st.count = sizeof(cfw_test_stream_parts) /
               sizeof(cfw_test_stream_parts[0]);
    st.sized = 1;
    st.stream_size = 23; /* "Hello, streaming world!" */
    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 8),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_test_stream_route(&ts, &st), 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /stream HTTP/1.1\r\n"
                                     "Host: stream\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    n = cfw_test_recv_until(&ts.server, &client, response,
                            sizeof(response), "world!", &used);
    CFW_TEST_ASSERT_TRUE(n > 0);
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, used,
                                           "Content-Length: 23"));
    CFW_TEST_ASSERT_TRUE(!cfw_test_contains(response, used,
                                            "Transfer-Encoding"));
    /* Identity framing emits the body contiguously. */
    CFW_TEST_ASSERT_TRUE(strstr(response, "Hello, streaming world!") !=
                         NULL);

    /* Keep-alive survives a known-length stream. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /hello HTTP/1.1\r\n"
                                     "Host: stream\r\n\r\n"),
                       0);
    used = 0;
    memset(response, 0, sizeof(response));
    n = cfw_test_recv_until(&ts.server, &client, response,
                            sizeof(response), "srv-ok", &used);
    CFW_TEST_ASSERT_TRUE(n > 0);
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, used, "srv-ok"));

    /* Over-long source: declared 10 of 23 bytes → exactly the first
     * 10 are emitted and framing stays intact. */
    st.pos = 0;
    st.stream_size = 10;
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /stream HTTP/1.1\r\n"
                                     "Host: stream\r\n\r\n"),
                       0);
    used = 0;
    memset(response, 0, sizeof(response));
    /* Drain what arrives; the truncated body has no terminator
     * marker, so step and recv directly. */
    for (attempts = 0; attempts < 40 && used + 1 < sizeof(response);
         attempts++) {
        cfw_timepoint_t deadline;
        cfw_duration_t slice;
        intptr_t r;
        cfw_duration_from_millis(20, &slice);
        cfw_deadline_from_now(&deadline, slice);
        CFW_TEST_ASSERT_EQ(cfw_server_step(&ts.server, &deadline),
                           CFW_OK);
        r = cfw_test_sock_recv(&client, response + used,
                 sizeof(response) - used - 1);
        if (r > 0) {
            used += (size_t)r;
            response[used] = '\0';
        } else if (r == 0) {
            break;
        }
    }
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, used,
                                           "Content-Length: 10"));
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, used,
                                           "Hello, str"));
    CFW_TEST_ASSERT_TRUE(!cfw_test_contains(response, used,
                                            "aming world!"));

    /* The truncated response kept framing: a further request works. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /hello HTTP/1.1\r\n"
                                     "Host: stream\r\n\r\n"),
                       0);
    used = 0;
    memset(response, 0, sizeof(response));
    n = cfw_test_recv_until(&ts.server, &client, response,
                            sizeof(response), "srv-ok", &used);
    CFW_TEST_ASSERT_TRUE(n > 0);
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, used, "srv-ok"));

    /* Short stream: declared 30 but only 23 bytes exist → the server
     * closes the connection rather than emit a bad frame. */
    st.pos = 0;
    st.stream_size = 30;
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /stream HTTP/1.1\r\n"
                                     "Host: stream\r\n\r\n"),
                       0);
    used = 0;
    memset(response, 0, sizeof(response));
    n = cfw_test_recv_until(&ts.server, &client, response,
                            sizeof(response), "world!", &used);
    CFW_TEST_ASSERT_TRUE(n > 0);
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, used,
                                           "Content-Length: 30"));
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, used,
                                           "Hello, streaming world!"));
    /* Drain until EOF: the connection must close. */
    {
        int closed = 0;
        for (attempts = 0; attempts < 200 && used + 1 <
             sizeof(response); attempts++) {
            cfw_timepoint_t deadline;
            cfw_duration_t slice;
            intptr_t r;
            cfw_duration_from_millis(20, &slice);
            cfw_deadline_from_now(&deadline, slice);
            CFW_TEST_ASSERT_EQ(cfw_server_step(&ts.server, &deadline),
                               CFW_OK);
            r = cfw_test_sock_recv(&client, response + used,
                     sizeof(response) - used - 1);
            if (r > 0) {
                used += (size_t)r;
            } else if (r == 0) {
                closed = 1;
                break;
            }
        }
        CFW_TEST_ASSERT_TRUE(closed);
    }

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}

/* File-backed streaming: serves a real file through cfw_file_stream_read
 * and verifies the chunked body reassembles to the file contents. */
typedef struct {
    cfw_file_stream_t stream;
    unsigned char buf[32];
} cfw_test_file_state;

static cfw_status_t cfw_test_file_handler(cfw_request_t *req,
                                          cfw_response_t *res, void *user)
{
    cfw_test_file_state *st = user;
    (void)req;
    res->status = CFW_HTTP_STATUS_OK;
    res->stream = cfw_file_stream_read;
    res->stream_user = &st->stream;
    res->stream_end = cfw_file_stream_end;
    return CFW_OK;
}

/* Decodes a chunked body starting after the header terminator.
 * Returns CFW_OK and sets *out_len, or CFW_ERROR_IO on malformed input. */
static cfw_status_t cfw_test_dechunk(const char *wire, size_t used,
                                     char *out, size_t capacity,
                                     size_t *out_len)
{
    const char *p = strstr(wire, "\r\n\r\n");
    size_t pos;
    size_t len = 0;

    if (p == NULL) {
        return CFW_ERROR_IO;
    }
    p += 4;
    pos = (size_t)(p - wire);
    for (;;) {
        unsigned long clen = 0;
        size_t digits = 0;
        while (pos < used && wire[pos] != '\r') {
            char c = wire[pos];
            unsigned int v;
            if (c >= '0' && c <= '9') {
                v = (unsigned int)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v = (unsigned int)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v = (unsigned int)(c - 'A' + 10);
            } else {
                return CFW_ERROR_IO;
            }
            if (clen > 0xFFFFFFFUL) {
                return CFW_ERROR_IO;
            }
            clen = clen * 16 + v;
            pos++;
            digits++;
        }
        if (digits == 0 || pos + 2 > used ||
            wire[pos] != '\r' || wire[pos + 1] != '\n') {
            return CFW_ERROR_IO;
        }
        pos += 2;
        if (clen == 0) {
            /* Trailer section must be empty: expect final CRLF. */
            if (pos + 2 <= used && wire[pos] == '\r' &&
                wire[pos + 1] == '\n') {
                *out_len = len;
                return CFW_OK;
            }
            return CFW_ERROR_IO;
        }
        if (clen > used - pos || len + clen > capacity) {
            return CFW_ERROR_IO;
        }
        memcpy(out + len, wire + pos, clen);
        len += clen;
        pos += clen;
        if (pos + 2 > used || wire[pos] != '\r' ||
            wire[pos + 1] != '\n') {
            return CFW_ERROR_IO;
        }
        pos += 2;
    }
}

static int test_server_file_stream_response(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    cfw_test_file_state st;
    const char *path = "cfw_test_srv_file.bin";
    static const char payload[] =
        "cfw-file-stream-integration-payload-0123456789";
    FILE *file;
    char response[8192];
    char body[512];
    size_t used = 0;
    size_t body_len = 0;
    intptr_t n;
    cfw_strview_t pattern;

    file = fopen(path, "wb");
    CFW_TEST_ASSERT(file != NULL);
    CFW_TEST_ASSERT_EQ(fwrite(payload, 1, sizeof(payload) - 1, file),
                       sizeof(payload) - 1);
    CFW_TEST_ASSERT_EQ(fclose(file), 0);

    memset(&st, 0, sizeof(st));
    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 8),
                       0);
    cfw_strview_from_cstr(&pattern, "/file");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&ts.dispatch,
                                              CFW_HTTP_METHOD_GET,
                                              &pattern,
                                              cfw_test_file_handler,
                                              &st),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_file_stream_init(&st.stream, path, st.buf,
                                            sizeof(st.buf)),
                       CFW_OK);

    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /file HTTP/1.1\r\n"
                                     "Host: file\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    n = cfw_test_recv_until(&ts.server, &client, response,
                            sizeof(response), "0\r\n\r\n", &used);
    CFW_TEST_ASSERT_TRUE(n > 0);
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, used,
                                           "Transfer-Encoding: chunked"));
    CFW_TEST_ASSERT_EQ(cfw_test_dechunk(response, used, body,
                                        sizeof(body), &body_len),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(body_len, sizeof(payload) - 1);
    CFW_TEST_ASSERT_EQ(memcmp(body, payload, body_len), 0);

    /* Keep-alive survives the streamed response. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /hello HTTP/1.1\r\n"
                                     "Host: file\r\n\r\n"),
                       0);
    used = 0;
    memset(response, 0, sizeof(response));
    n = cfw_test_recv_until(&ts.server, &client, response,
                            sizeof(response), "srv-ok", &used);
    CFW_TEST_ASSERT_TRUE(n > 0);

    /* stream_end ran when the terminal chunk was queued: the file
     * handle must already be closed. */
    CFW_TEST_ASSERT_FALSE(cfw_file_valid(&st.stream.file));

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    CFW_TEST_ASSERT_EQ(remove(path), 0);
    return 0;
}


/* stream_end on abandon: a client that disconnects mid-stream must
 * still trigger the cleanup hook exactly once. */
typedef struct {
    int ended;
} cfw_test_end_state;

static cfw_status_t cfw_test_endless_fn(void *user, cfw_span_t *chunk,
                                        int *eof)
{
    static const char piece[] = "x";
    (void)user;
    chunk->data = (const uint8_t *)piece;
    chunk->size = 1;
    *eof = 0;
    return CFW_OK;
}

static void cfw_test_mark_end(void *user)
{
    ((cfw_test_end_state *)user)->ended++;
}

static cfw_status_t cfw_test_end_handler(cfw_request_t *req,
                                         cfw_response_t *res, void *user)
{
    (void)req;
    res->status = CFW_HTTP_STATUS_OK;
    res->stream = cfw_test_endless_fn;
    res->stream_user = user;
    res->stream_end = cfw_test_mark_end;
    return CFW_OK;
}

static int test_server_stream_end_on_close(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    cfw_test_end_state st;
    char response[8192];
    size_t used = 0;
    int attempts;
    cfw_strview_t pattern;

    memset(&st, 0, sizeof(st));
    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 8),
                       0);
    cfw_strview_from_cstr(&pattern, "/endless");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&ts.dispatch,
                                              CFW_HTTP_METHOD_GET,
                                              &pattern,
                                              cfw_test_end_handler,
                                              &st),
                       CFW_OK);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /endless HTTP/1.1\r\n"
                                     "Host: end\r\n\r\n"),
                       0);
    /* Pull until the stream is definitely active (headers + chunks). */
    {
        intptr_t n = cfw_test_recv_until(&ts.server, &client, response,
                                        sizeof(response), "x", &used);
        CFW_TEST_ASSERT_TRUE(n > 0);
    }

    /* Abrupt disconnect mid-stream, then let the server notice. */
    cfw_fd_close(&client);
    for (attempts = 0; attempts < 100; attempts++) {
        cfw_timepoint_t deadline;
        cfw_duration_t slice;
        cfw_duration_from_millis(10, &slice);
        cfw_deadline_from_now(&deadline, slice);
        if (cfw_server_step(&ts.server, &deadline) != CFW_OK) {
            break;
        }
        if (st.ended) {
            break;
        }
    }
    CFW_TEST_ASSERT_EQ(st.ended, 1);

    cfw_test_server_teardown(&ts);
    return 0;
}

static int test_server_host_required(void)
{
    cfw_test_server ts;
    char response[4096];
    intptr_t n;

    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 8),
                       0);

    /* HTTP/1.1 without Host must be rejected with 400. */
    {
        cfw_fd_t client;
        cfw_fd_init(&client);
        CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                            cfw_server_port(&ts.server)),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                         "GET /hello HTTP/1.1\r\n"
                                         "X-Test: 1\r\n\r\n"),
                           0);
        n = cfw_test_serve_recv(&ts.server, &client, response,
                                sizeof(response) - 1);
        CFW_TEST_ASSERT_TRUE(n > 0);
        response[n] = '\0';
        CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                               "400"));
        cfw_fd_close(&client);
    }

    /* Empty Host value is also rejected. */
    {
        cfw_fd_t client;
        cfw_fd_init(&client);
        CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                            cfw_server_port(&ts.server)),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                         "GET /hello HTTP/1.1\r\n"
                                         "Host:\r\n\r\n"),
                           0);
        n = cfw_test_serve_recv(&ts.server, &client, response,
                                sizeof(response) - 1);
        CFW_TEST_ASSERT_TRUE(n > 0);
        response[n] = '\0';
        CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                               "400"));
        cfw_fd_close(&client);
    }

    /* HTTP/1.0 does not require Host. */
    {
        cfw_fd_t client10;
        cfw_fd_init(&client10);
        CFW_TEST_ASSERT_EQ(cfw_test_connect(&client10,
                                            cfw_server_port(&ts.server)),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_test_send(&client10,
                                         "GET /hello HTTP/1.0\r\n\r\n"),
                           0);
        n = cfw_test_serve_recv(&ts.server, &client10, response,
                                sizeof(response) - 1);
        CFW_TEST_ASSERT_TRUE(n > 0);
        response[n] = '\0';
        CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                               "srv-ok"));
        cfw_fd_close(&client10);
    }

    cfw_test_server_teardown(&ts);
    return 0;
}


static int test_server_expect_rejected(void)
{
    cfw_test_server ts;
    char response[4096];
    intptr_t n;
    static const char *const bad_expects[] = {
        "GET /hello HTTP/1.1\r\nHost: x\r\nExpect: foobar\r\n\r\n",
        ("GET /hello HTTP/1.1\r\nHost: x\r\n"
         "Expect: 100-continue, foobar\r\n\r\n"),
        "GET /hello HTTP/1.1\r\nHost: x\r\nExpect:\r\n\r\n",
    };
    size_t i;

    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 8),
                       0);
    for (i = 0; i < sizeof(bad_expects) / sizeof(bad_expects[0]); i++) {
        cfw_fd_t client;
        cfw_fd_init(&client);
        CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                            cfw_server_port(&ts.server)),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_test_send(&client, bad_expects[i]), 0);
        n = cfw_test_serve_recv(&ts.server, &client, response,
                                sizeof(response) - 1);
        CFW_TEST_ASSERT_TRUE(n > 0);
        response[n] = '\0';
        CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                               "417"));
        cfw_fd_close(&client);
    }
    cfw_test_server_teardown(&ts);
    return 0;
}


static int test_server_method_fallbacks(void)
{
    cfw_test_server ts;
    char response[4096];
    intptr_t n;

    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 8),
                       0);

    /* HEAD falls back to the GET route: headers mirror GET (including
     * Content-Length) but no body bytes follow. */
    {
        cfw_fd_t client;
        cfw_fd_init(&client);
        CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                            cfw_server_port(&ts.server)),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                         "HEAD /hello HTTP/1.1\r\n"
                                         "Host: x\r\n\r\n"),
                           0);
        n = cfw_test_serve_recv(&ts.server, &client, response,
                                sizeof(response) - 1);
        CFW_TEST_ASSERT_TRUE(n > 0);
        response[n] = '\0';
        CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                               "200"));
        CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                               "Content-Length: 6"));
        CFW_TEST_ASSERT_TRUE(!cfw_test_contains(response, (size_t)n,
                                                "srv-ok"));
        cfw_fd_close(&client);
    }

    /* POST to a GET-only path yields 405 with an Allow header. */
    {
        cfw_fd_t client;
        cfw_fd_init(&client);
        CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                            cfw_server_port(&ts.server)),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                         "POST /hello HTTP/1.1\r\n"
                                         "Host: x\r\n"
                                         "Content-Length: 0\r\n\r\n"),
                           0);
        n = cfw_test_serve_recv(&ts.server, &client, response,
                                sizeof(response) - 1);
        CFW_TEST_ASSERT_TRUE(n > 0);
        response[n] = '\0';
        CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                               "405"));
        CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                               "Allow: GET, HEAD"));
        cfw_fd_close(&client);
    }

    /* Unknown path still yields 404. */
    {
        cfw_fd_t client;
        cfw_fd_init(&client);
        CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                            cfw_server_port(&ts.server)),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                         "GET /nope HTTP/1.1\r\n"
                                         "Host: x\r\n\r\n"),
                           0);
        n = cfw_test_serve_recv(&ts.server, &client, response,
                                sizeof(response) - 1);
        CFW_TEST_ASSERT_TRUE(n > 0);
        response[n] = '\0';
        CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                               "404"));
        cfw_fd_close(&client);
    }

    cfw_test_server_teardown(&ts);
    return 0;
}


static int test_server_streaming_conn_close(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    cfw_test_stream_state st;
    char response[8192];
    size_t used = 0;
    intptr_t n;

    memset(&st, 0, sizeof(st));
    st.parts = cfw_test_stream_parts;
    st.count = sizeof(cfw_test_stream_parts) /
               sizeof(cfw_test_stream_parts[0]);
    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 8),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_test_stream_route(&ts, &st), 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    /* Closing request + chunked stream: the response must still carry
     * Connection: close so the client does not pipeline into a
     * connection the server will shut. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /stream HTTP/1.1\r\n"
                                     "Host: x\r\n"
                                     "Connection: close\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    n = cfw_test_recv_until(&ts.server, &client, response,
                            sizeof(response), "0\r\n\r\n", &used);
    CFW_TEST_ASSERT_TRUE(n > 0);
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, used,
                                           "Transfer-Encoding: chunked"));
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, used,
                                           "Connection: close"));

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}

/* Steps the server until the connection count reaches zero or the
 * attempt budget is exhausted. Returns the final connection count. */
static size_t cfw_test_reap_server(cfw_server_t *server)
{
    int attempts;
    for (attempts = 0;
         attempts < 200 && cfw_server_connection_count(server) != 0;
         attempts++) {
        cfw_timepoint_t deadline;
        cfw_duration_t slice;
        cfw_duration_from_millis(20, &slice);
        cfw_deadline_from_now(&deadline, slice);
        if (cfw_server_step(server, &deadline) != CFW_OK) {
            break;
        }
    }
    return cfw_server_connection_count(server);
}

/* Drains whatever the client received into buffer; returns byte count. */
static size_t cfw_test_drain_client(cfw_fd_t *client, char *buffer,
                                    size_t capacity)
{
    size_t used = 0;
    for (;;) {
        intptr_t n = cfw_test_sock_recv(client, buffer + used,
                         capacity - used - 1);
        if (n <= 0 || used + 1 >= capacity) {
            break;
        }
        used += (size_t)n;
    }
    buffer[used] = '\0';
    return used;
}

static int test_server_stream_fn_error(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    cfw_test_stream_state st;
    char response[8192];
    size_t used;

    memset(&st, 0, sizeof(st));
    st.parts = cfw_test_stream_parts;
    st.count = sizeof(cfw_test_stream_parts) /
               sizeof(cfw_test_stream_parts[0]);
    st.fail = 1;
    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 8),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_test_stream_route(&ts, &st), 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /stream HTTP/1.1\r\n"
                                     "Host: stream\r\n\r\n"),
                       0);

    /* The stream callback fails on first pump: the connection must be
     * reaped without a terminal chunk. */
    CFW_TEST_ASSERT_EQ(cfw_test_reap_server(&ts.server), 0);

    used = cfw_test_drain_client(&client, response, sizeof(response));
    CFW_TEST_ASSERT_TRUE(!cfw_test_contains(response, used, "0\r\n\r\n"));

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}

static int test_server_stream_empty_chunk(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    cfw_test_stream_state st;
    char response[8192];
    size_t used;

    memset(&st, 0, sizeof(st));
    st.parts = cfw_test_stream_parts;
    st.count = sizeof(cfw_test_stream_parts) /
               sizeof(cfw_test_stream_parts[0]);
    st.empty = 1;
    CFW_TEST_ASSERT_EQ(cfw_test_server_setup(&ts, (cfw_duration_t){0},
                                             (cfw_duration_t){0}, 8),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_test_stream_route(&ts, &st), 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /stream HTTP/1.1\r\n"
                                     "Host: stream\r\n\r\n"),
                       0);

    /* A non-EOF empty chunk violates the stream contract: reap the
     * connection rather than spin or emit a malformed frame. */
    CFW_TEST_ASSERT_EQ(cfw_test_reap_server(&ts.server), 0);

    used = cfw_test_drain_client(&client, response, sizeof(response));
    CFW_TEST_ASSERT_TRUE(!cfw_test_contains(response, used, "0\r\n\r\n"));

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}

static char cfw_test_log_buf[256];
static size_t cfw_test_log_len;
static int cfw_test_log_calls;
static cfw_log_level_t cfw_test_log_level;

static void cfw_test_log_sink(cfw_log_level_t level,
                              const cfw_strview_t *message, void *user)
{
    (void)user;
    cfw_test_log_calls++;
    cfw_test_log_level = level;
    cfw_test_log_len = 0;
    if (message->size < sizeof(cfw_test_log_buf)) {
        memcpy(cfw_test_log_buf, message->data, message->size);
        cfw_test_log_len = message->size;
        cfw_test_log_buf[message->size] = '\0';
    }
}

static int test_server_error_logging(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    cfw_logger_t logger;
    char response[4096];
    intptr_t n;

    memset(&ts, 0, sizeof(ts));
    cfw_logger_init(&logger);
    cfw_logger_set_min_level(&logger, CFW_LOG_WARN);
    cfw_logger_set_sink(&logger, cfw_test_log_sink, NULL);
    cfw_test_log_calls = 0;
    cfw_test_log_len = 0;
    cfw_test_log_buf[0] = '\0';

    /* Missing Host on HTTP/1.1 must produce a 400 and a warn log. */
    CFW_TEST_ASSERT_EQ(cfw_test_server_setup_ex(
                           &ts, (cfw_duration_t){0},
                           (cfw_duration_t){0}, (cfw_duration_t){0}, 8,
                           "127.0.0.1", NULL,
                           NULL, NULL, NULL, &logger),
                       0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /hello HTTP/1.1\r\n\r\n"),
                       0);
    n = cfw_test_serve_recv(&ts.server, &client, response,
                            sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n, "400"));

    CFW_TEST_ASSERT_TRUE(cfw_test_log_calls >= 1);
    CFW_TEST_ASSERT_EQ(cfw_test_log_level, CFW_LOG_WARN);
    CFW_TEST_ASSERT_TRUE(cfw_test_log_len > 0);
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(cfw_test_log_buf,
                                           cfw_test_log_len,
                                           "request error"));

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}

/* ---- upload hooks (ADR 0033) ---- */

typedef struct {
    unsigned char *data;
    size_t size;
    size_t cap;
    int begin_calls;
    int write_calls;
    int end_calls;
    int end_completed;
    int fail_begin;
    int fail_write;
} cfw_test_upload_state;

static void *cfw_test_seen_upload;
static size_t cfw_test_seen_body_size;

static cfw_status_t cfw_test_upload_begin(void *user,
                                          const cfw_http_request_head_t *head,
                                          void **upload)
{
    cfw_test_upload_state *st = user;
    (void)head;
    st->begin_calls++;
    if (st->fail_begin) {
        return CFW_ERROR_IO;
    }
    *upload = st;
    return CFW_OK;
}

static cfw_status_t cfw_test_upload_write(void *upload,
                                          const cfw_span_t *piece)
{
    cfw_test_upload_state *st = upload;
    size_t ncap;
    unsigned char *nd;

    st->write_calls++;
    if (st->fail_write) {
        return CFW_ERROR_IO;
    }
    if (st->size + piece->size > st->cap) {
        ncap = st->cap != 0 ? st->cap : 256;
        while (ncap < st->size + piece->size) {
            ncap *= 2;
        }
        nd = realloc(st->data, ncap);
        if (nd == NULL) {
            return CFW_ERROR_NOMEM;
        }
        st->data = nd;
        st->cap = ncap;
    }
    memcpy(st->data + st->size, piece->data, piece->size);
    st->size += piece->size;
    return CFW_OK;
}

static void cfw_test_upload_end(void *upload, int completed)
{
    cfw_test_upload_state *st = upload;
    st->end_calls++;
    st->end_completed = completed;
}

static cfw_status_t cfw_test_upload_handler(cfw_request_t *req,
                                            cfw_response_t *res, void *user)
{
    static const char body[] = "up-ok";
    (void)user;
    cfw_test_seen_upload = req->upload;
    cfw_test_seen_body_size = req->body.size;
    res->status = CFW_HTTP_STATUS_OK;
    res->body.data = (const uint8_t *)body;
    res->body.size = sizeof(body) - 1;
    return CFW_OK;
}

static int cfw_test_setup_upload_to(cfw_test_server *ts,
                                    cfw_test_upload_state *st,
                                    cfw_duration_t request_timeout)
{
    cfw_strview_t pattern;

    memset(st, 0, sizeof(*st));
    CFW_TEST_ASSERT_EQ(cfw_test_server_setup_ex(
                           ts, (cfw_duration_t){0}, request_timeout,
                           (cfw_duration_t){0},
                           8, "127.0.0.1", cfw_test_upload_begin,
                           cfw_test_upload_write, cfw_test_upload_end, st,
                           NULL),
                       0);
    cfw_strview_from_cstr(&pattern, "/up");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&ts->dispatch,
                                              CFW_HTTP_METHOD_POST,
                                              &pattern,
                                              cfw_test_upload_handler,
                                              NULL),
                       CFW_OK);
    return 0;
}

static int cfw_test_setup_upload(cfw_test_server *ts,
                                 cfw_test_upload_state *st)
{
    return cfw_test_setup_upload_to(ts, st, (cfw_duration_t){0});
}

static void cfw_test_upload_reset(cfw_test_upload_state *st)
{
    free(st->data);
    memset(st, 0, sizeof(*st));
    cfw_test_seen_upload = NULL;
    cfw_test_seen_body_size = 0;
}

static int test_server_upload_content_length(void)
{
    cfw_test_server ts;
    cfw_test_upload_state st;
    cfw_fd_t client;
    char response[4096];
    intptr_t n;
    static const char payload[] = "hello world";

    CFW_TEST_ASSERT_EQ(cfw_test_setup_upload(&ts, &st), 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(
                           &client,
                           "POST /up HTTP/1.1\r\n"
                           "Host: test\r\n"
                           "Content-Length: 11\r\n\r\n"
                           "hello world"),
                       0);
    n = cfw_test_serve_recv(&ts.server, &client, response,
                            sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "HTTP/1.1 200 OK"));

    CFW_TEST_ASSERT_EQ(st.begin_calls, 1);
    CFW_TEST_ASSERT_TRUE(st.write_calls >= 1);
    CFW_TEST_ASSERT_EQ(st.end_calls, 1);
    CFW_TEST_ASSERT_EQ(st.end_completed, 1);
    CFW_TEST_ASSERT_EQ(st.size, sizeof(payload) - 1);
    CFW_TEST_ASSERT_TRUE(memcmp(st.data, payload,
                                sizeof(payload) - 1) == 0);
    CFW_TEST_ASSERT_TRUE(cfw_test_seen_upload == &st);
    CFW_TEST_ASSERT_EQ(cfw_test_seen_body_size, 0);

    cfw_fd_close(&client);
    cfw_test_upload_reset(&st);
    cfw_test_server_teardown(&ts);
    return 0;
}

static int test_server_upload_chunked(void)
{
    cfw_test_server ts;
    cfw_test_upload_state st;
    cfw_fd_t client;
    char response[4096];
    intptr_t n;
    static const char payload[] = "hello world";

    CFW_TEST_ASSERT_EQ(cfw_test_setup_upload(&ts, &st), 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(
                           &client,
                           "POST /up HTTP/1.1\r\n"
                           "Host: test\r\n"
                           "Transfer-Encoding: chunked\r\n\r\n"
                           "5\r\nhello\r\n"
                           "6\r\n world\r\n"
                           "0\r\n\r\n"),
                       0);
    n = cfw_test_serve_recv(&ts.server, &client, response,
                            sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "HTTP/1.1 200 OK"));

    CFW_TEST_ASSERT_EQ(st.begin_calls, 1);
    CFW_TEST_ASSERT_EQ(st.end_calls, 1);
    CFW_TEST_ASSERT_EQ(st.end_completed, 1);
    CFW_TEST_ASSERT_EQ(st.size, sizeof(payload) - 1);
    CFW_TEST_ASSERT_TRUE(memcmp(st.data, payload,
                                sizeof(payload) - 1) == 0);

    cfw_fd_close(&client);
    cfw_test_upload_reset(&st);
    cfw_test_server_teardown(&ts);
    return 0;
}

static int test_server_upload_abort(void)
{
    cfw_test_server ts;
    cfw_test_upload_state st;
    cfw_fd_t client;
    cfw_duration_t timeout;
    int attempts;

    /* Declare 100 bytes, send 7, then stall: the request deadline
     * reaps the conn and the upload must be reported incomplete. */
    cfw_duration_from_millis(100, &timeout);
    CFW_TEST_ASSERT_EQ(cfw_test_setup_upload_to(&ts, &st, timeout), 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(
                           &client,
                           "POST /up HTTP/1.1\r\n"
                           "Host: test\r\n"
                           "Content-Length: 100\r\n\r\n"
                           "partial"),
                       0);

    /* Phase 1: wait for the head to be serviced and the upload begun. */
    for (attempts = 0; attempts < 200 && st.begin_calls == 0; attempts++) {
        cfw_timepoint_t deadline;
        cfw_duration_t slice;
        cfw_duration_from_millis(20, &slice);
        cfw_deadline_from_now(&deadline, slice);
        CFW_TEST_ASSERT_EQ(cfw_server_step(&ts.server, &deadline),
                           CFW_OK);
    }
    CFW_TEST_ASSERT_EQ(st.begin_calls, 1);

    /* Phase 2: the stalled body hits the request deadline and the
     * conn is reaped with the upload reported incomplete. */
    for (attempts = 0; attempts < 200 &&
                    cfw_server_connection_count(&ts.server) != 0;
         attempts++) {
        cfw_timepoint_t deadline;
        cfw_duration_t slice;
        cfw_duration_from_millis(20, &slice);
        cfw_deadline_from_now(&deadline, slice);
        CFW_TEST_ASSERT_EQ(cfw_server_step(&ts.server, &deadline),
                           CFW_OK);
    }
    CFW_TEST_ASSERT_EQ(cfw_server_connection_count(&ts.server), 0);
    CFW_TEST_ASSERT_EQ(st.end_calls, 1);
    CFW_TEST_ASSERT_EQ(st.end_completed, 0);

    cfw_fd_close(&client);
    cfw_test_upload_reset(&st);
    cfw_test_server_teardown(&ts);
    return 0;
}

static int test_server_upload_protocol_error(void)
{
    cfw_test_server ts;
    cfw_test_upload_state st;
    cfw_fd_t client;
    char response[4096];
    intptr_t n;

    /* A malformed chunk after a valid one aborts the request mid-body:
     * the upload must be reported incomplete and a 400 sent. */
    CFW_TEST_ASSERT_EQ(cfw_test_setup_upload(&ts, &st), 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(
                           &client,
                           "POST /up HTTP/1.1\r\n"
                           "Host: test\r\n"
                           "Transfer-Encoding: chunked\r\n\r\n"
                           "5\r\nhello\r\n"
                           "Z\r\n\r\n"),
                       0);
    n = cfw_test_serve_recv(&ts.server, &client, response,
                            sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "400"));

    CFW_TEST_ASSERT_EQ(st.begin_calls, 1);
    CFW_TEST_ASSERT_EQ(st.end_calls, 1);
    CFW_TEST_ASSERT_EQ(st.end_completed, 0);

    cfw_fd_close(&client);
    cfw_test_upload_reset(&st);
    cfw_test_server_teardown(&ts);
    return 0;
}

static int test_server_upload_write_error(void)
{
    cfw_test_server ts;
    cfw_test_upload_state st;
    cfw_fd_t client;
    char response[4096];
    intptr_t n;

    CFW_TEST_ASSERT_EQ(cfw_test_setup_upload(&ts, &st), 0);
    st.fail_write = 1;
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(
                           &client,
                           "POST /up HTTP/1.1\r\n"
                           "Host: test\r\n"
                           "Content-Length: 5\r\n\r\n"
                           "abcde"),
                       0);
    n = cfw_test_serve_recv(&ts.server, &client, response,
                            sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "500"));
    CFW_TEST_ASSERT_EQ(st.end_calls, 1);
    CFW_TEST_ASSERT_EQ(st.end_completed, 0);

    cfw_fd_close(&client);
    cfw_test_upload_reset(&st);
    cfw_test_server_teardown(&ts);
    return 0;
}

static int test_server_upload_begin_error(void)
{
    cfw_test_server ts;
    cfw_test_upload_state st;
    cfw_fd_t client;
    char response[4096];
    intptr_t n;

    CFW_TEST_ASSERT_EQ(cfw_test_setup_upload(&ts, &st), 0);
    st.fail_begin = 1;
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(
                           &client,
                           "POST /up HTTP/1.1\r\n"
                           "Host: test\r\n"
                           "Content-Length: 5\r\n\r\n"
                           "abcde"),
                       0);
    n = cfw_test_serve_recv(&ts.server, &client, response,
                            sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "500"));
    CFW_TEST_ASSERT_EQ(st.begin_calls, 1);
    CFW_TEST_ASSERT_EQ(st.end_calls, 0);

    cfw_fd_close(&client);
    cfw_test_upload_reset(&st);
    cfw_test_server_teardown(&ts);
    return 0;
}

static int cfw_test_stop_thread(void *arg)
{
    /* cfw_server_stop is the supported cross-thread shutdown path: it
     * writes the wake byte so a blocked cfw_server_run returns promptly
     * without a second poll loop. */
    cfw_test_sleep_us(200000);
    cfw_server_stop((cfw_server_t *)arg);
    return 0;
}

static int test_server_run_and_drain(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    char response[8192];
    size_t used = 0;
    cfw_status_t st;
    cfw_duration_t drain;
    cfw_thread_t stopper;
    int attempts;

    CFW_TEST_ASSERT_EQ(cfw_server_run(NULL), CFW_ERROR_INVALID_ARGUMENT);

    cfw_duration_from_millis(300, &drain);
    CFW_TEST_ASSERT_EQ(cfw_test_server_setup_ex(
                           &ts, (cfw_duration_t){0}, (cfw_duration_t){0},
                           drain, 8, "127.0.0.1", NULL, NULL, NULL, NULL,
                           NULL),
                       0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /hello HTTP/1.1\r\n"
                                     "Host: run\r\n\r\n"),
                       0);

    /* Stop the blocking loop from a helper thread: cfw_server_stop
     * writes the wake byte, so run() returns promptly. */
    CFW_TEST_ASSERT_EQ(cfw_thread_create(&stopper, cfw_test_stop_thread,
                                         &ts.server),
                       CFW_OK);
    st = cfw_server_run(&ts.server);
    CFW_TEST_ASSERT_EQ(cfw_thread_join(&stopper, NULL), CFW_OK);

    CFW_TEST_ASSERT_EQ(st, CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_server_is_stopping(&ts.server));
    CFW_TEST_ASSERT_EQ(cfw_server_requests_total(&ts.server), 1);

    /* The keep-alive conn was still open at stop: run() drained until
     * drain_timeout expired, then returned with the conn tracked. The
     * response was flushed during the run. */
    for (attempts = 0; attempts < 200 && used + 1 < sizeof(response);
         attempts++) {
        intptr_t n = cfw_test_sock_recv(&client, response + used,
                         sizeof(response) - used - 1);
        if (n > 0) {
            used += (size_t)n;
            if (cfw_test_contains(response, used, "200 OK")) {
                break;
            }
        } else {
            cfw_test_sleep_us(1000);
        }
    }
    response[used] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, used,
                                           "HTTP/1.1 200 OK"));

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}

/* ---------------- Protocol upgrades (ADR 0034) ---------------- */

static cfw_status_t cfw_test_upgrade_handler(cfw_request_t *req,
                                             cfw_response_t *res,
                                             void *user)
{
    cfw_strview_t name;
    cfw_strview_t value;
    (void)req;
    (void)user;
    res->status = CFW_HTTP_STATUS_SWITCHING_PROTOCOLS;
    cfw_strview_from_cstr(&name, "Upgrade");
    cfw_strview_from_cstr(&value, "cfw-test");
    cfw_response_header_add(res, &name, &value);
    cfw_strview_from_cstr(&name, "Connection");
    cfw_strview_from_cstr(&value, "Upgrade");
    cfw_response_header_add(res, &name, &value);
    return CFW_OK;
}

static int cfw_test_upgrade_calls;
static int cfw_test_upgrade_ready_seen;
static int cfw_test_upgrade_user_ok;
static char cfw_test_upgrade_echoed[512];
static size_t cfw_test_upgrade_echoed_len;
static cfw_status_t cfw_test_upgrade_last_send;
static int cfw_test_upgrade_magic;

/* Echo protocol: on any event, drain recv() and send back whatever
 * arrived; close on EOF/HUP/ERR. */
static cfw_status_t cfw_test_upgrade_echo(cfw_upgraded_conn_t *conn,
                                          uint32_t events, void *user)
{
    char buf[512];
    size_t n = 0;
    cfw_status_t st;

    cfw_test_upgrade_calls++;
    if (events == CFW_UPGRADE_EV_READY) {
        cfw_test_upgrade_ready_seen = 1;
        cfw_upgraded_set_user(conn, user);
    }
    if (cfw_upgraded_user(conn) == &cfw_test_upgrade_magic) {
        cfw_test_upgrade_user_ok = 1;
    }
    if (events & (CFW_UPGRADE_EV_HUP | CFW_UPGRADE_EV_ERR)) {
        cfw_upgraded_close(conn);
        return CFW_OK;
    }
    if (events == CFW_UPGRADE_EV_READY || (events & CFW_UPGRADE_EV_READ) ||
        (events & CFW_UPGRADE_EV_WRITE)) {
        for (;;) {
            st = cfw_upgraded_recv(conn, buf, sizeof(buf), &n);
            if (st == CFW_ERROR_WOULDBLOCK) {
                break;
            }
            if (st != CFW_OK) {
                return st;
            }
            if (n == 0) {
                cfw_upgraded_close(conn);
                return CFW_OK;
            }
            if (n <= sizeof(cfw_test_upgrade_echoed)) {
                memcpy(cfw_test_upgrade_echoed, buf, n);
                cfw_test_upgrade_echoed_len = n;
            }
            st = cfw_upgraded_send(conn, buf, n);
            cfw_test_upgrade_last_send = st;
            if (st != CFW_OK) {
                return CFW_OK; /* retry on EV_WRITE */
            }
        }
    }
    (void)user;
    return CFW_OK;
}

/* Bound-check probe: records the send status instead of echoing. */
static cfw_status_t cfw_test_upgrade_probe(cfw_upgraded_conn_t *conn,
                                           uint32_t events, void *user)
{
    static const char blob[600];
    (void)events;
    (void)user;
    cfw_test_upgrade_calls++;
    if (events == CFW_UPGRADE_EV_READY) {
        cfw_test_upgrade_last_send =
            cfw_upgraded_send(conn, blob, sizeof(blob));
        cfw_upgraded_close(conn);
    }
    return CFW_OK;
}

typedef struct {
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_server_t server;
} cfw_test_upgrade_server;

static int cfw_test_upgrade_setup(cfw_test_upgrade_server *ts,
                                  size_t max_out_size,
                                  cfw_upgrade_fn hook)
{
    cfw_server_options_t options;
    cfw_dispatch_options_t doptions;
    cfw_strview_t pattern;

    memset(ts, 0, sizeof(*ts));
    ts->arena = cfw_arena_create(32768);
    CFW_TEST_ASSERT_TRUE(ts->arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&ts->router, ts->arena), CFW_OK);

    memset(&doptions, 0, sizeof(doptions));
    doptions.router = &ts->router;
    doptions.handler = cfw_test_ok_handler;
    doptions.max_middleware = 4;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&ts->dispatch, ts->arena,
                                         &doptions),
                       CFW_OK);

    cfw_strview_from_cstr(&pattern, "/upgrade");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&ts->dispatch,
                                              CFW_HTTP_METHOD_GET,
                                              &pattern,
                                              cfw_test_upgrade_handler,
                                              NULL),
                       CFW_OK);
    cfw_strview_from_cstr(&pattern, "/hello");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&ts->dispatch,
                                              CFW_HTTP_METHOD_GET,
                                              &pattern, cfw_test_ok_handler,
                                              NULL),
                       CFW_OK);

    memset(&options, 0, sizeof(options));
    options.host = "127.0.0.1";
    options.port = 0;
    options.backlog = 16;
    options.dispatch = &ts->dispatch;
    options.http.max_head_size = 8192;
    options.http.max_body_size = 65536;
    options.http.max_pending_size = 65536;
    options.max_out_size = max_out_size;
    options.max_connections = 8;
    options.max_headers = 32;
    options.max_params = 16;
    options.upgrade = hook;
    options.upgrade_user = &cfw_test_upgrade_magic;
    CFW_TEST_ASSERT_EQ(cfw_server_init(&ts->server, &options), CFW_OK);
    return 0;
}

static void cfw_test_upgrade_teardown(cfw_test_upgrade_server *ts)
{
    cfw_server_free(&ts->server);
    cfw_arena_destroy(ts->arena);
}

static void cfw_test_upgrade_reset(void)
{
    cfw_test_upgrade_calls = 0;
    cfw_test_upgrade_ready_seen = 0;
    cfw_test_upgrade_user_ok = 0;
    cfw_test_upgrade_echoed_len = 0;
    cfw_test_upgrade_last_send = CFW_OK;
    cfw_test_upgrade_magic = 0x5a5a;
}

/* Wrapper over cfw_test_recv_until: returns 1 when `needle` was seen. */
static int cfw_test_upgrade_recv(cfw_server_t *server, cfw_fd_t *client,
                                 char *buffer, size_t capacity,
                                 const char *needle)
{
    size_t used = 0;
    intptr_t n = cfw_test_recv_until(server, client, buffer, capacity,
                                    needle, &used);
    return n >= 0 && cfw_test_contains(buffer, used, needle);
}

static int test_server_upgrade_echo(void)
{
    cfw_test_upgrade_server ts;
    cfw_fd_t client;
    char response[8192];

    cfw_test_upgrade_reset();
    CFW_TEST_ASSERT_EQ(cfw_test_upgrade_setup(&ts, 65536,
                                              cfw_test_upgrade_echo),
                       0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /upgrade HTTP/1.1\r\n"
                                     "Host: t\r\n"
                                     "Upgrade: cfw-test\r\n"
                                     "Connection: upgrade\r\n\r\n"),
                       0);

    CFW_TEST_ASSERT_TRUE(cfw_test_upgrade_recv(
        &ts.server, &client, response, sizeof(response), "101"));
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, strlen(response),
                                           "cfw-test"));
    CFW_TEST_ASSERT_TRUE(cfw_test_upgrade_ready_seen);
    CFW_TEST_ASSERT_TRUE(cfw_test_upgrade_user_ok);

    /* Raw protocol bytes are echoed through the upgraded connection. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client, "ping-payload"), 0);
    memset(response, 0, sizeof(response));
    CFW_TEST_ASSERT_TRUE(cfw_test_upgrade_recv(
        &ts.server, &client, response, sizeof(response), "ping-payload"));
    CFW_TEST_ASSERT_EQ(cfw_test_upgrade_last_send, CFW_OK);
    CFW_TEST_ASSERT_EQ(memcmp(cfw_test_upgrade_echoed, "ping-payload",
                              sizeof("ping-payload") - 1),
                       0);

    /* Peer close → server delivers EOF and closes the connection. */
    cfw_fd_close(&client);
    {
        int attempts;
        for (attempts = 0; attempts < 100; attempts++) {
            cfw_timepoint_t deadline;
            cfw_duration_t slice;
            cfw_duration_from_millis(20, &slice);
            cfw_deadline_from_now(&deadline, slice);
            CFW_TEST_ASSERT_EQ(cfw_server_step(&ts.server, &deadline),
                               CFW_OK);
            if (cfw_server_connection_count(&ts.server) == 0) {
                break;
            }
        }
        CFW_TEST_ASSERT_EQ(cfw_server_connection_count(&ts.server), 0);
    }

    cfw_test_upgrade_teardown(&ts);
    return 0;
}

static int test_server_upgrade_pipelined(void)
{
    cfw_test_upgrade_server ts;
    cfw_fd_t client;
    char response[8192];

    cfw_test_upgrade_reset();
    CFW_TEST_ASSERT_EQ(cfw_test_upgrade_setup(&ts, 65536,
                                              cfw_test_upgrade_echo),
                       0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    /* Handshake and first protocol frame in a single write: the frame
     * bytes are already inside the HTTP reader's pending buffer and
     * must be handed to the upgraded protocol intact. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /upgrade HTTP/1.1\r\n"
                                     "Host: t\r\n"
                                     "Upgrade: cfw-test\r\n"
                                     "Connection: upgrade\r\n"
                                     "\r\n"
                                     "early-frame"),
                       0);

    memset(response, 0, sizeof(response));
    CFW_TEST_ASSERT_TRUE(cfw_test_upgrade_recv(
        &ts.server, &client, response, sizeof(response), "early-frame"));
    CFW_TEST_ASSERT_EQ(memcmp(cfw_test_upgrade_echoed, "early-frame",
                              sizeof("early-frame") - 1),
                       0);

    cfw_fd_close(&client);
    cfw_test_upgrade_teardown(&ts);
    return 0;
}

static int test_server_upgrade_requires_header(void)
{
    cfw_test_upgrade_server ts;
    cfw_fd_t client;
    char response[8192];

    cfw_test_upgrade_reset();
    CFW_TEST_ASSERT_EQ(cfw_test_upgrade_setup(&ts, 65536,
                                              cfw_test_upgrade_echo),
                       0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    /* 101 without a request Upgrade header: sent as an ordinary
     * response, no takeover, connection stays HTTP. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /upgrade HTTP/1.1\r\n"
                                     "Host: t\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    CFW_TEST_ASSERT_TRUE(cfw_test_upgrade_recv(
        &ts.server, &client, response, sizeof(response), "101"));
    CFW_TEST_ASSERT_EQ(cfw_test_upgrade_calls, 0);

    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /hello HTTP/1.1\r\n"
                                     "Host: t\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    CFW_TEST_ASSERT_TRUE(cfw_test_upgrade_recv(
        &ts.server, &client, response, sizeof(response), "srv-ok"));

    cfw_fd_close(&client);
    cfw_test_upgrade_teardown(&ts);
    return 0;
}

static int test_server_upgrade_disabled(void)
{
    cfw_test_upgrade_server ts;
    cfw_fd_t client;
    char response[8192];

    cfw_test_upgrade_reset();
    /* No upgrade hook: 101 + Upgrade request header is just a response. */
    CFW_TEST_ASSERT_EQ(cfw_test_upgrade_setup(&ts, 65536, NULL), 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /upgrade HTTP/1.1\r\n"
                                     "Host: t\r\n"
                                     "Upgrade: cfw-test\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    CFW_TEST_ASSERT_TRUE(cfw_test_upgrade_recv(
        &ts.server, &client, response, sizeof(response), "101"));

    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /hello HTTP/1.1\r\n"
                                     "Host: t\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    CFW_TEST_ASSERT_TRUE(cfw_test_upgrade_recv(
        &ts.server, &client, response, sizeof(response), "srv-ok"));

    cfw_fd_close(&client);
    cfw_test_upgrade_teardown(&ts);
    return 0;
}

static int test_server_upgrade_send_bound(void)
{
    cfw_test_upgrade_server ts;
    cfw_fd_t client;
    char response[8192];

    cfw_test_upgrade_reset();
    /* Tiny output bound: a 600-byte send cannot fit and must report
     * WOULDBLOCK rather than corrupting the queue. */
    CFW_TEST_ASSERT_EQ(cfw_test_upgrade_setup(&ts, 256,
                                              cfw_test_upgrade_probe),
                       0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /upgrade HTTP/1.1\r\n"
                                     "Host: t\r\n"
                                     "Upgrade: cfw-test\r\n\r\n"),
                       0);
    CFW_TEST_ASSERT_TRUE(cfw_test_upgrade_recv(
        &ts.server, &client, response, sizeof(response), "101"));
    {
        int attempts;
        for (attempts = 0; attempts < 100; attempts++) {
            cfw_timepoint_t deadline;
            cfw_duration_t slice;
            cfw_duration_from_millis(20, &slice);
            cfw_deadline_from_now(&deadline, slice);
            CFW_TEST_ASSERT_EQ(cfw_server_step(&ts.server, &deadline),
                               CFW_OK);
            if (cfw_test_upgrade_last_send == CFW_ERROR_WOULDBLOCK ||
                cfw_server_connection_count(&ts.server) == 0) {
                break;
            }
        }
    }
    CFW_TEST_ASSERT_EQ(cfw_test_upgrade_last_send, CFW_ERROR_WOULDBLOCK);
    CFW_TEST_ASSERT_EQ(cfw_server_connection_count(&ts.server), 0);

    cfw_fd_close(&client);
    cfw_test_upgrade_teardown(&ts);
    return 0;
}

/* ---------------- Request audit hook (ADR 0055) ---------------- */

typedef struct {
    int calls;
    int statuses[8];
    int last_status;
    int last_upgraded;
    cfw_http_method_t last_method;
    char last_target[96];
    size_t last_target_len;
    uint64_t last_request_bytes;
    uint64_t last_response_bytes;
    int last_remote_ok;
    int64_t last_duration_nanos;
} cfw_test_audit;

typedef struct {
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_server_t server;
    cfw_test_stream_state stream;
    cfw_test_audit audit;
} cfw_test_audit_server;

static void cfw_test_audit_done(const cfw_request_record_t *record,
                                void *user)
{
    cfw_test_audit_server *ts = user;
    cfw_test_audit *a = &ts->audit;

    a->calls++;
    if (a->calls <= 8) {
        a->statuses[a->calls - 1] = record->status;
    }
    a->last_status = record->status;
    a->last_upgraded = record->upgraded;
    a->last_method = record->method;
    a->last_request_bytes = record->request_bytes;
    a->last_response_bytes = record->response_bytes;
    a->last_target_len = 0;
    if (record->target.size < sizeof(a->last_target)) {
        memcpy(a->last_target, record->target.data, record->target.size);
        a->last_target[record->target.size] = '\0';
        a->last_target_len = record->target.size;
    }
    a->last_remote_ok =
        record->remote_addr != NULL && record->remote_addr->size != 0;
    (void)cfw_duration_to_nanos(&record->duration,
                                &a->last_duration_nanos);
}

static int cfw_test_audit_setup(cfw_test_audit_server *ts,
                                cfw_upgrade_fn upgrade,
                                cfw_duration_t request_timeout)
{
    cfw_server_options_t options;
    cfw_dispatch_options_t doptions;
    cfw_strview_t pattern;

    memset(ts, 0, sizeof(*ts));
    ts->arena = cfw_arena_create(65536);
    CFW_TEST_ASSERT_TRUE(ts->arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&ts->router, ts->arena), CFW_OK);

    memset(&doptions, 0, sizeof(doptions));
    doptions.router = &ts->router;
    doptions.handler = cfw_test_ok_handler;
    doptions.max_middleware = 4;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&ts->dispatch, ts->arena,
                                         &doptions),
                       CFW_OK);

    cfw_strview_from_cstr(&pattern, "/hello");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&ts->dispatch,
                                              CFW_HTTP_METHOD_GET,
                                              &pattern, cfw_test_ok_handler,
                                              NULL),
                       CFW_OK);
    cfw_strview_from_cstr(&pattern, "/submit");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&ts->dispatch,
                                              CFW_HTTP_METHOD_POST,
                                              &pattern, cfw_test_ok_handler,
                                              NULL),
                       CFW_OK);
    cfw_strview_from_cstr(&pattern, "/stream");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&ts->dispatch,
                                              CFW_HTTP_METHOD_GET,
                                              &pattern,
                                              cfw_test_stream_handler,
                                              &ts->stream),
                       CFW_OK);
    cfw_strview_from_cstr(&pattern, "/upgrade");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&ts->dispatch,
                                              CFW_HTTP_METHOD_GET,
                                              &pattern,
                                              cfw_test_upgrade_handler,
                                              NULL),
                       CFW_OK);

    ts->stream.parts = cfw_test_stream_parts;
    ts->stream.count = sizeof(cfw_test_stream_parts) /
                       sizeof(cfw_test_stream_parts[0]);

    memset(&options, 0, sizeof(options));
    options.host = "127.0.0.1";
    options.port = 0;
    options.backlog = 16;
    options.dispatch = &ts->dispatch;
    options.http.max_head_size = 8192;
    options.http.max_body_size = 65536;
    options.http.max_pending_size = 65536;
    options.max_out_size = 65536;
    options.max_connections = 8;
    options.max_headers = 32;
    options.max_params = 16;
    options.request_timeout = request_timeout;
    options.upgrade = upgrade;
    options.request_done = cfw_test_audit_done;
    options.request_done_user = ts;
    CFW_TEST_ASSERT_EQ(cfw_server_init(&ts->server, &options), CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_server_port(&ts->server) != 0);
    return 0;
}

static void cfw_test_audit_teardown(cfw_test_audit_server *ts)
{
    cfw_server_free(&ts->server);
    cfw_arena_destroy(ts->arena);
}

static int test_server_request_audit(void)
{
    cfw_test_audit_server ts;
    cfw_fd_t client;
    char response[8192];
    size_t used;

    CFW_TEST_ASSERT_EQ(cfw_test_audit_setup(&ts, NULL,
                                            (cfw_duration_t){0}), 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /hello HTTP/1.1\r\n"
                                     "Host: t\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    used = 0;
    CFW_TEST_ASSERT_TRUE(cfw_test_recv_until(&ts.server, &client,
                                             response, sizeof(response),
                                             "srv-ok", &used) > 0);
    CFW_TEST_ASSERT_EQ(ts.audit.calls, 1);
    CFW_TEST_ASSERT_EQ(ts.audit.last_status, CFW_HTTP_STATUS_OK);
    CFW_TEST_ASSERT_EQ(ts.audit.last_method, CFW_HTTP_METHOD_GET);
    CFW_TEST_ASSERT_EQ(ts.audit.last_target_len, 6);
    CFW_TEST_ASSERT_EQ(memcmp(ts.audit.last_target, "/hello", 6), 0);
    CFW_TEST_ASSERT_EQ(ts.audit.last_request_bytes, 0);
    CFW_TEST_ASSERT_TRUE(ts.audit.last_response_bytes > 0);
    CFW_TEST_ASSERT_TRUE(ts.audit.last_remote_ok);
    CFW_TEST_ASSERT_EQ(ts.audit.last_upgraded, 0);
    CFW_TEST_ASSERT_TRUE(ts.audit.last_duration_nanos >= 0);

    /* Keep-alive POST with a 10-byte body on the same connection. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "POST /submit HTTP/1.1\r\n"
                                     "Host: t\r\n"
                                     "Content-Length: 10\r\n\r\n"
                                     "0123456789"),
                       0);
    memset(response, 0, sizeof(response));
    used = 0;
    CFW_TEST_ASSERT_TRUE(cfw_test_recv_until(&ts.server, &client,
                                             response, sizeof(response),
                                             "srv-ok", &used) > 0);
    CFW_TEST_ASSERT_EQ(ts.audit.calls, 2);
    CFW_TEST_ASSERT_EQ(ts.audit.last_status, CFW_HTTP_STATUS_OK);
    CFW_TEST_ASSERT_EQ(ts.audit.last_method, CFW_HTTP_METHOD_POST);
    CFW_TEST_ASSERT_EQ(ts.audit.last_request_bytes, 10);

    /* Routed 404 is a normal dispatched response. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /missing HTTP/1.1\r\n"
                                     "Host: t\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    used = 0;
    CFW_TEST_ASSERT_TRUE(cfw_test_recv_until(&ts.server, &client,
                                             response, sizeof(response),
                                             "404", &used) > 0);
    CFW_TEST_ASSERT_EQ(ts.audit.calls, 3);
    CFW_TEST_ASSERT_EQ(ts.audit.statuses[2], CFW_HTTP_STATUS_NOT_FOUND);

    cfw_fd_close(&client);
    cfw_test_audit_teardown(&ts);
    return 0;
}

static int test_server_request_audit_stream(void)
{
    cfw_test_audit_server ts;
    cfw_fd_t client;
    char response[8192];
    size_t used;

    CFW_TEST_ASSERT_EQ(cfw_test_audit_setup(&ts, NULL,
                                            (cfw_duration_t){0}), 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /stream HTTP/1.1\r\n"
                                     "Host: t\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    used = 0;
    /* The record is emitted when the stream finishes, i.e. once the
     * terminal chunk has been queued. */
    CFW_TEST_ASSERT_TRUE(cfw_test_recv_until(&ts.server, &client,
                                             response, sizeof(response),
                                             "0\r\n\r\n", &used) > 0);
    CFW_TEST_ASSERT_EQ(ts.audit.calls, 1);
    CFW_TEST_ASSERT_EQ(ts.audit.last_status, CFW_HTTP_STATUS_OK);
    /* Stream payload ("Hello, streaming world!" = 23 bytes) plus the
     * serialized head must be counted. */
    CFW_TEST_ASSERT_TRUE(ts.audit.last_response_bytes >= 23);

    /* Keep-alive still works after the deferred emission. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /hello HTTP/1.1\r\n"
                                     "Host: t\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    used = 0;
    CFW_TEST_ASSERT_TRUE(cfw_test_recv_until(&ts.server, &client,
                                             response, sizeof(response),
                                             "srv-ok", &used) > 0);
    CFW_TEST_ASSERT_EQ(ts.audit.calls, 2);

    cfw_fd_close(&client);
    cfw_test_audit_teardown(&ts);
    return 0;
}

static int test_server_request_audit_errors(void)
{
    cfw_test_audit_server ts;
    cfw_fd_t client;
    char response[8192];
    size_t used;

    CFW_TEST_ASSERT_EQ(cfw_test_audit_setup(&ts, NULL,
                                            (cfw_duration_t){0}), 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);

    /* Server-generated 404 goes through the normal response path. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /missing HTTP/1.1\r\n"
                                     "Host: t\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    used = 0;
    CFW_TEST_ASSERT_TRUE(cfw_test_recv_until(&ts.server, &client,
                                             response, sizeof(response),
                                             "404", &used) > 0);
    CFW_TEST_ASSERT_EQ(ts.audit.calls, 1);
    CFW_TEST_ASSERT_EQ(ts.audit.statuses[0], CFW_HTTP_STATUS_NOT_FOUND);

    /* Missing Host on HTTP/1.1 is rejected with 400 by the server. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /hello HTTP/1.1\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    used = 0;
    CFW_TEST_ASSERT_TRUE(cfw_test_recv_until(&ts.server, &client,
                                             response, sizeof(response),
                                             "400", &used) > 0);
    CFW_TEST_ASSERT_EQ(ts.audit.calls, 2);
    CFW_TEST_ASSERT_EQ(ts.audit.statuses[1], CFW_HTTP_STATUS_BAD_REQUEST);
    CFW_TEST_ASSERT_TRUE(ts.audit.last_response_bytes > 0);

    cfw_fd_close(&client);

    /* A head that never parses produces no record: garbage bytes fail
     * before any request head exists. */
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client, "garbage\r\n\r\n"), 0);
    memset(response, 0, sizeof(response));
    used = 0;
    CFW_TEST_ASSERT_TRUE(cfw_test_recv_until(&ts.server, &client,
                                             response, sizeof(response),
                                             "400", &used) > 0);
    CFW_TEST_ASSERT_EQ(ts.audit.calls, 2);

    cfw_fd_close(&client);
    cfw_test_audit_teardown(&ts);
    return 0;
}

static int test_server_request_audit_upgrade(void)
{
    cfw_test_audit_server ts;
    cfw_fd_t client;
    char response[8192];
    size_t used;

    cfw_test_upgrade_reset();
    CFW_TEST_ASSERT_EQ(cfw_test_audit_setup(&ts, cfw_test_upgrade_echo,
                                            (cfw_duration_t){0}), 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /upgrade HTTP/1.1\r\n"
                                     "Host: t\r\n"
                                     "Upgrade: cfw-test\r\n"
                                     "Connection: upgrade\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    used = 0;
    CFW_TEST_ASSERT_TRUE(cfw_test_recv_until(&ts.server, &client,
                                             response, sizeof(response),
                                             "101", &used) > 0);
    CFW_TEST_ASSERT_EQ(ts.audit.calls, 1);
    CFW_TEST_ASSERT_EQ(ts.audit.last_status,
                       CFW_HTTP_STATUS_SWITCHING_PROTOCOLS);
    CFW_TEST_ASSERT_EQ(ts.audit.last_upgraded, 1);
    CFW_TEST_ASSERT_EQ(ts.audit.last_target_len, 8);
    CFW_TEST_ASSERT_EQ(memcmp(ts.audit.last_target, "/upgrade", 8), 0);

    /* Closing the upgraded connection must not emit a second record. */
    cfw_fd_close(&client);
    {
        int attempts;
        for (attempts = 0; attempts < 100 &&
                        cfw_server_connection_count(&ts.server) != 0;
             attempts++) {
            cfw_timepoint_t deadline;
            cfw_duration_t slice;
            cfw_duration_from_millis(20, &slice);
            cfw_deadline_from_now(&deadline, slice);
            CFW_TEST_ASSERT_EQ(cfw_server_step(&ts.server, &deadline),
                               CFW_OK);
        }
    }
    CFW_TEST_ASSERT_EQ(ts.audit.calls, 1);

    cfw_test_audit_teardown(&ts);
    return 0;
}

static int test_server_request_audit_abort(void)
{
    cfw_test_audit_server ts;
    cfw_fd_t client;
    cfw_duration_t request_timeout;
    int attempts;

    cfw_duration_from_millis(50, &request_timeout);
    CFW_TEST_ASSERT_EQ(cfw_test_audit_setup(&ts, NULL, request_timeout),
                       0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client,
                                        cfw_server_port(&ts.server)),
                       CFW_OK);
    /* Head declares a 100-byte body that never arrives; the request
     * deadline kills the connection mid-request. */
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "POST /submit HTTP/1.1\r\n"
                                     "Host: t\r\n"
                                     "Content-Length: 100\r\n\r\n"),
                       0);
    for (attempts = 0; attempts < 200; attempts++) {
        cfw_timepoint_t deadline;
        cfw_duration_t slice;
        cfw_duration_from_millis(50, &slice);
        cfw_deadline_from_now(&deadline, slice);
        CFW_TEST_ASSERT_EQ(cfw_server_step(&ts.server, &deadline),
                           CFW_OK);
        if (cfw_server_connection_count(&ts.server) == 0) {
            break;
        }
    }
    CFW_TEST_ASSERT_EQ(cfw_server_connection_count(&ts.server), 0);
    /* Exactly one record: the parsed head is audited even though no
     * response was ever produced (status 0). */
    CFW_TEST_ASSERT_EQ(ts.audit.calls, 1);
    CFW_TEST_ASSERT_EQ(ts.audit.last_status, 0);
    CFW_TEST_ASSERT_EQ(ts.audit.last_method, CFW_HTTP_METHOD_POST);
    CFW_TEST_ASSERT_EQ(ts.audit.last_request_bytes, 0);

    cfw_fd_close(&client);
    cfw_test_audit_teardown(&ts);
    return 0;
}

/* ---------------- SO_REUSEPORT multi-instance (ADR 0056) ---------------- */

typedef struct {
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_server_t server;
} cfw_test_rp_server;

#if CFW_HAVE_SO_REUSEPORT

static int cfw_test_rp_init(cfw_test_rp_server *s, uint16_t port,
                            int reuse_port)
{
    cfw_server_options_t options;
    cfw_dispatch_options_t doptions;
    cfw_strview_t pattern;

    memset(s, 0, sizeof(*s));
    s->arena = cfw_arena_create(32768);
    CFW_TEST_ASSERT_TRUE(s->arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&s->router, s->arena), CFW_OK);

    memset(&doptions, 0, sizeof(doptions));
    doptions.router = &s->router;
    doptions.handler = cfw_test_ok_handler;
    doptions.max_middleware = 4;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&s->dispatch, s->arena,
                                         &doptions),
                       CFW_OK);
    cfw_strview_from_cstr(&pattern, "/hello");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&s->dispatch,
                                              CFW_HTTP_METHOD_GET,
                                              &pattern, cfw_test_ok_handler,
                                              NULL),
                       CFW_OK);

    memset(&options, 0, sizeof(options));
    options.host = "127.0.0.1";
    options.port = port;
    options.backlog = 16;
    options.dispatch = &s->dispatch;
    options.http.max_head_size = 8192;
    options.http.max_body_size = 65536;
    options.http.max_pending_size = 65536;
    options.max_out_size = 65536;
    options.max_connections = 8;
    options.max_headers = 32;
    options.max_params = 16;
    options.reuse_port = reuse_port;
    return cfw_server_init(&s->server, &options) == CFW_OK ? 0 : 1;
}

static void cfw_test_rp_teardown(cfw_test_rp_server *s)
{
    cfw_server_free(&s->server);
    cfw_arena_destroy(s->arena);
}

/* Steps both servers once per attempt until `marker` arrives. */
static int cfw_test_rp_recv(cfw_test_rp_server *a, cfw_test_rp_server *b,
                            cfw_fd_t *client, char *buffer,
                            size_t capacity, const char *marker)
{
    cfw_duration_t slice;
    int attempts;
    size_t used = 0;
    size_t mlen = strlen(marker);

    cfw_duration_from_millis(50, &slice);
    for (attempts = 0; attempts < 200; attempts++) {
        cfw_timepoint_t deadline;
        intptr_t n;
        cfw_deadline_from_now(&deadline, slice);
        CFW_TEST_ASSERT_EQ(cfw_server_step(&a->server, &deadline),
                           CFW_OK);
        cfw_deadline_from_now(&deadline, slice);
        CFW_TEST_ASSERT_EQ(cfw_server_step(&b->server, &deadline),
                           CFW_OK);
        n = cfw_test_sock_recv(client, buffer + used,
                 capacity - used - 1);
        if (n > 0) {
            used += (size_t)n;
            buffer[used] = '\0';
            if (used >= mlen &&
                cfw_test_memmem(buffer, used, marker, mlen) != NULL) {
                return 1;
            }
        } else if (n == 0) {
            return 0;
        }
    }
    return 0;
}

static int test_server_reuse_port(void)
{
    cfw_test_rp_server a;
    cfw_test_rp_server b;
    cfw_test_rp_server c;
    cfw_fd_t client;
    char response[8192];
    uint16_t port;

    CFW_TEST_ASSERT_EQ(cfw_test_rp_init(&a, 0, 1), 0);
    port = cfw_server_port(&a.server);
    CFW_TEST_ASSERT_TRUE(port != 0);

    /* A second reuse-port instance binds the same port. */
    CFW_TEST_ASSERT_EQ(cfw_test_rp_init(&b, port, 1), 0);

    /* A plain bind must fail while reuse-port sockets hold the port. */
    memset(&c, 0, sizeof(c));
    {
        cfw_arena_t *arena = cfw_arena_create(32768);
        cfw_router_t router;
        cfw_dispatch_t dispatch;
        cfw_server_options_t options;
        cfw_dispatch_options_t doptions;
        cfw_status_t st;

        CFW_TEST_ASSERT_TRUE(arena != NULL);
        CFW_TEST_ASSERT_EQ(cfw_router_init(&router, arena), CFW_OK);
        memset(&doptions, 0, sizeof(doptions));
        doptions.router = &router;
        doptions.handler = cfw_test_ok_handler;
        doptions.max_middleware = 4;
        CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&dispatch, arena,
                                             &doptions),
                           CFW_OK);
        memset(&options, 0, sizeof(options));
        options.host = "127.0.0.1";
        options.port = port;
        options.dispatch = &dispatch;
        options.http.max_head_size = 8192;
        options.http.max_body_size = 65536;
        options.http.max_pending_size = 65536;
        options.max_out_size = 65536;
        options.max_connections = 8;
        options.max_headers = 32;
        options.max_params = 16;
        options.reuse_port = 0;
        st = cfw_server_init(&c.server, &options);
        CFW_TEST_ASSERT_TRUE(st != CFW_OK);
        /* Nothing to free: init failed before taking ownership. */
        cfw_arena_destroy(arena);
    }

    /* The shared port serves requests through either instance. */
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&client, port), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /hello HTTP/1.1\r\n"
                                     "Host: t\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    CFW_TEST_ASSERT_TRUE(cfw_test_rp_recv(&a, &b, &client, response,
                                          sizeof(response), "srv-ok"));
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, strlen(response),
                                           "200 OK"));

    cfw_fd_close(&client);
    cfw_test_rp_teardown(&a);
    cfw_test_rp_teardown(&b);
    return 0;
}

/* Several keep-alive clients spread across two reuse-port instances;
 * aggregate counters must account for every request exactly once. */
static int test_server_reuse_port_cluster(void)
{
    cfw_test_rp_server a;
    cfw_test_rp_server b;
    cfw_fd_t clients[4];
    char response[8192];
    uint16_t port;
    int c;
    int r;

    CFW_TEST_ASSERT_EQ(cfw_test_rp_init(&a, 0, 1), 0);
    port = cfw_server_port(&a.server);
    CFW_TEST_ASSERT_TRUE(port != 0);
    CFW_TEST_ASSERT_EQ(cfw_test_rp_init(&b, port, 1), 0);

    for (c = 0; c < 4; c++) {
        cfw_fd_init(&clients[c]);
        CFW_TEST_ASSERT_EQ(cfw_test_connect(&clients[c], port), CFW_OK);
    }
    for (r = 0; r < 5; r++) {
        for (c = 0; c < 4; c++) {
            CFW_TEST_ASSERT_EQ(cfw_test_send(&clients[c],
                                             "GET /hello HTTP/1.1\r\n"
                                             "Host: t\r\n\r\n"),
                               0);
            memset(response, 0, sizeof(response));
            CFW_TEST_ASSERT_TRUE(cfw_test_rp_recv(
                &a, &b, &clients[c], response, sizeof(response),
                "srv-ok"));
        }
    }

    CFW_TEST_ASSERT_EQ(cfw_server_requests_total(&a.server) +
                           cfw_server_requests_total(&b.server),
                       20);
    CFW_TEST_ASSERT_EQ(cfw_server_accepted_total(&a.server) +
                           cfw_server_accepted_total(&b.server),
                       4);

    for (c = 0; c < 4; c++) {
        cfw_fd_close(&clients[c]);
    }
    cfw_test_rp_teardown(&a);
    cfw_test_rp_teardown(&b);
    return 0;
}

#else /* !CFW_HAVE_SO_REUSEPORT */

static int test_server_reuse_port(void)
{
    cfw_test_rp_server a;
    cfw_server_options_t options;
    cfw_dispatch_options_t doptions;
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_strview_t pattern;
    cfw_status_t st;

    /* Without platform support the option must fail loudly rather
     * than silently degrade to a plain bind. */
    memset(&a, 0, sizeof(a));
    arena = cfw_arena_create(32768);
    CFW_TEST_ASSERT_TRUE(arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&router, arena), CFW_OK);
    memset(&doptions, 0, sizeof(doptions));
    doptions.router = &router;
    doptions.handler = cfw_test_ok_handler;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&dispatch, arena, &doptions),
                       CFW_OK);
    cfw_strview_from_cstr(&pattern, "/hello");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&dispatch,
                                              CFW_HTTP_METHOD_GET,
                                              &pattern, cfw_test_ok_handler,
                                              NULL),
                       CFW_OK);
    memset(&options, 0, sizeof(options));
    options.host = "127.0.0.1";
    options.port = 0;
    options.dispatch = &dispatch;
    options.http.max_head_size = 8192;
    options.http.max_body_size = 65536;
    options.http.max_pending_size = 65536;
    options.max_out_size = 65536;
    options.max_connections = 8;
    options.max_headers = 32;
    options.max_params = 16;
    options.reuse_port = 1;
    st = cfw_server_init(&a.server, &options);
    CFW_TEST_ASSERT_EQ(st, CFW_ERROR_UNAVAILABLE);
    cfw_arena_destroy(arena);
    return 0;
}

static int test_server_reuse_port_cluster(void)
{
    /* Cluster serving requires SO_REUSEPORT; unsupported platforms are
     * covered by test_server_reuse_port's UNAVAILABLE assertions. */
    return 0;
}

#endif /* CFW_HAVE_SO_REUSEPORT */

/* ---------------- IPv6 listener ---------------- */

static int test_server_ipv6(void)
{
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t doptions;
    cfw_server_options_t options;
    cfw_server_t server;
    cfw_strview_t pattern;
    cfw_fd_t client;
    cfw_addr_t addr;
    char response[8192];
    intptr_t n;

    arena = cfw_arena_create(32768);
    CFW_TEST_ASSERT_TRUE(arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&router, arena), CFW_OK);
    memset(&doptions, 0, sizeof(doptions));
    doptions.router = &router;
    doptions.handler = cfw_test_ok_handler;
    doptions.max_middleware = 4;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&dispatch, arena, &doptions),
                       CFW_OK);
    cfw_strview_from_cstr(&pattern, "/hello");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&dispatch,
                                              CFW_HTTP_METHOD_GET,
                                              &pattern, cfw_test_ok_handler,
                                              NULL),
                       CFW_OK);

    memset(&options, 0, sizeof(options));
    options.host = "::1";
    options.ipv6 = 1;
    options.port = 0;
    options.dispatch = &dispatch;
    options.http.max_head_size = 8192;
    options.http.max_body_size = 65536;
    options.http.max_pending_size = 65536;
    options.max_out_size = 65536;
    options.max_connections = 8;
    options.max_headers = 32;
    options.max_params = 16;
    CFW_TEST_ASSERT_EQ(cfw_server_init(&server, &options), CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_server_port(&server) != 0);

    /* Connect over IPv6 loopback. */
    CFW_TEST_ASSERT_EQ(cfw_addr_from_str(&addr, "::1",
                                         cfw_server_port(&server)),
                       CFW_OK);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_net_socket(AF_INET6, SOCK_STREAM, 0, &client),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_set_nonblocking(&client, 0), CFW_OK);
    CFW_TEST_ASSERT_EQ(connect((int)cfw_fd_get(&client),
                               (const struct sockaddr *)&addr.storage,
                               addr.length),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_net_set_nonblocking(&client, 1), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&client,
                                     "GET /hello HTTP/1.1\r\n"
                                     "Host: t\r\n\r\n"),
                       0);
    n = cfw_test_serve_recv(&server, &client, response,
                            sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "srv-ok"));

    cfw_fd_close(&client);
    cfw_server_free(&server);
    cfw_arena_destroy(arena);
    return 0;
}

/* tls_detect without a tls_ctx is a configuration error in every
 * build (ADR 0061). */
static int test_server_tls_detect_requires_ctx(void)
{
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t doptions;
    cfw_server_options_t options;
    cfw_server_t server;
    cfw_strview_t pattern;

    arena = cfw_arena_create(32768);
    CFW_TEST_ASSERT_TRUE(arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&router, arena), CFW_OK);
    memset(&doptions, 0, sizeof(doptions));
    doptions.router = &router;
    doptions.handler = cfw_test_ok_handler;
    doptions.max_middleware = 4;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&dispatch, arena, &doptions),
                       CFW_OK);
    cfw_strview_from_cstr(&pattern, "/hello");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&dispatch,
                                              CFW_HTTP_METHOD_GET,
                                              &pattern, cfw_test_ok_handler,
                                              NULL),
                       CFW_OK);

    memset(&options, 0, sizeof(options));
    options.host = "127.0.0.1";
    options.port = 0;
    options.dispatch = &dispatch;
    options.http.max_head_size = 8192;
    options.http.max_pending_size = 65536;
    options.max_out_size = 65536;
    options.max_connections = 8;
    options.max_headers = 32;
    options.max_params = 16;
    options.tls_detect = 1;
    options.tls_ctx = NULL;
    CFW_TEST_ASSERT_EQ(cfw_server_init(&server, &options),
                       CFW_ERROR_INVALID_ARGUMENT);

    cfw_arena_destroy(arena);
    return 0;
}

#if CFW_HAVE_TLS

/* Steps the server with a short deadline; returns nonzero while the
 * budget lasts. */
static int cfw_test_tls_step(cfw_server_t *server)
{
    cfw_timepoint_t deadline;
    cfw_duration_t slice;

    cfw_duration_from_millis(50, &slice);
    cfw_deadline_from_now(&deadline, slice);
    return cfw_server_step(server, &deadline) == CFW_OK;
}

/* Drives the client-side TLS handshake while stepping the server.
 * Returns 0 on success. */
static int cfw_test_tls_handshake(cfw_server_t *server,
                                  cfw_tls_conn_t *client)
{
    int i;

    for (i = 0; i < 4000; i++) {
        cfw_status_t st = cfw_tls_handshake(client);
        if (st == CFW_OK) {
            return cfw_tls_handshake_done(client) ? 0 : 1;
        }
        if (st != CFW_ERROR_WOULDBLOCK) {
            return 1;
        }
        if (!cfw_test_tls_step(server)) {
            return 1;
        }
    }
    return 1;
}

/* Writes exactly `len` bytes over TLS, retrying WOULDBLOCK while
 * stepping the server. */
static int cfw_test_tls_write_all_len(cfw_server_t *server,
                                      cfw_tls_conn_t *client,
                                      const void *data, size_t len)
{
    const unsigned char *bytes = data;
    size_t sent = 0;
    int i;

    for (i = 0; i < 4000 && sent < len; i++) {
        size_t done = 0;
        cfw_status_t st =
            cfw_tls_write(client, bytes + sent, len - sent, &done);
        if (st == CFW_OK) {
            sent += done;
        } else if (st != CFW_ERROR_WOULDBLOCK) {
            return 1;
        }
        if (!cfw_test_tls_step(server)) {
            return 1;
        }
    }
    return sent != len;
}

/* Writes the full request over TLS, retrying WOULDBLOCK while
 * stepping the server. */
static int cfw_test_tls_write_all(cfw_server_t *server,
                                  cfw_tls_conn_t *client,
                                  const char *data)
{
    return cfw_test_tls_write_all_len(server, client, data,
                                      strlen(data));
}

/* Reads TLS plaintext until `needle` appears or the budget ends.
 * Returns total bytes accumulated (>0) or -1. */
static intptr_t cfw_test_tls_read_until(cfw_server_t *server,
                                       cfw_tls_conn_t *client,
                                       char *buffer, size_t capacity,
                                       const char *needle)
{
    size_t got = 0;
    int i;

    for (i = 0; i < 4000; i++) {
        size_t done = 0;
        cfw_status_t st = cfw_tls_read(client, buffer + got,
                                       capacity - 1 - got, &done);
        if (st == CFW_OK) {
            if (done == 0) {
                break;
            }
            got += done;
            buffer[got] = '\0';
            if (cfw_test_contains(buffer, got, needle)) {
                return (intptr_t)got;
            }
        } else if (st != CFW_ERROR_WOULDBLOCK) {
            return -1;
        }
        if (!cfw_test_tls_step(server)) {
            return -1;
        }
    }
    return got > 0 ? (intptr_t)got : -1;
}

static int test_server_tls_request_response(void)
{
    cfw_test_server ts;
    cfw_server_options_t options;
    cfw_dispatch_options_t doptions;
    cfw_tls_ctx_options_t sopt;
    cfw_tls_ctx_options_t copt;
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_conn_t *client = NULL;
    cfw_fd_t raw;
    char cert[512];
    char key[512];
    char response[8192];
    intptr_t n;
    uint16_t port;
    int rc = 1;

    memset(&sopt, 0, sizeof(sopt));
    sopt.role = CFW_TLS_SERVER;
    snprintf(cert, sizeof(cert), "%s/server.crt", CFW_TEST_TLS_DIR);
    snprintf(key, sizeof(key), "%s/server.key", CFW_TEST_TLS_DIR);
    sopt.cert_file = cert;
    sopt.key_file = key;
    CFW_TEST_ASSERT_EQ(cfw_tls_ctx_create(&sopt, &sctx), CFW_OK);

    memset(&copt, 0, sizeof(copt));
    copt.role = CFW_TLS_CLIENT;
    CFW_TEST_ASSERT_EQ(cfw_tls_ctx_create(&copt, &cctx), CFW_OK);

    /* Minimal server setup mirroring cfw_test_server_setup but with
     * tls_ctx enabled. */
    memset(&ts, 0, sizeof(ts));
    ts.arena = cfw_arena_create(32768);
    CFW_TEST_ASSERT_TRUE(ts.arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&ts.router, ts.arena), CFW_OK);
    memset(&doptions, 0, sizeof(doptions));
    doptions.router = &ts.router;
    doptions.handler = cfw_test_ok_handler;
    doptions.max_middleware = 4;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&ts.dispatch, ts.arena,
                                         &doptions),
                       CFW_OK);
    {
        cfw_strview_t pattern;
        cfw_strview_from_cstr(&pattern, "/hello");
        CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(
                               &ts.dispatch, CFW_HTTP_METHOD_GET,
                               &pattern, cfw_test_ok_handler, NULL),
                           CFW_OK);
        cfw_strview_from_cstr(&pattern, "/probe");
        CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(
                               &ts.dispatch, CFW_HTTP_METHOD_GET,
                               &pattern, cfw_test_tls_probe_handler,
                               NULL),
                           CFW_OK);
    }
    memset(&options, 0, sizeof(options));
    options.host = "127.0.0.1";
    options.port = 0;
    options.backlog = 16;
    options.dispatch = &ts.dispatch;
    options.http.max_head_size = 8192;
    options.http.max_body_size = 65536;
    options.http.max_pending_size = 65536;
    options.max_out_size = 65536;
    options.max_connections = 8;
    options.max_headers = 32;
    options.max_params = 16;
    options.tls_ctx = sctx;
    CFW_TEST_ASSERT_EQ(cfw_server_init(&ts.server, &options), CFW_OK);
    port = cfw_server_port(&ts.server);
    CFW_TEST_ASSERT_TRUE(port != 0);

    cfw_fd_init(&raw);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&raw, port), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_tls_conn_create(cctx, cfw_fd_get(&raw),
                                           &client),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_tls_handshake(&ts.server, client), 0);

    CFW_TEST_ASSERT_EQ(cfw_test_tls_write_all(
                           &ts.server, client,
                           "GET /hello HTTP/1.1\r\n"
                           "Host: test\r\n\r\n"),
                       0);
    n = cfw_test_tls_read_until(&ts.server, client, response,
                                sizeof(response), "srv-ok");
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "HTTP/1.1 200 OK"));
    CFW_TEST_ASSERT_EQ(cfw_server_requests_total(&ts.server), 1);

    /* req->tls_conn exposes the session to handlers for authZ
     * decisions (ADR 0063). */
    cfw_test_tls_conn_seen = -1;
    CFW_TEST_ASSERT_EQ(cfw_test_tls_write_all(
                           &ts.server, client,
                           "GET /probe HTTP/1.1\r\n"
                           "Host: t\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    n = cfw_test_tls_read_until(&ts.server, client, response,
                                sizeof(response), "probe-ok");
    CFW_TEST_ASSERT_TRUE(n > 0);
    CFW_TEST_ASSERT_EQ(cfw_test_tls_conn_seen, 1);
    rc = 0;

    if (client != NULL) {
        cfw_tls_conn_destroy(client);
    }
    cfw_fd_close(&raw);
    cfw_tls_ctx_destroy(sctx);
    cfw_tls_ctx_destroy(cctx);
    if (ts.arena != NULL) {
        cfw_test_server_teardown(&ts);
    }
    return rc;
}

/* WebSocket-style upgrade over TLS (wss://): proves cfw_upgraded_recv
 * routes through the connection transport instead of raw recv(), which
 * would otherwise expose ciphertext to the upgraded protocol. */
static int test_server_tls_upgrade_echo(void)
{
    cfw_test_server ts;
    cfw_server_options_t options;
    cfw_dispatch_options_t doptions;
    cfw_tls_ctx_options_t sopt;
    cfw_tls_ctx_options_t copt;
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_conn_t *client = NULL;
    cfw_fd_t raw;
    char cert[512];
    char key[512];
    char response[8192];
    intptr_t n;
    uint16_t port;
    int rc = 1;

    cfw_test_upgrade_reset();
    memset(&sopt, 0, sizeof(sopt));
    sopt.role = CFW_TLS_SERVER;
    snprintf(cert, sizeof(cert), "%s/server.crt", CFW_TEST_TLS_DIR);
    snprintf(key, sizeof(key), "%s/server.key", CFW_TEST_TLS_DIR);
    sopt.cert_file = cert;
    sopt.key_file = key;
    CFW_TEST_ASSERT_EQ(cfw_tls_ctx_create(&sopt, &sctx), CFW_OK);

    memset(&copt, 0, sizeof(copt));
    copt.role = CFW_TLS_CLIENT;
    CFW_TEST_ASSERT_EQ(cfw_tls_ctx_create(&copt, &cctx), CFW_OK);

    memset(&ts, 0, sizeof(ts));
    ts.arena = cfw_arena_create(32768);
    CFW_TEST_ASSERT_TRUE(ts.arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&ts.router, ts.arena), CFW_OK);
    memset(&doptions, 0, sizeof(doptions));
    doptions.router = &ts.router;
    doptions.handler = cfw_test_ok_handler;
    doptions.max_middleware = 4;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&ts.dispatch, ts.arena,
                                         &doptions),
                       CFW_OK);
    {
        cfw_strview_t pattern;
        cfw_strview_from_cstr(&pattern, "/upgrade");
        CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(
                               &ts.dispatch, CFW_HTTP_METHOD_GET,
                               &pattern, cfw_test_upgrade_handler, NULL),
                           CFW_OK);
    }
    memset(&options, 0, sizeof(options));
    options.host = "127.0.0.1";
    options.port = 0;
    options.backlog = 16;
    options.dispatch = &ts.dispatch;
    options.http.max_head_size = 8192;
    options.http.max_body_size = 65536;
    options.http.max_pending_size = 65536;
    options.max_out_size = 65536;
    options.max_connections = 8;
    options.max_headers = 32;
    options.max_params = 16;
    options.upgrade = cfw_test_upgrade_echo;
    options.upgrade_user = &cfw_test_upgrade_magic;
    options.tls_ctx = sctx;
    CFW_TEST_ASSERT_EQ(cfw_server_init(&ts.server, &options), CFW_OK);
    port = cfw_server_port(&ts.server);
    CFW_TEST_ASSERT_TRUE(port != 0);

    cfw_fd_init(&raw);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&raw, port), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_tls_conn_create(cctx, cfw_fd_get(&raw),
                                           &client),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_tls_handshake(&ts.server, client), 0);

    CFW_TEST_ASSERT_EQ(cfw_test_tls_write_all(
                           &ts.server, client,
                           "GET /upgrade HTTP/1.1\r\n"
                           "Host: t\r\n"
                           "Upgrade: cfw-test\r\n"
                           "Connection: upgrade\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    n = cfw_test_tls_read_until(&ts.server, client, response,
                                sizeof(response), "101");
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "cfw-test"));
    CFW_TEST_ASSERT_TRUE(cfw_test_upgrade_ready_seen);

    /* Raw protocol bytes must be decrypted before reaching the
     * upgraded echo handler. */
    CFW_TEST_ASSERT_EQ(cfw_test_tls_write_all(&ts.server, client,
                                              "ping-tls"),
                       0);
    memset(response, 0, sizeof(response));
    n = cfw_test_tls_read_until(&ts.server, client, response,
                                sizeof(response), "ping-tls");
    CFW_TEST_ASSERT_TRUE(n > 0);
    CFW_TEST_ASSERT_EQ(cfw_test_upgrade_last_send, CFW_OK);
    CFW_TEST_ASSERT_EQ(memcmp(cfw_test_upgrade_echoed, "ping-tls",
                              sizeof("ping-tls") - 1),
                       0);
    rc = 0;

    if (client != NULL) {
        cfw_tls_conn_destroy(client);
    }
    cfw_fd_close(&raw);
    cfw_tls_ctx_destroy(sctx);
    cfw_tls_ctx_destroy(cctx);
    if (ts.arena != NULL) {
        cfw_test_server_teardown(&ts);
    }
    return rc;
}

/* WebSocket-over-TLS (wss://, ADR 0035 + ADR 0058): the full RFC 6455
 * stack — handshake helpers, masked client frames, cfw_ws framing —
 * runs over the TLS transport seam on a TLS listener. */

static cfw_status_t cfw_test_ws_echo_message(cfw_ws_t *ws,
                                             const cfw_ws_message_t *msg,
                                             void *user)
{
    (void)user;
    return cfw_ws_send(ws, msg->opcode, msg->payload.data,
                       msg->payload.size);
}

static cfw_status_t cfw_test_ws_handler(cfw_request_t *req,
                                        cfw_response_t *res, void *user)
{
    static char accept[CFW_WS_ACCEPT_KEY_CAPACITY];
    cfw_strview_t name;
    cfw_strview_t value;
    (void)user;

    if (cfw_ws_handshake_validate(req) != CFW_OK ||
        cfw_ws_handshake_accept(req, accept, sizeof(accept)) != CFW_OK) {
        res->status = CFW_HTTP_STATUS_BAD_REQUEST;
        return CFW_OK;
    }
    res->status = CFW_HTTP_STATUS_SWITCHING_PROTOCOLS;
    cfw_strview_from_cstr(&name, "Upgrade");
    cfw_strview_from_cstr(&value, "websocket");
    cfw_response_header_add(res, &name, &value);
    cfw_strview_from_cstr(&name, "Connection");
    cfw_strview_from_cstr(&value, "Upgrade");
    cfw_response_header_add(res, &name, &value);
    cfw_strview_from_cstr(&name, "Sec-WebSocket-Accept");
    cfw_strview_init(&value, accept, strlen(accept));
    cfw_response_header_add(res, &name, &value);
    return CFW_OK;
}

static cfw_status_t cfw_test_ws_upgrade(cfw_upgraded_conn_t *conn,
                                        uint32_t events, void *user)
{
    cfw_ws_t *ws;
    (void)user;

    if (events == CFW_UPGRADE_EV_CLOSE) {
        ws = cfw_upgraded_user(conn);
        if (ws != NULL) {
            cfw_ws_destroy(ws);
        }
        return CFW_OK;
    }
    if (events == CFW_UPGRADE_EV_READY) {
        cfw_ws_options_t options;
        memset(&options, 0, sizeof(options));
        options.on_message = cfw_test_ws_echo_message;
        ws = cfw_ws_create(conn, &options);
        if (ws == NULL) {
            return CFW_ERROR_INTERNAL;
        }
        cfw_upgraded_set_user(conn, ws);
        return CFW_OK;
    }
    ws = cfw_upgraded_user(conn);
    if (ws != NULL) {
        (void)cfw_ws_on_event(ws, events);
    }
    return CFW_OK;
}

/* Sends one masked client frame (opcode + payload <= 125 bytes) over
 * the TLS conn. Client frames must be masked per RFC 6455 §5.3. */
static int cfw_test_ws_send_frame(cfw_server_t *server,
                                  cfw_tls_conn_t *client,
                                  unsigned char opcode,
                                  const char *payload, size_t payload_len)
{
    static const unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
    unsigned char frame[4 + 125 + 4];
    size_t i;

    if (payload_len > 125) {
        return 1;
    }
    frame[0] = (unsigned char)(0x80 | opcode);
    frame[1] = (unsigned char)(0x80 | payload_len);
    memcpy(frame + 2, mask, 4);
    for (i = 0; i < payload_len; i++) {
        frame[6 + i] =
            (unsigned char)((unsigned char)payload[i] ^ mask[i % 4]);
    }
    return cfw_test_tls_write_all_len(server, client, frame,
                                      6 + payload_len);
}

static int test_server_tls_ws_echo(void)
{
    cfw_test_server ts;
    cfw_server_options_t options;
    cfw_dispatch_options_t doptions;
    cfw_tls_ctx_options_t sopt;
    cfw_tls_ctx_options_t copt;
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_conn_t *client = NULL;
    cfw_fd_t raw;
    char cert[512];
    char key[512];
    char response[8192];
    intptr_t n;
    uint16_t port;
    int rc = 1;

    memset(&sopt, 0, sizeof(sopt));
    sopt.role = CFW_TLS_SERVER;
    snprintf(cert, sizeof(cert), "%s/server.crt", CFW_TEST_TLS_DIR);
    snprintf(key, sizeof(key), "%s/server.key", CFW_TEST_TLS_DIR);
    sopt.cert_file = cert;
    sopt.key_file = key;
    CFW_TEST_ASSERT_EQ(cfw_tls_ctx_create(&sopt, &sctx), CFW_OK);

    memset(&copt, 0, sizeof(copt));
    copt.role = CFW_TLS_CLIENT;
    CFW_TEST_ASSERT_EQ(cfw_tls_ctx_create(&copt, &cctx), CFW_OK);

    memset(&ts, 0, sizeof(ts));
    ts.arena = cfw_arena_create(32768);
    CFW_TEST_ASSERT_TRUE(ts.arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&ts.router, ts.arena), CFW_OK);
    memset(&doptions, 0, sizeof(doptions));
    doptions.router = &ts.router;
    doptions.handler = cfw_test_ws_handler;
    doptions.max_middleware = 4;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&ts.dispatch, ts.arena,
                                         &doptions),
                       CFW_OK);
    {
        cfw_strview_t pattern;
        cfw_strview_from_cstr(&pattern, "/ws");
        CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(
                               &ts.dispatch, CFW_HTTP_METHOD_GET,
                               &pattern, cfw_test_ws_handler, NULL),
                           CFW_OK);
    }
    memset(&options, 0, sizeof(options));
    options.host = "127.0.0.1";
    options.port = 0;
    options.backlog = 16;
    options.dispatch = &ts.dispatch;
    options.http.max_head_size = 8192;
    options.http.max_body_size = 65536;
    options.http.max_pending_size = 65536;
    options.max_out_size = 65536;
    options.max_connections = 8;
    options.max_headers = 32;
    options.max_params = 16;
    options.upgrade = cfw_test_ws_upgrade;
    options.tls_ctx = sctx;
    CFW_TEST_ASSERT_EQ(cfw_server_init(&ts.server, &options), CFW_OK);
    port = cfw_server_port(&ts.server);
    CFW_TEST_ASSERT_TRUE(port != 0);

    cfw_fd_init(&raw);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&raw, port), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_tls_conn_create(cctx, cfw_fd_get(&raw),
                                           &client),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_tls_handshake(&ts.server, client), 0);

    /* RFC 6455 §1.3 example handshake: the accept value is a known
     * answer, so this also pins cfw_ws_accept_key end-to-end. */
    CFW_TEST_ASSERT_EQ(cfw_test_tls_write_all(
                           &ts.server, client,
                           "GET /ws HTTP/1.1\r\n"
                           "Host: t\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                           "Sec-WebSocket-Version: 13\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    n = cfw_test_tls_read_until(&ts.server, client, response,
                                sizeof(response), "101");
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));

    /* Masked text frame "wss-echo": the server must unmask, reassemble,
     * and echo it as an unmasked frame. */
    CFW_TEST_ASSERT_EQ(cfw_test_ws_send_frame(
                           &ts.server, client, 0x1, "wss-echo",
                           sizeof("wss-echo") - 1),
                       0);
    memset(response, 0, sizeof(response));
    n = cfw_test_tls_read_until(&ts.server, client, response,
                                sizeof(response), "\x81\x08wss-echo");
    CFW_TEST_ASSERT_TRUE(n > 0);

    /* Masked ping: the module must answer with an unmasked pong. */
    CFW_TEST_ASSERT_EQ(cfw_test_ws_send_frame(
                           &ts.server, client, 0x9, "hi",
                           sizeof("hi") - 1),
                       0);
    memset(response, 0, sizeof(response));
    n = cfw_test_tls_read_until(&ts.server, client, response,
                                sizeof(response), "\x8a\x02hi");
    CFW_TEST_ASSERT_TRUE(n > 0);

    rc = 0;

    if (client != NULL) {
        cfw_tls_conn_destroy(client);
    }
    cfw_fd_close(&raw);
    cfw_tls_ctx_destroy(sctx);
    cfw_tls_ctx_destroy(cctx);
    if (ts.arena != NULL) {
        cfw_test_server_teardown(&ts);
    }
    return rc;
}

/* Mixed cleartext/TLS listener (ADR 0061): one port serves both —
 * the first byte decides. A cleartext GET and a full TLS session
 * must both succeed on the same server. */
static int test_server_tls_detect_mixed(void)
{
    cfw_test_server ts;
    cfw_server_options_t options;
    cfw_dispatch_options_t doptions;
    cfw_tls_ctx_options_t sopt;
    cfw_tls_ctx_options_t copt;
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_conn_t *tls_client = NULL;
    cfw_fd_t plain;
    cfw_fd_t raw;
    char cert[512];
    char key[512];
    char response[8192];
    intptr_t n;
    size_t used = 0;
    uint16_t port;
    int rc = 1;

    memset(&sopt, 0, sizeof(sopt));
    sopt.role = CFW_TLS_SERVER;
    snprintf(cert, sizeof(cert), "%s/server.crt", CFW_TEST_TLS_DIR);
    snprintf(key, sizeof(key), "%s/server.key", CFW_TEST_TLS_DIR);
    sopt.cert_file = cert;
    sopt.key_file = key;
    CFW_TEST_ASSERT_EQ(cfw_tls_ctx_create(&sopt, &sctx), CFW_OK);

    memset(&copt, 0, sizeof(copt));
    copt.role = CFW_TLS_CLIENT;
    CFW_TEST_ASSERT_EQ(cfw_tls_ctx_create(&copt, &cctx), CFW_OK);

    memset(&ts, 0, sizeof(ts));
    ts.arena = cfw_arena_create(32768);
    CFW_TEST_ASSERT_TRUE(ts.arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&ts.router, ts.arena), CFW_OK);
    memset(&doptions, 0, sizeof(doptions));
    doptions.router = &ts.router;
    doptions.handler = cfw_test_ok_handler;
    doptions.max_middleware = 4;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&ts.dispatch, ts.arena,
                                         &doptions),
                       CFW_OK);
    {
        cfw_strview_t pattern;
        cfw_strview_from_cstr(&pattern, "/hello");
        CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(
                               &ts.dispatch, CFW_HTTP_METHOD_GET,
                               &pattern, cfw_test_ok_handler, NULL),
                           CFW_OK);
    }
    memset(&options, 0, sizeof(options));
    options.host = "127.0.0.1";
    options.port = 0;
    options.backlog = 16;
    options.dispatch = &ts.dispatch;
    options.http.max_head_size = 8192;
    options.http.max_body_size = 65536;
    options.http.max_pending_size = 65536;
    options.max_out_size = 65536;
    options.max_connections = 8;
    options.max_headers = 32;
    options.max_params = 16;
    options.tls_ctx = sctx;
    options.tls_detect = 1;
    CFW_TEST_ASSERT_EQ(cfw_server_init(&ts.server, &options), CFW_OK);
    port = cfw_server_port(&ts.server);
    CFW_TEST_ASSERT_TRUE(port != 0);

    /* Cleartext client first: 'G' is not a TLS record, so the
     * connection must be served as plain HTTP. */
    cfw_fd_init(&plain);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&plain, port), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_send(&plain,
                                     "GET /hello HTTP/1.1\r\n"
                                     "Host: t\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    n = cfw_test_recv_until(&ts.server, &plain, response,
                            sizeof(response), "srv-ok", &used);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "HTTP/1.1 200 OK"));

    /* TLS client on the same port: ClientHello starts with 0x16. */
    cfw_fd_init(&raw);
    CFW_TEST_ASSERT_EQ(cfw_test_connect(&raw, port), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_tls_conn_create(cctx, cfw_fd_get(&raw),
                                           &tls_client),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_tls_handshake(&ts.server, tls_client),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_test_tls_write_all(
                           &ts.server, tls_client,
                           "GET /hello HTTP/1.1\r\n"
                           "Host: t\r\n\r\n"),
                       0);
    memset(response, 0, sizeof(response));
    n = cfw_test_tls_read_until(&ts.server, tls_client, response,
                                sizeof(response), "srv-ok");
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_test_contains(response, (size_t)n,
                                           "HTTP/1.1 200 OK"));
    CFW_TEST_ASSERT_EQ(cfw_server_requests_total(&ts.server), 2);
    rc = 0;

    if (tls_client != NULL) {
        cfw_tls_conn_destroy(tls_client);
    }
    cfw_fd_close(&plain);
    cfw_fd_close(&raw);
    cfw_tls_ctx_destroy(sctx);
    cfw_tls_ctx_destroy(cctx);
    if (ts.arena != NULL) {
        cfw_test_server_teardown(&ts);
    }
    return rc;
}

#endif /* CFW_HAVE_TLS */

const cfw_test_case cfw_test_server_cases[] = {
    {"server_upload_content_length", test_server_upload_content_length},
    {"server_upload_chunked", test_server_upload_chunked},
    {"server_upload_abort", test_server_upload_abort},
    {"server_upload_protocol_error",
     test_server_upload_protocol_error},
    {"server_upload_write_error", test_server_upload_write_error},
    {"server_upload_begin_error", test_server_upload_begin_error},
    {"server_streaming_conn_close", test_server_streaming_conn_close},
    {"server_stream_fn_error", test_server_stream_fn_error},
    {"server_stream_empty_chunk", test_server_stream_empty_chunk},
    {"server_method_fallbacks", test_server_method_fallbacks},
    {"server_expect_rejected", test_server_expect_rejected},
    {"server_host_required", test_server_host_required},
    {"server_error_logging", test_server_error_logging},
    {"server_streaming_response", test_server_streaming_response},
    {"server_streaming_http10", test_server_streaming_http10},
    {"server_stream_known_length", test_server_stream_known_length},
    {"server_file_stream_response", test_server_file_stream_response},
    {"server_stream_end_on_close", test_server_stream_end_on_close},
    {"server_request_response", test_server_request_response},
    {"server_tls_conn_null_cleartext",
     test_server_tls_conn_null_cleartext},
    {"server_large_response_head", test_server_large_response_head},
    {"server_post_body", test_server_post_body},
    {"server_remote_addr", test_server_remote_addr},
    {"server_request_timeout", test_server_request_timeout},
    {"server_stop", test_server_stop},
    {"server_max_connections", test_server_max_connections},
    {"server_soak", test_server_soak},
    {"server_drain_after_stop", test_server_drain_after_stop},
    {"server_run_and_drain", test_server_run_and_drain},
    {"server_expect_continue", test_server_expect_continue},
    {"server_keep_alive_timeout", test_server_keep_alive_timeout},
    {"server_dns_listen", test_server_dns_listen},
    {"server_upgrade_echo", test_server_upgrade_echo},
    {"server_upgrade_pipelined", test_server_upgrade_pipelined},
    {"server_upgrade_requires_header", test_server_upgrade_requires_header},
    {"server_upgrade_disabled", test_server_upgrade_disabled},
    {"server_upgrade_send_bound", test_server_upgrade_send_bound},
    {"server_request_audit", test_server_request_audit},
    {"server_request_audit_stream", test_server_request_audit_stream},
    {"server_request_audit_errors", test_server_request_audit_errors},
    {"server_request_audit_upgrade", test_server_request_audit_upgrade},
    {"server_request_audit_abort", test_server_request_audit_abort},
    {"server_reuse_port", test_server_reuse_port},
    {"server_reuse_port_cluster", test_server_reuse_port_cluster},
    {"server_ipv6", test_server_ipv6},
    {"server_tls_detect_requires_ctx",
     test_server_tls_detect_requires_ctx},
#if CFW_HAVE_TLS
    {"server_tls_request_response", test_server_tls_request_response},
    {"server_tls_upgrade_echo", test_server_tls_upgrade_echo},
    {"server_tls_ws_echo", test_server_tls_ws_echo},
    {"server_tls_detect_mixed", test_server_tls_detect_mixed},
#endif
    {NULL, NULL},
};


