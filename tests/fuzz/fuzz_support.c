/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "fuzz_support.h"

#include <string.h>

static cfw_status_t cfw_fuzz_handler(cfw_request_t *req, cfw_response_t *res,
                                     void *user)
{
    (void)req;
    (void)user;
    res->status = CFW_HTTP_STATUS_OK;
    return CFW_OK;
}

cfw_status_t cfw_fuzz_dispatch_init(cfw_fuzz_dispatch_t *ctx)
{
    cfw_dispatch_options_t dopt = {0};
    cfw_strview_t pattern;

    memset(ctx, 0, sizeof(*ctx));
    ctx->arena = cfw_arena_create(64u * 1024u);
    if (ctx->arena == NULL) {
        return CFW_ERROR_NOMEM;
    }
    if (cfw_router_init(&ctx->router, ctx->arena) != CFW_OK) {
        goto fail;
    }
    memset(&dopt, 0, sizeof(dopt));
    dopt.router = &ctx->router;
    dopt.handler = cfw_fuzz_handler;
    dopt.max_middleware = 1;
    if (cfw_dispatch_init(&ctx->dispatch, ctx->arena, &dopt) != CFW_OK) {
        goto fail;
    }
    /* Route table covering literal, single-param, multi-param, and
     * method-specific patterns so parsed fuzz inputs exercise the
     * full matcher, param extraction, and method filtering. */
    {
        static const struct {
            cfw_http_method_t method;
            const char *pattern;
        } routes[] = {
            {CFW_HTTP_METHOD_GET, "/"},
            {CFW_HTTP_METHOD_GET, "/users/:id"},
            {CFW_HTTP_METHOD_GET, "/items/:id/comments/:cid"},
            {CFW_HTTP_METHOD_POST, "/submit"},
            {CFW_HTTP_METHOD_PUT, "/upload/:name"},
            {CFW_HTTP_METHOD_DELETE, "/admin/:section"},
            {CFW_HTTP_METHOD_GET, "/files/:name"},
        };
        size_t r;
        for (r = 0; r < sizeof(routes) / sizeof(routes[0]); r++) {
            if (cfw_strview_from_cstr(&pattern, routes[r].pattern) !=
                    CFW_OK ||
                cfw_dispatch_add_route(&ctx->dispatch, routes[r].method,
                                       &pattern, cfw_fuzz_handler,
                                       NULL) != CFW_OK) {
                goto fail;
            }
        }
    }
    return CFW_OK;

fail:
    cfw_arena_destroy(ctx->arena);
    ctx->arena = NULL;
    return CFW_ERROR_INTERNAL;
}

void cfw_fuzz_dispatch_free(cfw_fuzz_dispatch_t *ctx)
{
    if (ctx->arena != NULL) {
        cfw_arena_destroy(ctx->arena);
        ctx->arena = NULL;
    }
}

void cfw_fuzz_http_one(const void *data, size_t len,
                       const cfw_fuzz_dispatch_t *dispatch_ctx)
{
    cfw_strview_t view;
    cfw_http_request_head_t head;
    cfw_http_message_framing_t framing;
    cfw_http_chunked_decoder_t decoder;
    cfw_span_t span;
    cfw_span_t output;
    cfw_http1_server_options_t options;
    cfw_http1_server_t server;
    cfw_http1_event_t event;
    size_t consumed;
    cfw_status_t st;
    int finished = 0;
    int i;

    view.data = data;
    view.size = len;

    /* Head parser must never crash; any status is acceptable. */
    st = cfw_http_request_head_parse(&view, &head);

    if (st == CFW_OK) {
        /* Framing on a successfully parsed head. */
        (void)cfw_http_request_framing(&head, &framing);

        /* Dispatch pipeline on a successfully parsed head. */
        if (dispatch_ctx != NULL) {
            cfw_http_header_t res_headers[8];
            cfw_route_param_t params[4];
            cfw_response_t res;
            cfw_span_t empty_body;

            memset(&res, 0, sizeof(res));
            res.headers = res_headers;
            res.header_capacity = 8;
            memset(&empty_body, 0, sizeof(empty_body));
            (void)cfw_dispatch_request(&dispatch_ctx->dispatch, &head,
                                       &empty_body, res_headers, 8,
                                       params, 4, &res);
        }
    }

    /* Response head parser over the same bytes (cheap extra surface). */
    {
        cfw_http_response_head_t res_head;
        cfw_http_message_framing_t res_framing;

        st = cfw_http_response_head_parse(&view, &res_head);
        if (st == CFW_OK) {
            (void)cfw_http_response_framing(&res_head, CFW_HTTP_METHOD_GET,
                                            &res_framing);
        }
    }

    /* Chunked decoder over the raw bytes in one shot. */
    cfw_http_chunked_decoder_init(&decoder);
    cfw_span_init(&span, data, len);
    for (i = 0; i < 64 && !finished; i++) {
        st = cfw_http_chunked_decode(&decoder, &span, &output,
                                     &consumed, &finished);
        if (st != CFW_OK) {
            break;
        }
        if (consumed == 0 && output.size == 0) {
            break;
        }
        span.data += consumed;
        span.size -= consumed;
        if (span.size == 0) {
            break;
        }
    }

    /* Full server pipeline: feed everything, drain events. */
    options.max_head_size = 8192;
    options.max_body_size = 0;
    options.max_pending_size = 1u << 20;
    if (cfw_http1_server_init(&server, &options) == CFW_OK) {
        cfw_span_init(&span, data, len);
        (void)cfw_http1_server_feed(&server, &span);
        for (i = 0; i < 256; i++) {
            st = cfw_http1_server_next_event(&server, &event);
            if (st != CFW_OK) {
                break;
            }
        }
        cfw_http1_server_free(&server);
    }
}
