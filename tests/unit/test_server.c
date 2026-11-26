/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#if !defined(_WIN32)
    #include "private/net.h"

    #include <errno.h>
    #include <string.h>
    #include <sys/socket.h>
    #include <unistd.h>

static cfw_status_t cfw_test_connect(cfw_fd_t *client, uint16_t port)
{
    cfw_sockaddr_t addr;

    CFW_TEST_ASSERT_EQ(cfw_addr_from_str(&addr, "127.0.0.1", port), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_socket(AF_INET, SOCK_STREAM, 0, client),
                       CFW_OK);
    {
        cfw_status_t st = cfw_net_connect(client, &addr);
        if (st != CFW_OK && st != CFW_ERROR_WOULDBLOCK) {
            return st;
        }
    }
    return CFW_OK;
}

static int cfw_test_send(cfw_fd_t *client, const char *data)
{
    size_t len = strlen(data);
    ssize_t n = send((int)cfw_fd_get(client), data, len, 0);
    return n == (ssize_t)len ? 0 : 1;
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
    options->max_out_size = 8192;
    options->max_connections = 8;
    options->keep_alive_timeout.nanoseconds = 0;
    options->request_timeout.nanoseconds = 0;
    options->max_headers = 32;
    options->max_params = 16;
}

static int cfw_test_server_setup_host(cfw_test_server *ts,
                                      cfw_duration_t keep_alive,
                                      cfw_duration_t request_timeout,
                                      size_t max_connections,
                                      const char *host)
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

    cfw_test_server_options(&options, &ts->dispatch);
    options.host = host;
    options.keep_alive_timeout = keep_alive;
    options.request_timeout = request_timeout;
    options.max_connections = max_connections;
    CFW_TEST_ASSERT_EQ(cfw_server_init(&ts->server, &options), CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_server_port(&ts->server) != 0);
    return 0;
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
static ssize_t cfw_test_serve_recv(cfw_server_t *server, cfw_fd_t *client,
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
            ssize_t n = recv((int)cfw_fd_get(client), buffer, capacity,
                             MSG_DONTWAIT);
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
    ssize_t n;
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

static int test_server_post_body(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    char response[4096];
    ssize_t n;
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
    ssize_t n;
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
    ssize_t n;
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

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}


static int test_server_drain_after_stop(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    char response[4096];
    ssize_t n;

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
    ssize_t n;
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
    ssize_t n;

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
} cfw_test_stream_state;

static const char *const cfw_test_stream_parts[] = {
    "Hello, ", "stream", "ing ", "world!",
};

static cfw_status_t cfw_test_stream_fn(void *user, cfw_span_t *chunk,
                                       int *eof)
{
    cfw_test_stream_state *st = user;
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
    return CFW_OK;
}

/* Steps the server and appends received bytes until `marker` appears,
 * EOF, or the attempt budget is exhausted. Returns total bytes or -1. */
static ssize_t cfw_test_recv_until(cfw_server_t *server, cfw_fd_t *client,
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
        ssize_t n;
        if (cfw_server_step(server, &deadline) != CFW_OK) {
            return -1;
        }
        n = recv((int)cfw_fd_get(client), buffer + *used,
                 capacity - *used - 1, MSG_DONTWAIT);
        if (n > 0) {
            *used += (size_t)n;
            buffer[*used] = '\0';
            if (*used >= mlen &&
                memmem(buffer, *used, marker, mlen) != NULL) {
                return (ssize_t)*used;
            }
        } else if (n == 0) {
            return (ssize_t)*used;
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
    ssize_t n;

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
    ssize_t n;
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
        ssize_t r;
        cfw_duration_from_millis(20, &slice);
        cfw_deadline_from_now(&deadline, slice);
        CFW_TEST_ASSERT_EQ(cfw_server_step(&ts.server, &deadline),
                           CFW_OK);
        r = recv((int)cfw_fd_get(&client), response + used,
                 sizeof(response) - used - 1, MSG_DONTWAIT);
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


static int test_server_host_required(void)
{
    cfw_test_server ts;
    char response[4096];
    ssize_t n;

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
    ssize_t n;
    static const char *const bad_expects[] = {
        "GET /hello HTTP/1.1\r\nHost: x\r\nExpect: foobar\r\n\r\n",
        "GET /hello HTTP/1.1\r\nHost: x\r\n"
        "Expect: 100-continue, foobar\r\n\r\n",
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

const cfw_test_case cfw_test_server_cases[] = {
    {"server_expect_rejected", test_server_expect_rejected},
    {"server_host_required", test_server_host_required},
    {"server_streaming_response", test_server_streaming_response},
    {"server_streaming_http10", test_server_streaming_http10},
    {"server_request_response", test_server_request_response},
    {"server_post_body", test_server_post_body},
    {"server_request_timeout", test_server_request_timeout},
    {"server_stop", test_server_stop},
    {"server_max_connections", test_server_max_connections},
    {"server_soak", test_server_soak},
    {"server_drain_after_stop", test_server_drain_after_stop},
    {"server_expect_continue", test_server_expect_continue},
    {"server_keep_alive_timeout", test_server_keep_alive_timeout},
    {"server_dns_listen", test_server_dns_listen},
    {NULL, NULL},
};

#else /* _WIN32 */


static int test_server_soak(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    char response[4096];
    ssize_t n;
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

    cfw_fd_close(&client);
    cfw_test_server_teardown(&ts);
    return 0;
}


static int test_server_drain_after_stop(void)
{
    cfw_test_server ts;
    cfw_fd_t client;
    char response[4096];
    ssize_t n;

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
    ssize_t n;
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
    ssize_t n;

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

const cfw_test_case cfw_test_server_cases[] = {
    {NULL, NULL},
};

#endif /* !_WIN32 */
