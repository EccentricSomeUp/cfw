/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/* Security regression tests: request-smuggling and malformed-input
 * attacks must produce 400 responses (or connection close), never a
 * routed request. Runs the full server stack over real loopback TCP. */

#include <cfw/cfw.h>

#include "test_harness.h"

#if !defined(_WIN32)
    #include "private/net.h"

    #include <errno.h>
    #include <string.h>
    #include <sys/socket.h>
    #include <unistd.h>

static cfw_status_t cfw_sec_ok_handler(cfw_request_t *req,
                                       cfw_response_t *res, void *user)
{
    static const char body[] = "routed";
    (void)req;
    (void)user;
    res->status = CFW_HTTP_STATUS_OK;
    res->body.data = (const uint8_t *)body;
    res->body.size = sizeof(body) - 1;
    return CFW_OK;
}

/* Handler that attempts response-splitting via conflicting framing
 * headers; the server must strip Transfer-Encoding and manage
 * Content-Length itself. */
static cfw_status_t cfw_sec_framing_evil_handler(cfw_request_t *req,
                                                 cfw_response_t *res,
                                                 void *user)
{
    static const char body[] = "routed";
    cfw_strview_t hn, hv;
    (void)req;
    (void)user;
    res->status = CFW_HTTP_STATUS_OK;
    res->body.data = (const uint8_t *)body;
    res->body.size = sizeof(body) - 1;
    cfw_strview_from_cstr(&hn, "Transfer-Encoding");
    cfw_strview_from_cstr(&hv, "chunked");
    cfw_response_header_add(res, &hn, &hv);
    cfw_strview_from_cstr(&hn, "Content-Length");
    cfw_strview_from_cstr(&hv, "999999");
    cfw_response_header_add(res, &hn, &hv);
    return CFW_OK;
}

static int cfw_sec_connect(cfw_fd_t *client, uint16_t port)
{
    cfw_sockaddr_t addr;
    cfw_status_t st;

    CFW_TEST_ASSERT_EQ(cfw_addr_from_str(&addr, "127.0.0.1", port), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_socket(AF_INET, SOCK_STREAM, 0, client),
                       CFW_OK);
    st = cfw_net_connect(client, &addr);
    if (st != CFW_OK && st != CFW_ERROR_WOULDBLOCK) {
        return 1;
    }
    return 0;
}

static int cfw_sec_send(cfw_fd_t *client, const char *data)
{
    size_t len = strlen(data);
    ssize_t n = send((int)cfw_fd_get(client), data, len, 0);
    return n == (ssize_t)len ? 0 : 1;
}

static int cfw_sec_contains(const char *haystack, size_t len,
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

typedef struct {
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_server_t server;
} cfw_sec_server;

static int cfw_sec_setup(cfw_sec_server *ss)
{
    cfw_dispatch_options_t dopt;
    cfw_server_options_t sopt;
    cfw_strview_t pattern;

    memset(ss, 0, sizeof(*ss));
    ss->arena = cfw_arena_create(32768);
    CFW_TEST_ASSERT_TRUE(ss->arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&ss->router, ss->arena), CFW_OK);

    memset(&dopt, 0, sizeof(dopt));
    dopt.router = &ss->router;
    dopt.handler = cfw_sec_ok_handler;
    dopt.max_middleware = 2;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&ss->dispatch, ss->arena, &dopt),
                       CFW_OK);

    cfw_strview_from_cstr(&pattern, "/x");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&ss->dispatch,
                                              CFW_HTTP_METHOD_POST,
                                              &pattern, cfw_sec_ok_handler,
                                              NULL),
                       CFW_OK);
    cfw_strview_from_cstr(&pattern, "/hello");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&ss->dispatch,
                                              CFW_HTTP_METHOD_GET,
                                              &pattern, cfw_sec_ok_handler,
                                              NULL),
                       CFW_OK);
    cfw_strview_from_cstr(&pattern, "/evil");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(
                           &ss->dispatch, CFW_HTTP_METHOD_GET, &pattern,
                           cfw_sec_framing_evil_handler, NULL),
                       CFW_OK);

    memset(&sopt, 0, sizeof(sopt));
    sopt.host = "127.0.0.1";
    sopt.port = 0;
    sopt.backlog = 8;
    sopt.dispatch = &ss->dispatch;
    sopt.http.max_head_size = 8192;
    sopt.http.max_body_size = 65536;
    sopt.http.max_pending_size = 65536;
    sopt.max_out_size = 8192;
    sopt.max_connections = 8;
    sopt.max_headers = 32;
    sopt.max_params = 16;
    CFW_TEST_ASSERT_EQ(cfw_server_init(&ss->server, &sopt), CFW_OK);
    return 0;
}

