/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/* End-to-end pipeline test: real loopback TCP connection through
 * cfw_http1_conn event reading, cfw_dispatch routing/middleware, and
 * response serialization back onto the wire. */

#include <cfw/cfw.h>

#include "test_harness.h"

#if !defined(_WIN32)
    #include "private/net.h"

    #include <errno.h>
    #include <string.h>
    #include <sys/socket.h>
    #include <unistd.h>

static int cfw_itcp_pair(cfw_fd_t *server_out, cfw_fd_t *client_out)
{
    cfw_fd_t listener;
    cfw_fd_t client;
    cfw_fd_t server_side;
    cfw_sockaddr_t bind_addr;
    cfw_sockaddr_t connect_addr;
    cfw_sockaddr_t local_addr;
    cfw_sockaddr_t peer_addr;
    uint16_t port;
    int attempts;

    cfw_fd_init(&listener);
    cfw_fd_init(&client);
    cfw_fd_init(&server_side);

    CFW_TEST_ASSERT_EQ(cfw_addr_any(0, 0, &bind_addr), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_socket(AF_INET, SOCK_STREAM, 0, &listener),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_bind(&listener, &bind_addr), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_listen(&listener, 4), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_local_addr(&listener, &local_addr), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_addr_port(&local_addr, &port), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_addr_from_str(&connect_addr, "127.0.0.1", port),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_socket(AF_INET, SOCK_STREAM, 0, &client),
                       CFW_OK);

    {
        cfw_status_t status = cfw_net_connect(&client, &connect_addr);
        if (status != CFW_OK && status != CFW_ERROR_WOULDBLOCK) {
            return 1;
        }
    }

    for (attempts = 0; attempts < 1000; attempts++) {
        cfw_status_t status =
            cfw_net_accept(&listener, &peer_addr, &server_side);
        if (status == CFW_OK) {
            break;
        }
        if (status != CFW_ERROR_WOULDBLOCK) {
            cfw_fd_close(&listener);
            cfw_fd_close(&client);
            return 1;
        }
        usleep(1000);
    }
    if (attempts == 1000) {
        cfw_fd_close(&listener);
        cfw_fd_close(&client);
        return 1;
    }

    cfw_fd_close(&listener);
    *server_out = server_side;
    *client_out = client;
    return 0;
}

static int cfw_itcp_send(cfw_fd_t *client, const char *data)
{
    size_t len = strlen(data);
    ssize_t n = send((int)cfw_fd_get(client), data, len, 0);
    return n == (ssize_t)len ? 0 : 1;
}

