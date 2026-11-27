/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <string.h>

static cfw_strview_t cfw_test_sv(const char *text)
{
    cfw_strview_t view;
    cfw_strview_from_cstr(&view, text);
    return view;
}

static cfw_status_t cfw_test_parse_head(const char *text,
                                        cfw_http_request_head_t *head)
{
    cfw_strview_t view = cfw_test_sv(text);
    return cfw_http_request_head_parse(&view, head);
}

static int cfw_test_router_add(cfw_router_t *router,
                               cfw_http_method_t method, const char *pattern,
                               void *user)
{
    cfw_strview_t view = cfw_test_sv(pattern);
    return cfw_router_add(router, method, &view, user, NULL) == CFW_OK
               ? 0
               : 1;
}

static int cfw_test_contains(const char *haystack, size_t haystack_len,
                             const char *needle, size_t needle_len)
{
    size_t i;
    if (needle_len == 0 || haystack_len < needle_len) {
        return 0;
    }
    for (i = 0; i + needle_len <= haystack_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

typedef struct {
    int calls;
    cfw_http_status_t status;
    const char *body;
} cfw_test_handler_state;

static cfw_status_t cfw_test_handler(cfw_request_t *req, cfw_response_t *res,
                                     void *user)
{
    cfw_test_handler_state *state = user;
    cfw_strview_t body;
    (void)req;
    state->calls++;
    res->status = state->status;
    cfw_strview_from_cstr(&body, state->body);
    res->body.data = (const uint8_t *)body.data;
    res->body.size = body.size;
    return CFW_OK;
}

static cfw_status_t cfw_test_dispatch_run(
    cfw_dispatch_t *dispatch, const char *head_text, const char *body_text,
    cfw_http_header_t *headers, size_t header_capacity,
    cfw_route_param_t *params, size_t param_capacity, cfw_response_t *res)
{
    cfw_http_request_head_t head;
    cfw_span_t body;
    cfw_status_t st;

    st = cfw_test_parse_head(head_text, &head);
    CFW_TEST_ASSERT_EQ(st, CFW_OK);
    memset(&body, 0, sizeof(body));
    if (body_text != NULL) {
        cfw_span_init(&body, body_text, strlen(body_text));
    }
    memset(res, 0, sizeof(*res));
    res->headers = headers;
    res->header_capacity = header_capacity;
    return cfw_dispatch_request(dispatch, &head, &body, headers,
                                header_capacity, params, param_capacity, res);
}

static int test_dispatch_basic(void)
{
    cfw_arena_t *arena = cfw_arena_create(8192);
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t options;
    cfw_test_handler_state handler = {0, CFW_HTTP_STATUS_OK, "hello"};
    cfw_http_header_t res_headers[8];
    cfw_route_param_t params[8];
    cfw_response_t res;
    static const char head[] =
        "GET /hello HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";

    CFW_TEST_ASSERT_TRUE(arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&router, arena), CFW_OK);
    CFW_TEST_ASSERT_EQ(
        cfw_test_router_add(&router, CFW_HTTP_METHOD_GET, "/hello", NULL), 0);
    memset(&options, 0, sizeof(options));
    options.router = &router;
    options.handler = cfw_test_handler;
    options.handler_user = &handler;
    options.max_middleware = 4;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&dispatch, arena, &options), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_test_dispatch_run(&dispatch, head, NULL,
                                             res_headers, 8, params, 8, &res),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(handler.calls, 1);
    CFW_TEST_ASSERT_EQ(res.status, CFW_HTTP_STATUS_OK);
    CFW_TEST_ASSERT_EQ(res.body.size, 5);
    CFW_TEST_ASSERT_EQ(memcmp(res.body.data, "hello", 5), 0);

    cfw_arena_destroy(arena);
    return 0;
}

typedef struct {
    int calls;
    int id;
    int cancel;
    int fail;
    int post;
    int *log;
    int *log_pos;
} cfw_test_mw_state;

