/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/* HTTP client integration tests: real loopback servers — the cfw_server
 * event loop (cleartext and TLS) plus a minimal raw-socket responder
 * for protocol edge cases the server never emits. Runs on every
 * platform with a socket backend (POSIX and Windows). */

#include <cfw/cfw.h>

#include "test_harness.h"
#include "test_loopback.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* cfw_server-backed fixture                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_server_t server;
    cfw_thread_t thread;
    int thread_started;
} cfw_client_fixture_t;

static cfw_status_t cfw_test_client_hello(cfw_request_t *req,
                                          cfw_response_t *res, void *user)
{
    static const char body[] = "client-ok";
    (void)req;
    (void)user;
    res->status = CFW_HTTP_STATUS_OK;
    res->body.data = (const uint8_t *)body;
    res->body.size = sizeof(body) - 1;
    return CFW_OK;
}

static char cfw_test_client_echo_buf[8192];

static cfw_status_t cfw_test_client_echo(cfw_request_t *req,
                                         cfw_response_t *res, void *user)
{
    size_t n = req->body.size;
    (void)user;
    if (n > sizeof(cfw_test_client_echo_buf)) {
        n = sizeof(cfw_test_client_echo_buf);
    }
    if (n != 0) {
        memcpy(cfw_test_client_echo_buf, req->body.data, n);
    }
    res->status = CFW_HTTP_STATUS_OK;
    res->body.data = (const uint8_t *)cfw_test_client_echo_buf;
    res->body.size = n;
    return CFW_OK;
}

/* Echoes the X-Test-Req request header back as X-Echo. */
static cfw_status_t cfw_test_client_hdr(cfw_request_t *req,
                                        cfw_response_t *res, void *user)
{
    static const char body[] = "hdr-ok";
    size_t i;
    cfw_strview_t want;
    (void)user;

    cfw_strview_from_cstr(&want, "x-test-req");
    for (i = 0; i < req->header_count; i++) {
        int eq = 0;
        if (cfw_strview_equal_nocase(&req->headers[i].name, &want,
                                     &eq) == CFW_OK &&
            eq) {
            cfw_strview_t hname;
            cfw_strview_from_cstr(&hname, "X-Echo");
            cfw_response_header_add(res, &hname, &req->headers[i].value);
            break;
        }
    }
    res->status = CFW_HTTP_STATUS_OK;
    res->body.data = (const uint8_t *)body;
    res->body.size = sizeof(body) - 1;
    return CFW_OK;
}

typedef struct {
    int sent;
} cfw_test_stream_state_t;

/* Stream state for the /chunked route; written before the server
 * thread starts and only read by it afterwards. */
static cfw_test_stream_state_t cfw_test_client_stream_state;

static cfw_status_t cfw_test_client_stream(void *user, cfw_span_t *chunk,
                                           int *eof)
{
    static const char *const parts[] = {"stream-", "body"};
    cfw_test_stream_state_t *state = (cfw_test_stream_state_t *)user;

    if (state->sent >= 2) {
        *eof = 1;
        return CFW_OK;
    }
    chunk->data = (const uint8_t *)parts[state->sent];
    chunk->size = strlen(parts[state->sent]);
    state->sent++;
    *eof = 0;
    return CFW_OK;
}

static cfw_status_t cfw_test_client_chunked(cfw_request_t *req,
                                            cfw_response_t *res,
                                            void *user)
{
    cfw_test_stream_state_t *state =
        (cfw_test_stream_state_t *)user;
    (void)req;
    res->status = CFW_HTTP_STATUS_OK;
    res->stream = cfw_test_client_stream;
    res->stream_user = state;
    res->stream_size = CFW_STREAM_SIZE_UNKNOWN;
    return CFW_OK;
}

static int cfw_client_server_thread_main(void *arg)
{
    cfw_client_fixture_t *fixture = (cfw_client_fixture_t *)arg;
    return (int)cfw_server_run(&fixture->server);
}

static int cfw_client_add_route(cfw_client_fixture_t *fixture,
                                cfw_http_method_t method,
                                const char *pattern, cfw_handler_fn handler,
                                void *user)
{
    cfw_strview_t view;
    cfw_strview_from_cstr(&view, pattern);
    return cfw_dispatch_add_route(&fixture->dispatch, method, &view,
                                  handler, user) == CFW_OK
               ? 0
               : 1;
}

