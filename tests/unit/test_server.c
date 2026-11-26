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

static int cfw_test_server_setup(cfw_test_server *ts,
                                 cfw_duration_t keep_alive,
                                 cfw_duration_t request_timeout,
                                 size_t max_connections)
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
    options.keep_alive_timeout = keep_alive;
    options.request_timeout = request_timeout;
    options.max_connections = max_connections;
    CFW_TEST_ASSERT_EQ(cfw_server_init(&ts->server, &options), CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_server_port(&ts->server) != 0);
    return 0;
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

const cfw_test_case cfw_test_server_cases[] = {
    {"server_request_response", test_server_request_response},
    {"server_post_body", test_server_post_body},
    {"server_request_timeout", test_server_request_timeout},
    {"server_stop", test_server_stop},
    {"server_max_connections", test_server_max_connections},
    {NULL, NULL},
};

#else /* _WIN32 */

const cfw_test_case cfw_test_server_cases[] = {
    {NULL, NULL},
};

#endif /* !_WIN32 */