static cfw_status_t cfw_test_mw(cfw_request_t *req, cfw_response_t *res,
                                cfw_dispatch_next_t next,
                                cfw_dispatch_frame_t *frame, void *user)
{
    cfw_test_mw_state *state = user;
    (void)req;

    state->calls++;
    state->log[(*state->log_pos)++] = state->id;
    if (state->fail) {
        return CFW_ERROR_INTERNAL;
    }
    if (state->cancel) {
        res->status = CFW_HTTP_STATUS_FORBIDDEN;
        return CFW_OK; /* short-circuit: never call next */
    }
    {
        cfw_status_t st = next(frame);
        if (st != CFW_OK) {
            return st;
        }
    }
    if (state->post) {
        cfw_strview_t name = cfw_test_sv("X-Post");
        cfw_strview_t val = cfw_test_sv("yes");
        cfw_response_header_add(res, &name, &val);
    }
    return CFW_OK;
}

static int test_dispatch_middleware_order_cancel(void)
{
    cfw_arena_t *arena = cfw_arena_create(8192);
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t options;
    cfw_test_handler_state handler = {0, CFW_HTTP_STATUS_OK, "ok"};
    int log[8];
    int log_pos = 0;
    cfw_test_mw_state mw1 = {0, 1, 0, 0, 0, log, &log_pos};
    cfw_test_mw_state mw2 = {0, 2, 0, 0, 1, log, &log_pos};
    cfw_test_mw_state mw3 = {0, 3, 1, 0, 0, log, &log_pos}; /* cancels */
    cfw_http_header_t res_headers[8];
    cfw_route_param_t params[8];
    cfw_response_t res;
    static const char head[] = "GET /x HTTP/1.1\r\n\r\n";

    CFW_TEST_ASSERT_TRUE(arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&router, arena), CFW_OK);
    CFW_TEST_ASSERT_EQ(
        cfw_test_router_add(&router, CFW_HTTP_METHOD_GET, "/x", NULL), 0);
    memset(&options, 0, sizeof(options));
    options.router = &router;
    options.handler = cfw_test_handler;
    options.handler_user = &handler;
    options.max_middleware = 4;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&dispatch, arena, &options), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_dispatch_use(&dispatch, cfw_test_mw, &mw1),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_dispatch_use(&dispatch, cfw_test_mw, &mw2),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_dispatch_use(&dispatch, cfw_test_mw, &mw3),
                       CFW_OK);

    /* mw3 cancels: handler never runs; mw1/mw2 entered in order and
     * mw2's post-processing still runs after next() returns. */
    CFW_TEST_ASSERT_EQ(cfw_test_dispatch_run(&dispatch, head, NULL,
                                             res_headers, 8, params, 8, &res),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(handler.calls, 0);
    CFW_TEST_ASSERT_EQ(res.status, CFW_HTTP_STATUS_FORBIDDEN);
    CFW_TEST_ASSERT_EQ(log_pos, 3);
    CFW_TEST_ASSERT_EQ(log[0], 1);
    CFW_TEST_ASSERT_EQ(log[1], 2);
    CFW_TEST_ASSERT_EQ(log[2], 3);
    CFW_TEST_ASSERT_EQ(res.header_count, 1); /* X-Post from mw2 */

    cfw_arena_destroy(arena);
    return 0;
}