static void cfw_sec_teardown(cfw_sec_server *ss)
{
    cfw_server_free(&ss->server);
    cfw_arena_destroy(ss->arena);
}

/* Sends `request`, serves until bytes arrive, and returns them in
 * `out` (NUL-terminated, length via *out_len). Returns -1 on timeout. */
static ssize_t cfw_sec_roundtrip(cfw_sec_server *ss, cfw_fd_t *client,
                                 const char *request, char *out,
                                 size_t out_cap, size_t *out_len)
{
    cfw_timepoint_t deadline;
    cfw_duration_t slice;
    int attempts;

    CFW_TEST_ASSERT_EQ(cfw_sec_send(client, request), 0);
    cfw_duration_from_millis(50, &slice);
    for (attempts = 0; attempts < 200; attempts++) {
        ssize_t n;
        cfw_deadline_from_now(&deadline, slice);
        if (cfw_server_step(&ss->server, &deadline) != CFW_OK) {
            return -1;
        }
        n = recv((int)cfw_fd_get(client), out, out_cap - 1, MSG_DONTWAIT);
        if (n > 0) {
            out[n] = '\0';
            *out_len = (size_t)n;
            return n;
        }
        if (n == 0) {
            *out_len = 0;
            return 0;
        }
    }
    return -1;
}

/* Asserts the server responds 400 (or closes without routing) to a
 * malicious request. */
static int cfw_sec_expect_400(const char *name, const char *request)
{
    cfw_sec_server ss;
    cfw_fd_t client;
    char response[4096];
    size_t len = 0;
    ssize_t n;

    CFW_TEST_ASSERT_EQ(cfw_sec_setup(&ss), 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_sec_connect(&client,
                                       cfw_server_port(&ss.server)),
                       0);

    n = cfw_sec_roundtrip(&ss, &client, request, response,
                          sizeof(response), &len);
    CFW_TEST_ASSERT_TRUE(n >= 0);
    /* Either a 400 response, or the connection closed with nothing
     * routed (len == 0 is acceptable only when n == 0 EOF). */
    CFW_TEST_ASSERT_TRUE(
        cfw_sec_contains(response, len, "400") || len == 0);
    /* The routed handler body must never appear. */
    CFW_TEST_ASSERT_TRUE(!cfw_sec_contains(response, len, "routed"));

    (void)name;
    cfw_fd_close(&client);
    cfw_sec_teardown(&ss);
    return 0;
}

static int test_sec_cl_te_conflict(void)
{
    return cfw_sec_expect_400(
        "cl_te_conflict",
        "POST /x HTTP/1.1\r\n"
        "Host: sec\r\n"
        "Content-Length: 5\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n\r\n");
}

static int test_sec_duplicate_cl_mismatch(void)
{
    return cfw_sec_expect_400(
        "dup_cl",
        "POST /x HTTP/1.1\r\n"
        "Host: sec\r\n"
        "Content-Length: 5\r\n"
        "Content-Length: 6\r\n"
        "\r\n"
        "12345");
}

static int test_sec_te_non_chunked(void)
{
    return cfw_sec_expect_400(
        "te_gzip",
        "POST /x HTTP/1.1\r\n"
        "Host: sec\r\n"
        "Transfer-Encoding: gzip\r\n"
        "\r\n"
        "data");
}

static int test_sec_te_multi_coding(void)
{
    return cfw_sec_expect_400(
        "te_multi",
        "POST /x HTTP/1.1\r\n"
        "Host: sec\r\n"
        "Transfer-Encoding: chunked, chunked\r\n"
        "\r\n"
        "0\r\n\r\n");
}

static int test_sec_obs_fold(void)
{
    return cfw_sec_expect_400(
        "obs_fold",
        "GET /hello HTTP/1.1\r\n"
        "Host: sec\r\n"
        "X-Fold: one\r\n"
        " two\r\n"
        "\r\n");
}

static int test_sec_bad_request_line(void)
{
    return cfw_sec_expect_400(
        "bad_request_line",
        "G ET /hello HTTP/1.1\r\n"
        "Host: sec\r\n"
        "\r\n");
}

static int test_sec_cl_garbage(void)
{
    return cfw_sec_expect_400(
        "cl_garbage",
        "POST /x HTTP/1.1\r\n"
        "Host: sec\r\n"
        "Content-Length: +5\r\n"
        "\r\n"
        "12345");
}

static int test_sec_chunked_bare_lf(void)
{
    return cfw_sec_expect_400(
        "chunked_bare_lf",
        "POST /x HTTP/1.1\r\n"
        "Host: sec\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "4\n"
        "Wiki\n"
        "0\n\n");
}