static int cfw_client_server_start(cfw_client_fixture_t *fixture)
{
    cfw_server_options_t options;
    cfw_dispatch_options_t doptions;
    cfw_status_t status;

    memset(fixture, 0, sizeof(*fixture));
    memset(&cfw_test_client_stream_state, 0,
           sizeof(cfw_test_client_stream_state));
    fixture->arena = cfw_arena_create(32768);
    CFW_TEST_ASSERT_TRUE(fixture->arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&fixture->router, fixture->arena),
                       CFW_OK);

    memset(&doptions, 0, sizeof(doptions));
    doptions.router = &fixture->router;
    doptions.handler = cfw_test_client_hello;
    doptions.max_middleware = 4;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&fixture->dispatch,
                                         fixture->arena, &doptions),
                       CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_client_add_route(
                           fixture, CFW_HTTP_METHOD_GET, "/hello",
                           cfw_test_client_hello, NULL),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_client_add_route(
                           fixture, CFW_HTTP_METHOD_POST, "/echo",
                           cfw_test_client_echo, NULL),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_client_add_route(
                           fixture, CFW_HTTP_METHOD_GET, "/hdr",
                           cfw_test_client_hdr, NULL),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_client_add_route(
                           fixture, CFW_HTTP_METHOD_GET, "/chunked",
                           cfw_test_client_chunked,
                           &cfw_test_client_stream_state),
                       0);

    memset(&options, 0, sizeof(options));
    options.host = "127.0.0.1";
    options.port = 0;
    options.dispatch = &fixture->dispatch;
    options.http.max_head_size = 8192;
    options.http.max_body_size = 65536;
    options.http.max_pending_size = 65536;
    options.max_out_size = 65536;
    options.max_connections = 8;
    options.max_headers = 32;
    options.max_params = 8;
    status = cfw_server_init(&fixture->server, &options);
    if (status != CFW_OK) {
        return 1;
    }
    CFW_TEST_ASSERT_TRUE(cfw_server_port(&fixture->server) != 0);

    status = cfw_thread_create(&fixture->thread,
                               cfw_client_server_thread_main, fixture);
    if (status != CFW_OK) {
        cfw_server_free(&fixture->server);
        return 1;
    }
    fixture->thread_started = 1;
    return 0;
}

static void cfw_client_server_stop(cfw_client_fixture_t *fixture)
{
    if (fixture->thread_started) {
        cfw_server_stop(&fixture->server);
        {
            int exit_code = 0;
            cfw_thread_join(&fixture->thread, &exit_code);
        }
    }
    cfw_server_free(&fixture->server);
    cfw_arena_destroy(fixture->arena);
}

/* ------------------------------------------------------------------ */
/* Request helpers                                                     */
/* ------------------------------------------------------------------ */

static void cfw_client_req_init(cfw_http_client_request_t *req,
                                const char *host, uint16_t port,
                                const char *target)
{
    memset(req, 0, sizeof(*req));
    req->host = host;
    req->port = port;
    req->method = CFW_HTTP_METHOD_GET;
    req->target = target;
    cfw_duration_from_millis(5000, &req->timeout);
}