static int test_dispatch_no_route_and_errors(void)
{
    cfw_arena_t *arena = cfw_arena_create(8192);
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t options;
    cfw_test_handler_state handler = {0, CFW_HTTP_STATUS_OK, "ok"};
    cfw_http_header_t res_headers[8];
    cfw_route_param_t params[8];
    cfw_response_t res;
    static const char head[] = "GET /missing HTTP/1.1\r\n\r\n";
    static const char bad_head[] =
        "POST /x HTTP/1.1\r\n"
        "Content-Length: 5\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    static const char post_head[] =
        "POST /submit HTTP/1.1\r\nContent-Length: 5\r\n\r\n";

    CFW_TEST_ASSERT_TRUE(arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&router, arena), CFW_OK);
    CFW_TEST_ASSERT_EQ(
        cfw_test_router_add(&router, CFW_HTTP_METHOD_GET, "/x", NULL), 0);
    CFW_TEST_ASSERT_EQ(
        cfw_test_router_add(&router, CFW_HTTP_METHOD_POST, "/submit", NULL),
        0);
    memset(&options, 0, sizeof(options));
    options.router = &router;
    options.handler = cfw_test_handler;
    options.handler_user = &handler;
    options.max_middleware = 2;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&dispatch, arena, &options), CFW_OK);

    /* No route → 404. */
    CFW_TEST_ASSERT_EQ(cfw_test_dispatch_run(&dispatch, head, NULL,
                                             res_headers, 8, params, 8, &res),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(res.status, CFW_HTTP_STATUS_NOT_FOUND);
    CFW_TEST_ASSERT_EQ(handler.calls, 0);

    /* Framing conflict → 400. */
    CFW_TEST_ASSERT_EQ(cfw_test_dispatch_run(&dispatch, bad_head, NULL,
                                             res_headers, 8, params, 8, &res),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(res.status, CFW_HTTP_STATUS_BAD_REQUEST);

    /* Body size mismatch (CL=5, empty body) → 400. */
    CFW_TEST_ASSERT_EQ(cfw_test_dispatch_run(&dispatch, post_head, NULL,
                                             res_headers, 8, params, 8, &res),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(res.status, CFW_HTTP_STATUS_BAD_REQUEST);

    /* Matching body → handler runs. */
    CFW_TEST_ASSERT_EQ(cfw_test_dispatch_run(&dispatch, post_head, "12345",
                                             res_headers, 8, params, 8, &res),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(handler.calls, 1);
    CFW_TEST_ASSERT_EQ(res.status, CFW_HTTP_STATUS_OK);

    cfw_arena_destroy(arena);
    return 0;
}

typedef struct {
    int calls;
    int saw_unmatched;
    int cancel;
} cfw_test_unmatched_mw_state;

/* Middleware that observes whether the request matched a route,
 * optionally short-circuiting it (the cfw_static_serve pattern —
 * ADR 0065). */
static cfw_status_t cfw_test_unmatched_mw(cfw_request_t *req,
                                          cfw_response_t *res,
                                          cfw_dispatch_next_t next,
                                          cfw_dispatch_frame_t *frame,
                                          void *user)
{
    cfw_test_unmatched_mw_state *state = user;
    (void)res;

    state->calls++;
    if (req->match.route == NULL) {
        state->saw_unmatched++;
    }
    if (state->cancel) {
        res->status = CFW_HTTP_STATUS_OK;
        return CFW_OK; /* short-circuit: never call next */
    }
    return next(frame);
}

static int test_dispatch_middleware_unmatched(void)
{
    cfw_arena_t *arena = cfw_arena_create(8192);
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t options;
    cfw_test_handler_state handler = {0, CFW_HTTP_STATUS_OK, "ok"};
    cfw_test_unmatched_mw_state mw = {0, 0, 0};
    cfw_http_header_t res_headers[8];
    cfw_route_param_t params[8];
    cfw_response_t res;
    static const char head[] = "GET /missing HTTP/1.1\r\n\r\n";

    CFW_TEST_ASSERT_TRUE(arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&router, arena), CFW_OK);
    CFW_TEST_ASSERT_EQ(
        cfw_test_router_add(&router, CFW_HTTP_METHOD_GET, "/x", NULL), 0);
    memset(&options, 0, sizeof(options));
    options.router = &router;
    options.handler = cfw_test_handler;
    options.handler_user = &handler;
    options.max_middleware = 2;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&dispatch, arena, &options), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_dispatch_use(&dispatch, cfw_test_unmatched_mw,
                                        &mw),
                       CFW_OK);

    /* Pass-through: middleware sees the unmatched request, then the
     * chain terminal produces the 404. */
    CFW_TEST_ASSERT_EQ(cfw_test_dispatch_run(&dispatch, head, NULL,
                                             res_headers, 8, params, 8, &res),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(mw.calls, 1);
    CFW_TEST_ASSERT_EQ(mw.saw_unmatched, 1);
    CFW_TEST_ASSERT_EQ(res.status, CFW_HTTP_STATUS_NOT_FOUND);
    CFW_TEST_ASSERT_EQ(handler.calls, 0);

    /* Short-circuit: middleware answers the unmatched request itself
     * and the 404 never happens (cfw_static_serve interception). */
    mw.cancel = 1;
    CFW_TEST_ASSERT_EQ(cfw_test_dispatch_run(&dispatch, head, NULL,
                                             res_headers, 8, params, 8, &res),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(mw.calls, 2);
    CFW_TEST_ASSERT_EQ(res.status, CFW_HTTP_STATUS_OK);
    CFW_TEST_ASSERT_EQ(handler.calls, 0);

    cfw_arena_destroy(arena);
    return 0;
}

static cfw_status_t cfw_test_inspect_handler(cfw_request_t *req,
                                             cfw_response_t *res,
                                             void *user)
{
    cfw_strview_t value;
    cfw_strview_t name;
    (void)user;

    res->status = CFW_HTTP_STATUS_OK;

    /* Route param extraction. */
    name = cfw_test_sv("id");
    if (cfw_route_match_param(req->params, req->params_written, &name,
                              &value) != CFW_OK ||
        value.size != 2 || memcmp(value.data, "42", 2) != 0) {
        res->status = CFW_HTTP_STATUS_INTERNAL_SERVER_ERROR;
        return CFW_OK;
    }

    /* Header lookup is case-insensitive. */
    name = cfw_test_sv("x-custom");
    if (cfw_request_header(req, &name, &value) != CFW_OK ||
        value.size != 3 || memcmp(value.data, "yes", 3) != 0) {
        res->status = CFW_HTTP_STATUS_INTERNAL_SERVER_ERROR;
        return CFW_OK;
    }
    name = cfw_test_sv("X-CUSTOM");
    if (cfw_request_header(req, &name, &value) != CFW_OK) {
        res->status = CFW_HTTP_STATUS_INTERNAL_SERVER_ERROR;
        return CFW_OK;
    }

    /* Missing header fails. */
    name = cfw_test_sv("x-missing");
    if (cfw_request_header(req, &name, &value) !=
        CFW_ERROR_INVALID_ARGUMENT) {
        res->status = CFW_HTTP_STATUS_INTERNAL_SERVER_ERROR;
    }
    return CFW_OK;
}

static int test_dispatch_params_and_headers(void)
{
    cfw_arena_t *arena = cfw_arena_create(8192);
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t options;
    cfw_http_header_t res_headers[8];
    cfw_route_param_t params[8];
    cfw_response_t res;
    static const char head[] =
        "GET /users/42/profile HTTP/1.1\r\n"
        "X-Custom: yes\r\n"
        "\r\n";

    CFW_TEST_ASSERT_TRUE(arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&router, arena), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_router_add(&router, CFW_HTTP_METHOD_GET,
                                           "/users/:id/profile", (void *)"route"),
                       0);
    memset(&options, 0, sizeof(options));
    options.router = &router;
    options.handler = cfw_test_inspect_handler;
    options.handler_user = NULL;
    options.max_middleware = 1;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&dispatch, arena, &options), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_test_dispatch_run(&dispatch, head, NULL,
                                             res_headers, 8, params, 8, &res),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(res.status, CFW_HTTP_STATUS_OK);

    cfw_arena_destroy(arena);
    return 0;
}