static int test_sec_legit_request_still_works(void)
{
    cfw_sec_server ss;
    cfw_fd_t client;
    char response[4096];
    size_t len = 0;
    ssize_t n;

    CFW_TEST_ASSERT_EQ(cfw_sec_setup(&ss), 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_sec_connect(&client,
                                       cfw_server_port(&ss.server)),
                       0);
    n = cfw_sec_roundtrip(&ss, &client,
                          "GET /hello HTTP/1.1\r\n"
                          "Host: sec\r\n"
                          "\r\n",
                          response, sizeof(response), &len);
    CFW_TEST_ASSERT_TRUE(n > 0);
    CFW_TEST_ASSERT_TRUE(cfw_sec_contains(response, len, "200"));
    CFW_TEST_ASSERT_TRUE(cfw_sec_contains(response, len, "routed"));

    cfw_fd_close(&client);
    cfw_sec_teardown(&ss);
    return 0;
}

static int test_sec_response_framing_headers(void)
{
    cfw_sec_server ss;
    cfw_fd_t client;
    char response[4096];
    size_t len = 0;
    ssize_t n;

    CFW_TEST_ASSERT_EQ(cfw_sec_setup(&ss), 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_sec_connect(&client,
                                       cfw_server_port(&ss.server)),
                       0);
    n = cfw_sec_roundtrip(
        &ss, &client,
        "GET /evil HTTP/1.1\r\nHost: x\r\n\r\n",
        response, sizeof(response), &len);
    CFW_TEST_ASSERT_TRUE(n > 0);
    /* Transfer-Encoding must be stripped entirely. */
    CFW_TEST_ASSERT_TRUE(!cfw_sec_contains(response, len,
                                           "Transfer-Encoding"));
    /* The bogus Content-Length must not survive; the server emits the
     * correct one for the 6-byte body. */
    CFW_TEST_ASSERT_TRUE(!cfw_sec_contains(response, len, "999999"));
    CFW_TEST_ASSERT_TRUE(cfw_sec_contains(response, len,
                                          "Content-Length: 6"));
    CFW_TEST_ASSERT_TRUE(cfw_sec_contains(response, len, "routed"));

    cfw_fd_close(&client);
    cfw_sec_teardown(&ss);
    return 0;
}

const cfw_test_case cfw_test_security_cases[] = {
    {"smuggling_cl_te_conflict", test_sec_cl_te_conflict},
    {"smuggling_duplicate_cl", test_sec_duplicate_cl_mismatch},
    {"smuggling_te_non_chunked", test_sec_te_non_chunked},
    {"smuggling_te_multi_coding", test_sec_te_multi_coding},
    {"smuggling_obs_fold", test_sec_obs_fold},
    {"smuggling_bad_request_line", test_sec_bad_request_line},
    {"smuggling_cl_garbage", test_sec_cl_garbage},
    {"smuggling_chunked_bare_lf", test_sec_chunked_bare_lf},
    {"legit_request_unaffected", test_sec_legit_request_still_works},
    {"response_framing_headers", test_sec_response_framing_headers},
    {NULL, NULL},
};

#else /* _WIN32 */

static int test_sec_response_framing_headers(void)
{
    cfw_sec_server ss;
    cfw_fd_t client;
    char response[4096];
    size_t len = 0;
    ssize_t n;

    CFW_TEST_ASSERT_EQ(cfw_sec_setup(&ss), 0);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_sec_connect(&client,
                                       cfw_server_port(&ss.server)),
                       0);
    n = cfw_sec_roundtrip(
        &ss, &client,
        "GET /evil HTTP/1.1\r\nHost: x\r\n\r\n",
        response, sizeof(response), &len);
    CFW_TEST_ASSERT_TRUE(n > 0);
    /* Transfer-Encoding must be stripped entirely. */
    CFW_TEST_ASSERT_TRUE(!cfw_sec_contains(response, len,
                                           "Transfer-Encoding"));
    /* The bogus Content-Length must not survive; the server emits the
     * correct one for the 6-byte body. */
    CFW_TEST_ASSERT_TRUE(!cfw_sec_contains(response, len, "999999"));
    CFW_TEST_ASSERT_TRUE(cfw_sec_contains(response, len,
                                          "Content-Length: 6"));
    CFW_TEST_ASSERT_TRUE(cfw_sec_contains(response, len, "routed"));

    cfw_fd_close(&client);
    cfw_sec_teardown(&ss);
    return 0;
}

const cfw_test_case cfw_test_security_cases[] = {
    {NULL, NULL},
};

#endif /* !_WIN32 */