static ssize_t cfw_itcp_recv(cfw_fd_t *client, char *buffer, size_t capacity)
{
    int attempts;
    for (attempts = 0; attempts < 500; attempts++) {
        ssize_t n = recv((int)cfw_fd_get(client), buffer, capacity, 0);
        if (n > 0) {
            return n;
        }
        if (n == 0) {
            return 0;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        usleep(1000);
    }
    return -1;
}

static int cfw_itcp_contains(const char *haystack, size_t len,
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

/* --- Pipeline under test ------------------------------------------- */

typedef struct {
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_http1_conn_t conn;
} cfw_pipeline_t;

static cfw_status_t cfw_test_echo_handler(cfw_request_t *req,
                                          cfw_response_t *res, void *user)
{
    static const char body[] = "pipeline-ok";
    (void)user;

    res->status = CFW_HTTP_STATUS_OK;
    res->body.data = (const uint8_t *)body;
    res->body.size = sizeof(body) - 1;

    /* Echo the route param back as a header when present. */
    {
        cfw_strview_t id_name;
        cfw_strview_t id_value;
        cfw_strview_from_cstr(&id_name, "id");
        if (cfw_route_match_param(req->params, req->params_written,
                                  &id_name, &id_value) == CFW_OK) {
            cfw_strview_t hname;
            cfw_strview_from_cstr(&hname, "X-Route-Id");
            cfw_response_header_add(res, &hname, &id_value);
        }
    }
    return CFW_OK;
}

static cfw_status_t cfw_test_tag_middleware(cfw_request_t *req,
                                            cfw_response_t *res,
                                            cfw_dispatch_next_t next,
                                            cfw_dispatch_frame_t *frame,
                                            void *user)
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
    cfw_strview_from_cstr(&name, "X-Middleware");
    cfw_strview_from_cstr(&value, "ran");
    return cfw_response_header_add(res, &name, &value);
}

static void cfw_pipeline_destroy(cfw_pipeline_t *p)
{
    cfw_http1_conn_free(&p->conn);
    cfw_arena_destroy(p->arena);
}

/* Drives the connection until `expect_events` MESSAGE_DONE dispatches
 * have run or an error occurs. Returns 0 on success. */
static int cfw_pipeline_serve(cfw_pipeline_t *p, int max_dispatches)
{
    int dispatches = 0;
    cfw_http_request_head_t head;
    cfw_http_header_t res_headers[16];
    cfw_route_param_t params[16];
    cfw_response_t res;
    unsigned char body_buf[8192];
    size_t body_len = 0;
    int have_head = 0;
    int attempts;

    for (attempts = 0; attempts < 100000 && dispatches < max_dispatches;
         attempts++) {
        cfw_http1_event_t event;
        cfw_status_t st = cfw_http1_conn_next_event(&p->conn, &event);

        if (st == CFW_ERROR_WOULDBLOCK) {
            usleep(1000);
            continue;
        }
        if (st != CFW_OK) {
            return 1;
        }

        switch (event.kind) {
        case CFW_HTTP1_EV_HEAD:
            head = event.head;
            body_len = 0;
            have_head = 1;
            break;
        case CFW_HTTP1_EV_BODY:
            if (body_len + event.body.size > sizeof(body_buf)) {
                return 1;
            }
            memcpy(body_buf + body_len, event.body.data, event.body.size);
            body_len += event.body.size;
            break;
        case CFW_HTTP1_EV_MESSAGE_DONE: {
            cfw_span_t body;
            char out_head[1024];
            size_t written = 0;
            cfw_request_t req_view;

            if (!have_head) {
                return 1;
            }
            memset(&body, 0, sizeof(body));
            body.data = body_buf;
            body.size = body_len;
            memset(&res, 0, sizeof(res));
            res.headers = res_headers;
            res.header_capacity = 16;
            if (cfw_dispatch_request(&p->dispatch, &head, &body,
                                     res_headers, 16, params, 16, &res) !=
                CFW_OK) {
                return 1;
            }

            /* Build a request view for the head formatter. */
            memset(&req_view, 0, sizeof(req_view));
            req_view.method = head.request_line.method;
            req_view.version = head.request_line.version;

            if (cfw_dispatch_response_head_format(&req_view, &res, out_head,
                                                  sizeof(out_head),
                                                  &written) != CFW_OK) {
                return 1;
            }
            if (cfw_http1_conn_queue(&p->conn, out_head, written) != CFW_OK) {
                return 1;
            }
            if (cfw_http_response_has_body(head.request_line.method,
                                           res.status) &&
                res.body.size != 0) {
                if (cfw_http1_conn_queue(&p->conn, res.body.data,
                                         res.body.size) != CFW_OK) {
                    return 1;
                }
            }
            if (cfw_http1_conn_flush(&p->conn) == CFW_ERROR_IO) {
                return 1;
            }
            dispatches++;
            have_head = 0;
            break;
        }
        case CFW_HTTP1_EV_ERROR:
            return 1;
        }
    }
    return dispatches == max_dispatches ? 0 : 1;
}

static int test_integration_get_and_404(void)
{
    cfw_pipeline_t p;
    cfw_fd_t client;
    cfw_dispatch_options_t options;
    char response[4096];
    ssize_t n;

    memset(&p, 0, sizeof(p));
    p.arena = cfw_arena_create(16384);
    CFW_TEST_ASSERT_TRUE(p.arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&p.router, p.arena), CFW_OK);

    memset(&options, 0, sizeof(options));
    options.router = &p.router;
    options.handler = cfw_test_echo_handler;
    options.handler_user = NULL;
    options.max_middleware = 4;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&p.dispatch, p.arena, &options),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_dispatch_use(&p.dispatch, cfw_test_tag_middleware,
                                        NULL),
                       CFW_OK);
    {
        cfw_strview_t pattern;
        cfw_strview_from_cstr(&pattern, "/items/:id");
        CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(
                               &p.dispatch, CFW_HTTP_METHOD_GET, &pattern,
                               cfw_test_echo_handler, NULL),
                           CFW_OK);
    }

    {
        cfw_fd_t server_fd;
        cfw_http1_conn_options_t conn_options;
        cfw_fd_init(&server_fd);
        CFW_TEST_ASSERT_EQ(cfw_itcp_pair(&server_fd, &client), 0);
        conn_options.http.max_head_size = 8192;
        conn_options.http.max_body_size = 65536;
        conn_options.http.max_pending_size = 65536;
        conn_options.max_out_size = 8192;
        CFW_TEST_ASSERT_EQ(
            cfw_http1_conn_init(&p.conn, &server_fd, &conn_options), CFW_OK);
    }

    /* Request 1: bound route with param + middleware. */
    CFW_TEST_ASSERT_EQ(cfw_itcp_send(&client,
                                     "GET /items/77 HTTP/1.1\r\n"
                                     "Host: test\r\n\r\n"),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_pipeline_serve(&p, 1), 0);
    n = cfw_itcp_recv(&client, response, sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_itcp_contains(response, (size_t)n,
                                           "HTTP/1.1 200 OK"));
    CFW_TEST_ASSERT_TRUE(cfw_itcp_contains(response, (size_t)n,
                                           "Content-Length: 11"));
    CFW_TEST_ASSERT_TRUE(cfw_itcp_contains(response, (size_t)n,
                                           "pipeline-ok"));
    CFW_TEST_ASSERT_TRUE(cfw_itcp_contains(response, (size_t)n,
                                           "X-Middleware: ran"));
    CFW_TEST_ASSERT_TRUE(cfw_itcp_contains(response, (size_t)n, "77"));

    /* Request 2 (keep-alive): unmatched route → 404. */
    CFW_TEST_ASSERT_EQ(cfw_itcp_send(&client,
                                     "GET /nope HTTP/1.1\r\n"
                                     "Host: test\r\n\r\n"),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_pipeline_serve(&p, 1), 0);
    n = cfw_itcp_recv(&client, response, sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_itcp_contains(response, (size_t)n,
                                           "404"));

    cfw_fd_close(&client);
    cfw_pipeline_destroy(&p);
    return 0;
}