static int test_dispatch_response_format(void)
{
    cfw_arena_t *arena = cfw_arena_create(8192);
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t options;
    cfw_test_handler_state handler = {0, CFW_HTTP_STATUS_OK, "body-data"};
    cfw_http_header_t res_headers[8];
    cfw_route_param_t params[8];
    cfw_response_t res;
    cfw_request_t req;
    char buf[1024];
    size_t written = 0;
    static const char head[] = "GET /h HTTP/1.1\r\n\r\n";

    CFW_TEST_ASSERT_TRUE(arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&router, arena), CFW_OK);
    CFW_TEST_ASSERT_EQ(
        cfw_test_router_add(&router, CFW_HTTP_METHOD_GET, "/h", NULL), 0);
    memset(&options, 0, sizeof(options));
    options.router = &router;
    options.handler = cfw_test_handler;
    options.handler_user = &handler;
    options.max_middleware = 1;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&dispatch, arena, &options), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_dispatch_run(&dispatch, head, NULL,
                                             res_headers, 8, params, 8, &res),
                       CFW_OK);

    memset(&req, 0, sizeof(req));
    req.method = CFW_HTTP_METHOD_GET;
    req.version.major = 1;
    req.version.minor = 1;

    /* Auto Content-Length for a GET with body. */
    CFW_TEST_ASSERT_EQ(cfw_dispatch_response_head_format(&req, &res, buf,
                                                         sizeof(buf),
                                                         &written),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(
        cfw_test_contains(buf, written, "Content-Length: 9", 17));
    CFW_TEST_ASSERT_TRUE(
        cfw_test_contains(buf, written, "\r\n\r\n", 4));

    /* HEAD mirrors GET headers: Content-Length is still emitted even
     * though the body itself is suppressed at the transport layer. */
    req.method = CFW_HTTP_METHOD_HEAD;
    written = 0;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_response_head_format(&req, &res, buf,
                                                         sizeof(buf),
                                                         &written),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(
        cfw_test_contains(buf, written, "Content-Length: 9", 17));

    /* Explicit Content-Length wins over auto. */
    req.method = CFW_HTTP_METHOD_GET;
    {
        cfw_strview_t n = cfw_test_sv("Content-Length");
        cfw_strview_t v = cfw_test_sv("3");
        res.headers[0].name = n;
        res.headers[0].value = v;
        res.header_count = 1;
        written = 0;
        CFW_TEST_ASSERT_EQ(cfw_dispatch_response_head_format(
                               &req, &res, buf, sizeof(buf), &written),
                           CFW_OK);
        CFW_TEST_ASSERT_TRUE(
            cfw_test_contains(buf, written, "Content-Length: 3", 17));
        /* Exactly one Content-Length occurrence. */
        {
            size_t count = 0;
            size_t i;
            for (i = 0; i + 14 <= written; i++) {
                if (memcmp(buf + i, "Content-Length", 14) == 0) {
                    count++;
                }
            }
            CFW_TEST_ASSERT_EQ(count, 1);
        }
        res.header_count = 0;
    }

    /* Tiny buffer → INVALID_ARGUMENT. */
    written = 0;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_response_head_format(&req, &res, buf, 4,
                                                         &written),
                       CFW_ERROR_INVALID_ARGUMENT);

    cfw_arena_destroy(arena);
    return 0;
}