static int cfw_client_expect_status(
    cfw_http_client_response_t *response, int status)
{
    cfw_http_response_head_t head;
    CFW_TEST_ASSERT_EQ(cfw_http_client_response_head(response, &head),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ((int)head.status_code, status);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Tests against the live server                                       */
/* ------------------------------------------------------------------ */

static int test_client_get_head_and_headers(void)
{
    cfw_client_fixture_t fixture;
    cfw_http_client_request_t req;
    cfw_http_client_response_t response;
    uint16_t port;

    CFW_TEST_ASSERT_EQ(cfw_client_server_start(&fixture), 0);
    port = cfw_server_port(&fixture.server);

    /* GET via the "localhost" name exercises the DNS path. */
    cfw_client_req_init(&req, "localhost", port, "/hello");
    memset(&response, 0, sizeof(response));
    CFW_TEST_ASSERT_EQ(cfw_http_client_request(&req, &response), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_client_expect_status(&response, 200), 0);
    CFW_TEST_ASSERT_TRUE(response.body.size == 9 &&
                         memcmp(response.body.data, "client-ok", 9) == 0);
    /* The stored head must re-parse and expose a non-empty header
     * block. */
    {
        cfw_http_response_head_t head;
        CFW_TEST_ASSERT_EQ(cfw_http_client_response_head(&response, &head),
                           CFW_OK);
        CFW_TEST_ASSERT_TRUE(head.headers.size != 0);
    }
    cfw_http_client_response_free(&response);

    /* HEAD: same route, no body even though Content-Length is sent. */
    cfw_client_req_init(&req, "127.0.0.1", port, "/hello");
    req.method = CFW_HTTP_METHOD_HEAD;
    memset(&response, 0, sizeof(response));
    CFW_TEST_ASSERT_EQ(cfw_http_client_request(&req, &response), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_client_expect_status(&response, 200), 0);
    CFW_TEST_ASSERT_EQ(response.body.size, 0);
    cfw_http_client_response_free(&response);

    /* Custom request header reaches the handler; response header
     * comes back through the parsed head. */
    {
        cfw_http_header_t headers[1];
        cfw_strview_t hn, hv;
        cfw_strview_from_cstr(&hn, "X-Test-Req");
        cfw_strview_from_cstr(&hv, "ping");
        headers[0].name = hn;
        headers[0].value = hv;
        cfw_client_req_init(&req, "127.0.0.1", port, "/hdr");
        req.headers = headers;
        req.header_count = 1;
        memset(&response, 0, sizeof(response));
        CFW_TEST_ASSERT_EQ(cfw_http_client_request(&req, &response),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_client_expect_status(&response, 200), 0);
        {
            cfw_http_response_head_t head;
            cfw_http_header_t rh;
            size_t cursor = 0;
            int done = 0;
            int found = 0;
            CFW_TEST_ASSERT_EQ(cfw_http_client_response_head(&response,
                                                           &head),
                               CFW_OK);
            while (cfw_http_headers_next(&head.headers, &cursor, &rh,
                                         &done) == CFW_OK &&
                   !done) {
                cfw_strview_t echo_name;
                cfw_strview_from_cstr(&echo_name, "x-echo");
                {
                    int eq = 0;
                    if (cfw_strview_equal_nocase(&rh.name, &echo_name,
                                                 &eq) == CFW_OK &&
                        eq && rh.value.size == 4 &&
                        memcmp(rh.value.data, "ping", 4) == 0) {
                        found = 1;
                    }
                }
            }
            CFW_TEST_ASSERT_TRUE(found);
        }
        cfw_http_client_response_free(&response);
    }

    /* 404 for an unmatched route. */
    cfw_client_req_init(&req, "127.0.0.1", port, "/missing");
    memset(&response, 0, sizeof(response));
    CFW_TEST_ASSERT_EQ(cfw_http_client_request(&req, &response), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_client_expect_status(&response, 404), 0);
    cfw_http_client_response_free(&response);

    cfw_client_server_stop(&fixture);
    return 0;
}

static int test_client_post_echo_and_chunked(void)
{
    cfw_client_fixture_t fixture;
    cfw_http_client_request_t req;
    cfw_http_client_response_t response;
    static const char payload[] = "ping-body-0123456789";
    uint16_t port;

    CFW_TEST_ASSERT_EQ(cfw_client_server_start(&fixture), 0);
    port = cfw_server_port(&fixture.server);

    /* POST with a body; the handler echoes it back. */
    cfw_client_req_init(&req, "127.0.0.1", port, "/echo");
    req.method = CFW_HTTP_METHOD_POST;
    req.body = payload;
    req.body_size = sizeof(payload) - 1;
    memset(&response, 0, sizeof(response));
    CFW_TEST_ASSERT_EQ(cfw_http_client_request(&req, &response), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_client_expect_status(&response, 200), 0);
    CFW_TEST_ASSERT_TRUE(response.body.size == sizeof(payload) - 1 &&
                         memcmp(response.body.data, payload,
                                sizeof(payload) - 1) == 0);
    cfw_http_client_response_free(&response);

    /* Chunked (close-unknown-size stream) response decodes fully. */
    cfw_client_req_init(&req, "127.0.0.1", port, "/chunked");
    memset(&response, 0, sizeof(response));
    CFW_TEST_ASSERT_EQ(cfw_http_client_request(&req, &response), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_client_expect_status(&response, 200), 0);
    CFW_TEST_ASSERT_TRUE(response.body.size == 11 &&
                         memcmp(response.body.data, "stream-body", 11) == 0);
    cfw_http_client_response_free(&response);

    cfw_client_server_stop(&fixture);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Raw canned-response server                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    cfw_thread_t thread;
    cfw_fd_t listener;
    uint16_t port;
    const char *response;
    size_t response_len;
    int read_request;
    int hold_ms;
    int thread_started;
} cfw_raw_server_t;

static int cfw_raw_server_send_all(cfw_fd_t *fd, const char *data,
                                   size_t size)
{
    size_t off = 0;
    int attempts = 0;
    while (off < size && attempts < 10000) {
        size_t sent = 0;
        cfw_status_t st = cfw_net_send(fd, data + off, size - off, &sent);
        if (st == CFW_OK) {
            off += sent;
            attempts = 0;
            continue;
        }
        if (st != CFW_ERROR_WOULDBLOCK) {
            return 1;
        }
        cfw_test_sleep_us(1000);
        attempts++;
    }
    return off == size ? 0 : 1;
}

static int cfw_raw_server_main(void *arg)
{
    cfw_raw_server_t *rs = (cfw_raw_server_t *)arg;
    cfw_fd_t conn;
    cfw_addr_t peer;
    char drain[4096];
    int attempts;

    cfw_fd_init(&conn);
    for (attempts = 0; attempts < 10000; attempts++) {
        cfw_status_t st = cfw_net_accept(&rs->listener, &peer, &conn);
        if (st == CFW_OK) {
            break;
        }
        if (st != CFW_ERROR_WOULDBLOCK) {
            return 1;
        }
        cfw_test_sleep_us(1000);
    }
    if (attempts == 10000) {
        return 1;
    }

    if (rs->read_request) {
        /* Consume whatever the client sent so far; retry briefly on
         * WOULDBLOCK since the request may still be in flight. */
        int tries;
        for (tries = 0; tries < 500; tries++) {
            size_t got = 0;
            cfw_status_t st =
                cfw_net_recv(&conn, drain, sizeof(drain), &got);
            if (st == CFW_OK || st != CFW_ERROR_WOULDBLOCK) {
                break;
            }
            cfw_test_sleep_us(1000);
        }
    }
    if (rs->hold_ms > 0) {
        cfw_test_sleep_us((long)rs->hold_ms * 1000L);
    }
    if (rs->response != NULL &&
        cfw_raw_server_send_all(&conn, rs->response, rs->response_len) != 0) {
        cfw_fd_close(&conn);
        return 1;
    }
    cfw_fd_close(&conn);
    cfw_fd_close(&rs->listener);
    return 0;
}

static int cfw_raw_server_start(cfw_raw_server_t *rs, const char *response,
                                size_t response_len, int read_request,
                                int hold_ms)
{
    cfw_addr_t bind_addr;
    cfw_addr_t local_addr;
    cfw_status_t status;

    memset(rs, 0, sizeof(*rs));
    rs->response = response;
    rs->response_len = response_len;
    rs->read_request = read_request;
    rs->hold_ms = hold_ms;

    cfw_fd_init(&rs->listener);
    CFW_TEST_ASSERT_EQ(cfw_addr_any(0, 0, &bind_addr), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_socket(AF_INET, SOCK_STREAM, 0,
                                      &rs->listener),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_bind(&rs->listener, &bind_addr), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_listen(&rs->listener, 4), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_local_addr(&rs->listener, &local_addr),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_addr_port(&local_addr, &rs->port), CFW_OK);

    status = cfw_thread_create(&rs->thread, cfw_raw_server_main, rs);
    CFW_TEST_ASSERT_EQ(status, CFW_OK);
    rs->thread_started = 1;
    return 0;
}

static void cfw_raw_server_stop(cfw_raw_server_t *rs)
{
    if (rs->thread_started) {
        int exit_code = 0;
        cfw_thread_join(&rs->thread, &exit_code);
        rs->thread_started = 0;
    }
}

static cfw_status_t cfw_raw_request(cfw_raw_server_t *rs,
                                    cfw_http_client_response_t *response)
{
    cfw_http_client_request_t req;
    cfw_client_req_init(&req, "127.0.0.1", rs->port, "/");
    cfw_duration_from_millis(2000, &req.timeout);
    memset(response, 0, sizeof(*response));
    return cfw_http_client_request(&req, response);
}

static int test_client_raw_until_close(void)
{
    static const char response[] =
        "HTTP/1.1 200 OK\r\nX-Tag: raw\r\n\r\nclose-body";
    cfw_raw_server_t rs;
    cfw_http_client_response_t out;

    CFW_TEST_ASSERT_EQ(cfw_raw_server_start(&rs, response,
                                            sizeof(response) - 1, 1, 0),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_raw_request(&rs, &out), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_client_expect_status(&out, 200), 0);
    CFW_TEST_ASSERT_TRUE(out.body.size == 10 &&
                         memcmp(out.body.data, "close-body", 10) == 0);
    cfw_http_client_response_free(&out);
    cfw_raw_server_stop(&rs);
    return 0;
}

static int test_client_raw_interim_100(void)
{
    static const char response[] =
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    cfw_raw_server_t rs;
    cfw_http_client_response_t out;

    CFW_TEST_ASSERT_EQ(cfw_raw_server_start(&rs, response,
                                            sizeof(response) - 1, 1, 0),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_raw_request(&rs, &out), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_client_expect_status(&out, 200), 0);
    CFW_TEST_ASSERT_TRUE(out.body.size == 2 &&
                         memcmp(out.body.data, "ok", 2) == 0);
    /* The stored head is the FINAL response head, not the 100. */
    {
        cfw_http_response_head_t head;
        CFW_TEST_ASSERT_EQ(cfw_http_client_response_head(&out, &head),
                           CFW_OK);
        CFW_TEST_ASSERT_TRUE(head.status_code == 200);
    }
    cfw_http_client_response_free(&out);
    cfw_raw_server_stop(&rs);
    return 0;
}

static int test_client_raw_chunked_and_204(void)
{
    static const char chunked[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
    cfw_raw_server_t rs;
    cfw_http_client_response_t out;

    CFW_TEST_ASSERT_EQ(cfw_raw_server_start(&rs, chunked,
                                            sizeof(chunked) - 1, 1, 0),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_raw_request(&rs, &out), CFW_OK);
    CFW_TEST_ASSERT_TRUE(out.body.size == 11 &&
                         memcmp(out.body.data, "hello world", 11) == 0);
    cfw_http_client_response_free(&out);
    cfw_raw_server_stop(&rs);

    {
        static const char no_content[] =
            "HTTP/1.1 204 No Content\r\nContent-Length: 9\r\n\r\n";
        CFW_TEST_ASSERT_EQ(cfw_raw_server_start(&rs, no_content,
                                                sizeof(no_content) - 1,
                                                1, 0),
                           0);
        CFW_TEST_ASSERT_EQ(cfw_raw_request(&rs, &out), CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_client_expect_status(&out, 204), 0);
        CFW_TEST_ASSERT_EQ(out.body.size, 0);
        cfw_http_client_response_free(&out);
        cfw_raw_server_stop(&rs);
    }
    return 0;
}

static int test_client_raw_errors(void)
{
    cfw_raw_server_t rs;
    cfw_http_client_response_t out;
    cfw_http_client_request_t req;

    /* Malformed status line → INVALID_ARGUMENT. */
    {
        static const char bad[] = "this is not http\r\n\r\n";
        CFW_TEST_ASSERT_EQ(cfw_raw_server_start(&rs, bad,
                                                sizeof(bad) - 1, 1, 0),
                           0);
        CFW_TEST_ASSERT_EQ(cfw_raw_request(&rs, &out),
                           CFW_ERROR_INVALID_ARGUMENT);
        cfw_http_client_response_free(&out);
        cfw_raw_server_stop(&rs);
    }

    /* Truncated Content-Length body → IO. */
    {
        static const char short_body[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc";
        CFW_TEST_ASSERT_EQ(cfw_raw_server_start(&rs, short_body,
                                                sizeof(short_body) - 1,
                                                1, 0),
                           0);
        CFW_TEST_ASSERT_EQ(cfw_raw_request(&rs, &out), CFW_ERROR_IO);
        cfw_http_client_response_free(&out);
        cfw_raw_server_stop(&rs);
    }

    /* Response head over max_head_size → OVERFLOW. */
    {
        char big[256];
        size_t off = 0;
        off += (size_t)snprintf(big + off, sizeof(big) - off,
                                "HTTP/1.1 200 OK\r\nX-Pad: ");
        memset(big + off, 'a', 200);
        off += 200;
        off += (size_t)snprintf(big + off, sizeof(big) - off, "\r\n\r\n");
        CFW_TEST_ASSERT_EQ(cfw_raw_server_start(&rs, big, off, 1, 0), 0);
        cfw_client_req_init(&req, "127.0.0.1", rs.port, "/");
        req.max_head_size = 64;
        memset(&out, 0, sizeof(out));
        CFW_TEST_ASSERT_EQ(cfw_http_client_request(&req, &out),
                           CFW_ERROR_OVERFLOW);
        cfw_http_client_response_free(&out);
        cfw_raw_server_stop(&rs);
    }

    /* Decoded body over max_body_size → OVERFLOW. */
    {
        static const char body_resp[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 32\r\n\r\n"
            "01234567890123456789012345678901";
        CFW_TEST_ASSERT_EQ(cfw_raw_server_start(&rs, body_resp,
                                                sizeof(body_resp) - 1,
                                                1, 0),
                           0);
        cfw_client_req_init(&req, "127.0.0.1", rs.port, "/");
        req.max_body_size = 16;
        memset(&out, 0, sizeof(out));
        CFW_TEST_ASSERT_EQ(cfw_http_client_request(&req, &out),
                           CFW_ERROR_OVERFLOW);
        cfw_http_client_response_free(&out);
        cfw_raw_server_stop(&rs);
    }

    /* Server accepts but never responds → TIMEOUT. */
    {
        CFW_TEST_ASSERT_EQ(cfw_raw_server_start(&rs, NULL, 0, 1, 1500), 0);
        cfw_client_req_init(&req, "127.0.0.1", rs.port, "/");
        cfw_duration_from_millis(200, &req.timeout);
        memset(&out, 0, sizeof(out));
        CFW_TEST_ASSERT_EQ(cfw_http_client_request(&req, &out),
                           CFW_ERROR_TIMEOUT);
        cfw_http_client_response_free(&out);
        cfw_raw_server_stop(&rs);
    }

    /* Nothing listening → connect refused → IO. */
    {
        cfw_addr_t bind_addr;
        cfw_addr_t local_addr;
        cfw_fd_t probe;
        uint16_t dead_port = 0;

        cfw_fd_init(&probe);
        CFW_TEST_ASSERT_EQ(cfw_addr_any(0, 0, &bind_addr), CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_net_socket(AF_INET, SOCK_STREAM, 0, &probe),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_net_bind(&probe, &bind_addr), CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_net_listen(&probe, 1), CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_net_local_addr(&probe, &local_addr), CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_addr_port(&local_addr, &dead_port), CFW_OK);
        cfw_fd_close(&probe);

        cfw_client_req_init(&req, "127.0.0.1", dead_port, "/");
        memset(&out, 0, sizeof(out));
        CFW_TEST_ASSERT_EQ(cfw_http_client_request(&req, &out),
                           CFW_ERROR_IO);
        cfw_http_client_response_free(&out);
    }
    return 0;
}

static int test_client_bad_args(void)
{
    cfw_http_client_request_t req;
    cfw_http_client_response_t response;
    cfw_http_header_t headers[1];
    cfw_strview_t hn, hv;

    memset(&response, 0, sizeof(response));

    /* NULL request. */
    CFW_TEST_ASSERT_EQ(cfw_http_client_request(NULL, &response),
                       CFW_ERROR_INVALID_ARGUMENT);
    /* Missing host / port / target. */
    cfw_client_req_init(&req, NULL, 80, "/");
    CFW_TEST_ASSERT_EQ(cfw_http_client_request(&req, &response),
                       CFW_ERROR_INVALID_ARGUMENT);
    cfw_client_req_init(&req, "127.0.0.1", 0, "/");
    CFW_TEST_ASSERT_EQ(cfw_http_client_request(&req, &response),
                       CFW_ERROR_INVALID_ARGUMENT);
    cfw_client_req_init(&req, "127.0.0.1", 80, NULL);
    CFW_TEST_ASSERT_EQ(cfw_http_client_request(&req, &response),
                       CFW_ERROR_INVALID_ARGUMENT);
    /* Unknown method. */
    cfw_client_req_init(&req, "127.0.0.1", 80, "/");
    req.method = CFW_HTTP_METHOD_UNKNOWN;
    CFW_TEST_ASSERT_EQ(cfw_http_client_request(&req, &response),
                       CFW_ERROR_INVALID_ARGUMENT);
    /* Framework-managed header names are rejected. */
    cfw_client_req_init(&req, "127.0.0.1", 80, "/");
    cfw_strview_from_cstr(&hn, "Host");
    cfw_strview_from_cstr(&hv, "evil");
    headers[0].name = hn;
    headers[0].value = hv;
    req.headers = headers;
    req.header_count = 1;
    CFW_TEST_ASSERT_EQ(cfw_http_client_request(&req, &response),
                       CFW_ERROR_INVALID_ARGUMENT);
    /* body_size without body. */
    req.headers = NULL;
    req.header_count = 0;
    req.body = NULL;
    req.body_size = 4;
    CFW_TEST_ASSERT_EQ(cfw_http_client_request(&req, &response),
                       CFW_ERROR_INVALID_ARGUMENT);
    /* response_head on an empty response → INVALID_ARGUMENT. */
    {
        cfw_http_response_head_t head;
        CFW_TEST_ASSERT_EQ(cfw_http_client_response_head(&response, &head),
                           CFW_ERROR_INVALID_ARGUMENT);
    }
    return 0;
}

#if CFW_HAVE_TLS

static int test_client_tls_roundtrip(void)
{
    /* TLS variant of the fixture: same dispatch, tls_ctx on both ends. */
    cfw_client_fixture_t fixture;
    cfw_http_client_request_t req;
    cfw_http_client_response_t response;
    cfw_tls_ctx_t *server_ctx = NULL;
    cfw_tls_ctx_t *client_ctx = NULL;
    cfw_tls_ctx_t *bad_ctx = NULL;
    cfw_tls_ctx_options_t opts;
    char path[512];
    uint16_t port;
    cfw_status_t status;

    /* Build contexts directly (paths under CFW_TEST_TLS_DIR). */
    memset(&opts, 0, sizeof(opts));
    opts.role = CFW_TLS_SERVER;
    snprintf(path, sizeof(path), "%s/server.crt", CFW_TEST_TLS_DIR);
    opts.cert_file = strdup(path);
    snprintf(path, sizeof(path), "%s/server.key", CFW_TEST_TLS_DIR);
    opts.key_file = strdup(path);
    status = cfw_tls_ctx_create(&opts, &server_ctx);
    free((void *)opts.cert_file);
    free((void *)opts.key_file);
    CFW_TEST_ASSERT_EQ(status, CFW_OK);

    memset(&opts, 0, sizeof(opts));
    opts.role = CFW_TLS_CLIENT;
    snprintf(path, sizeof(path), "%s/ca.crt", CFW_TEST_TLS_DIR);
    opts.ca_file = strdup(path);
    opts.verify_peer = 1;
    status = cfw_tls_ctx_create(&opts, &client_ctx);
    free((void *)opts.ca_file);
    CFW_TEST_ASSERT_EQ(status, CFW_OK);

    /* A client ctx trusting the wrong anchor must fail the handshake:
     * server.crt is a valid PEM but not a CA and did not sign itself. */
    memset(&opts, 0, sizeof(opts));
    opts.role = CFW_TLS_CLIENT;
    snprintf(path, sizeof(path), "%s/server.crt", CFW_TEST_TLS_DIR);
    opts.ca_file = path;
    opts.verify_peer = 1;
    status = cfw_tls_ctx_create(&opts, &bad_ctx);
    CFW_TEST_ASSERT_EQ(status, CFW_OK);

    /* Fixture with TLS on the listener. */
    memset(&fixture, 0, sizeof(fixture));
    fixture.arena = cfw_arena_create(32768);
    CFW_TEST_ASSERT_TRUE(fixture.arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&fixture.router, fixture.arena),
                       CFW_OK);
    {
        cfw_dispatch_options_t doptions;
        memset(&doptions, 0, sizeof(doptions));
        doptions.router = &fixture.router;
        doptions.handler = cfw_test_client_hello;
        doptions.max_middleware = 4;
        CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&fixture.dispatch,
                                             fixture.arena, &doptions),
                           CFW_OK);
    }
    CFW_TEST_ASSERT_EQ(cfw_client_add_route(
                           &fixture, CFW_HTTP_METHOD_GET, "/hello",
                           cfw_test_client_hello, NULL),
                       0);
    {
        cfw_server_options_t options;
        memset(&options, 0, sizeof(options));
        options.host = "127.0.0.1";
        options.port = 0;
        options.dispatch = &fixture.dispatch;
        options.http.max_head_size = 8192;
        options.http.max_body_size = 65536;
        options.http.max_pending_size = 65536;
        options.max_out_size = 65536;
        options.max_connections = 4;
        options.max_headers = 32;
        options.max_params = 8;
        options.tls_ctx = server_ctx;
        CFW_TEST_ASSERT_EQ(cfw_server_init(&fixture.server, &options),
                           CFW_OK);
    }
    port = cfw_server_port(&fixture.server);
    CFW_TEST_ASSERT_EQ(cfw_thread_create(&fixture.thread,
                                       cfw_client_server_thread_main,
                                       &fixture),
                       CFW_OK);
    fixture.thread_started = 1;

    /* Positive: verify_peer + SNI "localhost" matches the cert CN. */
    cfw_client_req_init(&req, "localhost", port, "/hello");
    req.tls_ctx = client_ctx;
    memset(&response, 0, sizeof(response));
    CFW_TEST_ASSERT_EQ(cfw_http_client_request(&req, &response), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_client_expect_status(&response, 200), 0);
    CFW_TEST_ASSERT_TRUE(response.body.size == 9 &&
                         memcmp(response.body.data, "client-ok", 9) == 0);
    cfw_http_client_response_free(&response);

    /* Negative: untrusted CA → handshake failure surfaces as IO. */
    cfw_client_req_init(&req, "localhost", port, "/hello");
    req.tls_ctx = bad_ctx;
    memset(&response, 0, sizeof(response));
    {
        cfw_status_t st = cfw_http_client_request(&req, &response);
        CFW_TEST_ASSERT_TRUE(st == CFW_ERROR_IO ||
                             st == CFW_ERROR_TIMEOUT);
    }
    cfw_http_client_response_free(&response);

    /* Negative: IP literal fails hostname verification even with a
     * trusted CA (cert is CN=localhost, no IP SAN). */
    cfw_client_req_init(&req, "127.0.0.1", port, "/hello");
    req.tls_ctx = client_ctx;
    memset(&response, 0, sizeof(response));
    {
        cfw_status_t st = cfw_http_client_request(&req, &response);
        CFW_TEST_ASSERT_TRUE(st == CFW_ERROR_IO ||
                             st == CFW_ERROR_TIMEOUT);
    }
    cfw_http_client_response_free(&response);

    cfw_client_server_stop(&fixture);
    cfw_tls_ctx_destroy(server_ctx);
    cfw_tls_ctx_destroy(client_ctx);
    cfw_tls_ctx_destroy(bad_ctx);
    return 0;
}

#endif /* CFW_HAVE_TLS */

const cfw_test_case cfw_test_http_client_cases[] = {
    {"client_get_head_and_headers", test_client_get_head_and_headers},
    {"client_post_echo_and_chunked", test_client_post_echo_and_chunked},
    {"client_raw_until_close", test_client_raw_until_close},
    {"client_raw_interim_100", test_client_raw_interim_100},
    {"client_raw_chunked_and_204", test_client_raw_chunked_and_204},
    {"client_raw_errors", test_client_raw_errors},
    {"client_bad_args", test_client_bad_args},
#if CFW_HAVE_TLS
    {"client_tls_roundtrip", test_client_tls_roundtrip},
#endif
    {NULL, NULL},
};