static int test_integration_post_body(void)
{
    cfw_pipeline_t p;
    cfw_fd_t client;
    cfw_dispatch_options_t options;
    char response[4096];
    ssize_t n;

    memset(&p, 0, sizeof(p));
    p.arena = cfw_arena_create(16384);
    CFW_TEST_ASSERT_TRUE(p.arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&p.router, p.arena), CFW_OK);

    memset(&options, 0, sizeof(options));
    options.router = &p.router;
    options.handler = cfw_test_echo_handler;
    options.handler_user = NULL;
    options.max_middleware = 2;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&p.dispatch, p.arena, &options),
                       CFW_OK);
    {
        cfw_strview_t pattern;
        cfw_strview_from_cstr(&pattern, "/echo");
        CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(
                               &p.dispatch, CFW_HTTP_METHOD_POST, &pattern,
                               cfw_test_echo_handler, NULL),
                           CFW_OK);
    }

    {
        cfw_fd_t server_fd;
        cfw_http1_conn_options_t conn_options;
        cfw_fd_init(&server_fd);
        CFW_TEST_ASSERT_EQ(cfw_itcp_pair(&server_fd, &client), 0);
        conn_options.http.max_head_size = 8192;
        conn_options.http.max_body_size = 65536;
        conn_options.http.max_pending_size = 65536;
        conn_options.max_out_size = 8192;
        CFW_TEST_ASSERT_EQ(
            cfw_http1_conn_init(&p.conn, &server_fd, &conn_options), CFW_OK);
    }

    /* POST with Content-Length body. */
    CFW_TEST_ASSERT_EQ(cfw_itcp_send(&client,
                                     "POST /echo HTTP/1.1\r\n"
                                     "Host: test\r\n"
                                     "Content-Length: 11\r\n"
                                     "\r\n"
                                     "hello world"),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_pipeline_serve(&p, 1), 0);
    n = cfw_itcp_recv(&client, response, sizeof(response) - 1);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[n] = '\0';
    CFW_TEST_ASSERT_TRUE(cfw_itcp_contains(response, (size_t)n,
                                           "HTTP/1.1 200 OK"));
    CFW_TEST_ASSERT_TRUE(cfw_itcp_contains(response, (size_t)n,
                                           "pipeline-ok"));

    cfw_fd_close(&client);
    cfw_pipeline_destroy(&p);
    return 0;
}

const cfw_test_case cfw_test_integration_cases[] = {
    {"integration_get_and_404", test_integration_get_and_404},
    {"integration_post_body", test_integration_post_body},
    {NULL, NULL},
};

#else /* _WIN32 */

const cfw_test_case cfw_test_integration_cases[] = {
    {NULL, NULL},
};

#endif /* !_WIN32 */