static int test_dispatch_validation(void)
{
    cfw_dispatch_t dispatch;
    cfw_response_t res;
    cfw_http_request_head_t head;
    cfw_span_t body;
    cfw_http_header_t headers[4];
    cfw_route_param_t params[4];

    memset(&dispatch, 0, sizeof(dispatch));
    memset(&res, 0, sizeof(res));
    memset(&head, 0, sizeof(head));
    memset(&body, 0, sizeof(body));

    CFW_TEST_ASSERT_EQ(cfw_dispatch_request(NULL, &head, &body, headers, 4,
                                            params, 4, &res),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_dispatch_request(&dispatch, &head, &body, headers,
                                            4, params, 4, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_dispatch_request(&dispatch, &head, &body, NULL,
                                            4, params, 4, &res),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* Init validation. */
    {
        cfw_arena_t *arena = cfw_arena_create(1024);
        cfw_dispatch_options_t options;
        memset(&options, 0, sizeof(options));
        CFW_TEST_ASSERT_EQ(cfw_dispatch_init(NULL, arena, &options),
                           CFW_ERROR_INVALID_ARGUMENT);
        CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&dispatch, NULL, &options),
                           CFW_ERROR_INVALID_ARGUMENT);
        options.router = (cfw_router_t *)1;
        options.handler = (cfw_handler_fn)1;
        options.max_middleware = 0;
        CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&dispatch, arena, &options),
                           CFW_ERROR_INVALID_ARGUMENT);
        cfw_arena_destroy(arena);
    }

    /* Middleware capacity enforcement. */
    {
        cfw_arena_t *arena = cfw_arena_create(8192);
        cfw_router_t router;
        cfw_dispatch_t d;
        cfw_dispatch_options_t options;
        cfw_test_handler_state handler = {0, 200, "x"};
        memset(&options, 0, sizeof(options));
        CFW_TEST_ASSERT_EQ(cfw_router_init(&router, arena), CFW_OK);
        options.router = &router;
        options.handler = cfw_test_handler;
        options.handler_user = &handler;
        options.max_middleware = 1;
        CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&d, arena, &options), CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_dispatch_use(&d, cfw_test_mw, NULL), CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_dispatch_use(&d, cfw_test_mw, NULL),
                           CFW_ERROR_INVALID_ARGUMENT);
        CFW_TEST_ASSERT_EQ(cfw_dispatch_use(&d, NULL, NULL),
                           CFW_ERROR_INVALID_ARGUMENT);
        cfw_arena_destroy(arena);
    }
    return 0;
}


static cfw_status_t cfw_test_route_handler(cfw_request_t *req,
                                           cfw_response_t *res, void *user)
{
    cfw_strview_t body;
    (void)req;
    res->status = CFW_HTTP_STATUS_ACCEPTED;
    cfw_strview_from_cstr(&body, (const char *)user);
    res->body.data = (const uint8_t *)body.data;
    res->body.size = body.size;
    return CFW_OK;
}

static int test_dispatch_route_handlers(void)
{
    cfw_arena_t *arena = cfw_arena_create(8192);
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t options;
    cfw_test_handler_state global = {0, CFW_HTTP_STATUS_OK, "global"};
    cfw_http_header_t res_headers[8];
    cfw_route_param_t params[8];
    cfw_response_t res;
    static const char head_bound[] = "GET /bound HTTP/1.1\r\n\r\n";
    static const char head_plain[] = "GET /plain HTTP/1.1\r\n\r\n";

    CFW_TEST_ASSERT_TRUE(arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&router, arena), CFW_OK);
    memset(&options, 0, sizeof(options));
    options.router = &router;
    options.handler = cfw_test_handler;
    options.handler_user = &global;
    options.max_middleware = 2;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&dispatch, arena, &options), CFW_OK);

    /* Bound route uses its own handler. */
    {
        cfw_strview_t pattern = cfw_test_sv("/bound");
        CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(
                               &dispatch, CFW_HTTP_METHOD_GET, &pattern,
                               cfw_test_route_handler,
                               (void *)"bound-body"),
                           CFW_OK);
    }
    /* Plain route falls back to the global handler. */
    CFW_TEST_ASSERT_EQ(cfw_test_router_add(&router, CFW_HTTP_METHOD_GET,
                                           "/plain", NULL),
                       0);

    CFW_TEST_ASSERT_EQ(cfw_test_dispatch_run(&dispatch, head_bound, NULL,
                                             res_headers, 8, params, 8, &res),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(res.status, CFW_HTTP_STATUS_ACCEPTED);
    CFW_TEST_ASSERT_EQ(res.body.size, 10);
    CFW_TEST_ASSERT_EQ(memcmp(res.body.data, "bound-body", 10), 0);
    CFW_TEST_ASSERT_EQ(global.calls, 0);

    CFW_TEST_ASSERT_EQ(cfw_test_dispatch_run(&dispatch, head_plain, NULL,
                                             res_headers, 8, params, 8, &res),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(res.status, CFW_HTTP_STATUS_OK);
    CFW_TEST_ASSERT_EQ(global.calls, 1);

    /* Validation: NULL handler rejected. */
    {
        cfw_strview_t pattern = cfw_test_sv("/nope");
        CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(
                               &dispatch, CFW_HTTP_METHOD_GET, &pattern,
                               NULL, NULL),
                           CFW_ERROR_INVALID_ARGUMENT);
    }

    cfw_arena_destroy(arena);
    return 0;
}

static cfw_status_t cfw_test_stream_fn(void *user, cfw_span_t *chunk,
                                       int *eof)
{
    (void)user;
    (void)chunk;
    *eof = 1;
    return CFW_OK;
}

static cfw_status_t cfw_test_stream_handler(cfw_request_t *req,
                                            cfw_response_t *res, void *user)
{
    (void)req;
    (void)user;
    res->status = CFW_HTTP_STATUS_OK;
    res->stream = cfw_test_stream_fn;
    return CFW_OK;
}

/* Reusing a cfw_response_t across dispatch calls must not leak a stale
 * stream source into a response that does not set one. */
static int test_dispatch_response_reset(void)
{
    cfw_arena_t *arena = cfw_arena_create(8192);
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t options;
    cfw_test_handler_state state = {0, CFW_HTTP_STATUS_OK, "plain"};
    cfw_http_header_t res_headers[8];
    cfw_route_param_t params[8];
    cfw_response_t res;
    static const char head_stream[] = "GET /s HTTP/1.1\r\n\r\n";
    static const char head_plain[] = "GET /p HTTP/1.1\r\n\r\n";

    CFW_TEST_ASSERT_TRUE(arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&router, arena), CFW_OK);
    memset(&options, 0, sizeof(options));
    options.router = &router;
    options.handler = cfw_test_handler;
    options.handler_user = &state;
    options.max_middleware = 1;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&dispatch, arena, &options),
                       CFW_OK);

    {
        cfw_strview_t pattern = cfw_test_sv("/s");
        CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(
                               &dispatch, CFW_HTTP_METHOD_GET, &pattern,
                               cfw_test_stream_handler, NULL),
                           CFW_OK);
    }
    CFW_TEST_ASSERT_EQ(cfw_test_router_add(&router, CFW_HTTP_METHOD_GET,
                                           "/p", NULL),
                       0);

    CFW_TEST_ASSERT_EQ(cfw_test_dispatch_run(&dispatch, head_stream, NULL,
                                             res_headers, 8, params, 8, &res),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(res.stream != NULL);

    CFW_TEST_ASSERT_EQ(cfw_test_dispatch_run(&dispatch, head_plain, NULL,
                                             res_headers, 8, params, 8, &res),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(res.stream == NULL);
    CFW_TEST_ASSERT_TRUE(res.stream_user == NULL);

    cfw_arena_destroy(arena);
    return 0;
}

const cfw_test_case cfw_test_dispatch_cases[] = {
    {"dispatch_basic", test_dispatch_basic},
    {"dispatch_middleware_order_cancel", test_dispatch_middleware_order_cancel},
    {"dispatch_no_route_and_errors", test_dispatch_no_route_and_errors},
    {"dispatch_middleware_unmatched", test_dispatch_middleware_unmatched},
    {"dispatch_params_and_headers", test_dispatch_params_and_headers},
    {"dispatch_response_format", test_dispatch_response_format},
    {"dispatch_validation", test_dispatch_validation},
    {"dispatch_route_handlers", test_dispatch_route_handlers},
    {"dispatch_response_reset", test_dispatch_response_reset},
    {NULL, NULL},
};
