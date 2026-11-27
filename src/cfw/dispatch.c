/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/dispatch.h>

#include <stdint.h>
#include <string.h>

struct cfw_dispatch_frame {
    const cfw_dispatch_t *dispatch;
    cfw_request_t *req;
    cfw_response_t *res;
    size_t next_index;
};

cfw_status_t cfw_dispatch_init(cfw_dispatch_t *dispatch, cfw_arena_t *arena,
                               const cfw_dispatch_options_t *options)
{
    void *storage = NULL;

    if (dispatch == NULL || arena == NULL || options == NULL ||
        options->router == NULL || options->handler == NULL ||
        options->max_middleware == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    if (options->max_middleware >
        SIZE_MAX / sizeof(cfw_dispatch_middleware_t)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (cfw_arena_alloc_zeroed(
            arena,
            options->max_middleware *
                sizeof(cfw_dispatch_middleware_t),
            &storage) != CFW_OK) {
        return CFW_ERROR_NOMEM;
    }

    memset(dispatch, 0, sizeof(*dispatch));
    dispatch->router = options->router;
    dispatch->middleware = storage;
    dispatch->middleware_capacity = options->max_middleware;
    dispatch->handler = options->handler;
    dispatch->handler_user = options->handler_user;
    dispatch->error_handler = options->error_handler;
    dispatch->error_user = options->error_user;
    dispatch->arena = arena;
    return CFW_OK;
}

cfw_status_t cfw_dispatch_add_route(cfw_dispatch_t *dispatch,
                                    cfw_http_method_t method,
                                    const cfw_strview_t *pattern,
                                    cfw_handler_fn handler, void *user)
{
    cfw_route_binding_t *binding = NULL;

    if (dispatch == NULL || dispatch->router == NULL || pattern == NULL ||
        handler == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (cfw_arena_alloc(dispatch->arena, sizeof(*binding),
                        (void **)&binding) != CFW_OK) {
        return CFW_ERROR_NOMEM;
    }
    binding->handler = handler;
    binding->user = user;
    return cfw_router_add_ex(dispatch->router, method, pattern, user,
                             binding, NULL);
}

cfw_status_t cfw_dispatch_use(cfw_dispatch_t *dispatch,
                              cfw_middleware_fn fn, void *user)
{
    if (dispatch == NULL || fn == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (dispatch->middleware_count >= dispatch->middleware_capacity) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    dispatch->middleware[dispatch->middleware_count].fn = fn;
    dispatch->middleware[dispatch->middleware_count].user = user;
    dispatch->middleware_count++;
    return CFW_OK;
}

cfw_status_t cfw_response_header_add(cfw_response_t *res,
                                     const cfw_strview_t *name,
                                     const cfw_strview_t *value)
{
    cfw_http_header_t *slot;

    if (res == NULL || name == NULL || value == NULL ||
        name->size == 0 || name->data == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (res->header_count >= res->header_capacity) {
        return CFW_ERROR_NOMEM;
    }
    slot = &res->headers[res->header_count++];
    slot->name = *name;
    slot->value = *value;
    return CFW_OK;
}

static void cfw_dispatch_default_error(cfw_request_t *req,
                                       cfw_response_t *res,
                                       cfw_http_status_t status_hint,
                                       void *user)
{
    (void)req;
    (void)user;
    res->status = status_hint;
    res->header_count = 0;
    memset(&res->body, 0, sizeof(res->body));
}

static cfw_status_t cfw_dispatch_fail(const cfw_dispatch_t *dispatch,
                                      cfw_request_t *req,
                                      cfw_response_t *res,
                                      cfw_http_status_t hint)
{
    cfw_dispatch_error_fn fn =
        dispatch->error_handler != NULL ? dispatch->error_handler
                                        : cfw_dispatch_default_error;
    void *user =
        dispatch->error_handler != NULL ? dispatch->error_user : NULL;
    fn(req, res, hint, user);
    return CFW_OK;
}

/* Produces the route-miss response at the end of the middleware
 * chain: 405 + Allow when the path matched under other methods,
 * else 404. Runs as the chain terminal so middleware may
 * short-circuit unmatched requests first (ADR 0065). */
static cfw_status_t cfw_dispatch_route_miss(const cfw_dispatch_t *dispatch,
                                            cfw_request_t *req,
                                            cfw_response_t *res)
{
    if (req->match.allowed_methods != 0) {
        /* Path exists under other method(s): 405 + Allow. */
        cfw_status_t fst = cfw_dispatch_fail(
            dispatch, req, res, CFW_HTTP_STATUS_METHOD_NOT_ALLOWED);
        if (fst == CFW_OK) {
            static const char allow_name[] = "Allow";
            char *allow_value;
            size_t av_len = 0;
            int m;
            cfw_strview_t hn, hv;
            for (m = 1; m <= 9; m++) {
                if ((req->match.allowed_methods & (1u << m)) != 0) {
                    av_len += strlen(cfw_http_method_name(
                        (cfw_http_method_t)m)) + 2;
                }
            }
            if (av_len != 0 &&
                cfw_arena_alloc(dispatch->arena, av_len,
                                (void **)&allow_value) == CFW_OK) {
                char *w = allow_value;
                for (m = 1; m <= 9; m++) {
                    const char *mn;
                    size_t ml;
                    if ((req->match.allowed_methods & (1u << m)) == 0) {
                        continue;
                    }
                    mn = cfw_http_method_name((cfw_http_method_t)m);
                    ml = strlen(mn);
                    memcpy(w, mn, ml);
                    w += ml;
                    *w++ = ',';
                    *w++ = ' ';
                }
                w -= 2; /* trailing ", " */
                cfw_strview_init(&hn, allow_name,
                                 sizeof(allow_name) - 1);
                cfw_strview_init(&hv, allow_value,
                                 (size_t)(w - allow_value));
                cfw_response_header_add(res, &hn, &hv);
            }
        }
        return fst;
    }
    return cfw_dispatch_fail(dispatch, req, res,
                             CFW_HTTP_STATUS_NOT_FOUND);
}

static cfw_status_t cfw_dispatch_chain_next(cfw_dispatch_frame_t *frame)
{
    const cfw_dispatch_t *dispatch = frame->dispatch;
    size_t index = frame->next_index++;

    if (index < dispatch->middleware_count) {
        return dispatch->middleware[index].fn(
            frame->req, frame->res, cfw_dispatch_chain_next, frame,
            dispatch->middleware[index].user);
    }
    if (frame->req->match.route == NULL) {
        return cfw_dispatch_route_miss(dispatch, frame->req,
                                       frame->res);
    }
    {
        const cfw_route_binding_t *binding =
            (const cfw_route_binding_t *)frame->req->match.ex;
        if (binding != NULL) {
            return binding->handler(frame->req, frame->res,
                                    binding->user);
        }
    }
    return dispatch->handler(frame->req, frame->res,
                             dispatch->handler_user);
}

static int cfw_dispatch_body_consistent(const cfw_http_message_framing_t *f,
                                        const cfw_span_t *body)
{
    size_t size = body != NULL ? body->size : 0;

    switch (f->framing) {
    case CFW_HTTP_BODY_NONE:
        return size == 0;
    case CFW_HTTP_BODY_CONTENT_LENGTH:
        return f->content_length <= SIZE_MAX &&
               (uint64_t)size == f->content_length;
    case CFW_HTTP_BODY_CHUNKED:
        return 1;
    }
    return 0;
}

cfw_status_t cfw_dispatch_request(
    const cfw_dispatch_t *dispatch, const cfw_http_request_head_t *head,
    const cfw_span_t *body, cfw_http_header_t *headers,
    size_t header_capacity, cfw_route_param_t *params,
    size_t param_capacity, cfw_response_t *res)
{
    return cfw_dispatch_request_ex(dispatch, head, body, headers,
                                   header_capacity, params, param_capacity,
                                   res, NULL);
}

cfw_status_t cfw_dispatch_request_ex(
    const cfw_dispatch_t *dispatch, const cfw_http_request_head_t *head,
    const cfw_span_t *body, cfw_http_header_t *headers,
    size_t header_capacity, cfw_route_param_t *params,
    size_t param_capacity, cfw_response_t *res,
    const cfw_dispatch_request_info_t *info)
{
    cfw_request_t req;
    cfw_http_message_framing_t framing;
    cfw_strview_t path;
    size_t cursor = 0;
    cfw_dispatch_frame_t frame;
    cfw_status_t status;

    if (dispatch == NULL || head == NULL || res == NULL ||
        (header_capacity != 0 && headers == NULL) ||
        (param_capacity != 0 && params == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    /* Reset the response; the caller-provided header scratch is kept. */
    res->status = CFW_HTTP_STATUS_OK;
    res->header_count = 0;
    memset(&res->body, 0, sizeof(res->body));
    res->stream = NULL;
    res->stream_user = NULL;
    res->stream_size = CFW_STREAM_SIZE_UNKNOWN;
    res->stream_end = NULL;
    res->user = NULL;

    memset(&req, 0, sizeof(req));
    req.method = head->request_line.method;
    req.target = head->request_line.target;
    req.version = head->request_line.version;
    req.headers = headers;
    req.body = body != NULL ? *body : (cfw_span_t){0};
    if (info != NULL) {
        if (info->remote_addr != NULL) {
            req.remote_addr = *info->remote_addr;
        }
        req.upload = info->upload;
        req.tls_conn = info->tls_conn;
    }

    /* Transport-independent validation: framing rules and body size.
     * When the transport streamed the body to an upload sink, req.body
     * is legitimately empty and consistency is the transport's job. */
    if (cfw_http_request_framing(head, &framing) != CFW_OK ||
        (req.upload == NULL &&
         !cfw_dispatch_body_consistent(&framing, body))) {
        return cfw_dispatch_fail(dispatch, &req, res,
                                 CFW_HTTP_STATUS_BAD_REQUEST);
    }

    /* Routing path; targets without a path component simply match
     * nothing (404 via the error handler). */
    if (cfw_http_target_path(&head->request_line.target, &path) != CFW_OK) {
        memset(&path, 0, sizeof(path));
    }
    req.path = path;

    /* Parse header fields into caller scratch. */
    for (;;) {
        cfw_http_header_t field;
        int done = 0;
        if (cfw_http_headers_next(&head->headers, &cursor, &field,
                                  &done) != CFW_OK) {
            return cfw_dispatch_fail(dispatch, &req, res,
                                     CFW_HTTP_STATUS_BAD_REQUEST);
        }
        if (done) {
            break;
        }
        if (req.header_count < header_capacity) {
            headers[req.header_count++] = field;
        } else {
            req.headers_dropped++;
        }
    }

    /* Route match. */
    if (cfw_router_match(dispatch->router, req.method, &req.path, params,
                         param_capacity, &req.match) != CFW_OK) {
        return CFW_ERROR_INTERNAL;
    }
    req.params = params;
    req.params_written = req.match.params_written;
    req.route_user = req.match.user;

    /* Middleware chain → terminal handler (or route-miss response
     * when nothing matched; middleware may intercept first — ADR
     * 0065). */
    frame.dispatch = dispatch;
    frame.req = &req;
    frame.res = res;
    frame.next_index = 0;
    status = cfw_dispatch_chain_next(&frame);
    if (status != CFW_OK) {
        return cfw_dispatch_fail(dispatch, &req, res,
                                 CFW_HTTP_STATUS_INTERNAL_SERVER_ERROR);
    }
    return CFW_OK;
}

cfw_status_t cfw_request_header(const cfw_request_t *req,
                                const cfw_strview_t *name,
                                cfw_strview_t *value)
{
    size_t i;

    if (req == NULL || name == NULL || value == NULL || name->size == 0 ||
        name->data == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < req->header_count; i++) {
        int equal = 0;
        if (req->headers[i].name.size == name->size &&
            cfw_strview_equal_nocase(&req->headers[i].name, name,
                                     &equal) == CFW_OK &&
            equal) {
            *value = req->headers[i].value;
            return CFW_OK;
        }
    }
    return CFW_ERROR_INVALID_ARGUMENT;
}

static int cfw_dispatch_has_framing_header(const cfw_response_t *res,
                                           const char *name, size_t len)
{
    cfw_strview_t wanted;
    size_t i;

    memset(&wanted, 0, sizeof(wanted));
    wanted.data = name;
    wanted.size = len;
    for (i = 0; i < res->header_count; i++) {
        int equal = 0;
        if (cfw_strview_equal_nocase(&res->headers[i].name, &wanted,
                                     &equal) == CFW_OK &&
            equal) {
            return 1;
        }
    }
    return 0;
}

static cfw_status_t cfw_dispatch_write(char *buffer, size_t capacity,
                                       size_t *offset, const void *data,
                                       size_t size)
{
    if (size > capacity - *offset) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    memcpy(buffer + *offset, data, size);
    *offset += size;
    return CFW_OK;
}

static cfw_status_t cfw_dispatch_write_u64(char *buffer, size_t capacity,
                                           size_t *offset, uint64_t value)
{
    char digits[20];
    size_t count = 0;
    size_t i;

    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    for (i = 0; i < count; i++) {
        char c = digits[count - 1 - i];
        if (cfw_dispatch_write(buffer, capacity, offset, &c, 1) != CFW_OK) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
    }
    return CFW_OK;
}

cfw_status_t cfw_dispatch_response_head_format(
    const cfw_request_t *req, const cfw_response_t *res, char *buffer,
    size_t capacity, size_t *written)
{
    static const char crlf[] = "\r\n";
    cfw_http_version_t version;
    size_t offset = 0;
    size_t i;
    int need_length;
    int body_allowed;
    uint64_t declared_size;
    cfw_status_t st;
    size_t n;

    if (req == NULL || res == NULL || buffer == NULL || written == NULL ||
        (res->header_count != 0 && res->headers == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    version = req->version;
    /* Responses that may carry a body always get a Content-Length —
     * "0" for empty bodies — unless the caller supplied framing
     * headers. HEAD mirrors the GET decision: it emits the same
     * headers (body bytes are suppressed at the transport layer). A
     * declared stream_size takes precedence over the (empty) body
     * span for streams and for HEAD, which never installs one. */
    body_allowed = cfw_http_response_has_body(req->method, res->status) ||
                   (req->method == CFW_HTTP_METHOD_HEAD &&
                    cfw_http_response_has_body(CFW_HTTP_METHOD_GET,
                                               res->status));
    need_length = body_allowed &&
                  !cfw_dispatch_has_framing_header(
                      res, "content-length",
                      sizeof("content-length") - 1) &&
                  !cfw_dispatch_has_framing_header(
                      res, "transfer-encoding",
                      sizeof("transfer-encoding") - 1);
    if (need_length &&
        (res->stream != NULL || req->method == CFW_HTTP_METHOD_HEAD) &&
        res->stream_size != CFW_STREAM_SIZE_UNKNOWN) {
        declared_size = res->stream_size;
    } else {
        declared_size = (uint64_t)res->body.size;
    }

    /* Status line, written directly into the output buffer. */
    n = 0;
    st = cfw_http_status_line_format(&version, res->status, buffer,
                                     capacity, &n);
    if (st != CFW_OK) {
        return st == CFW_ERROR_INVALID_ARGUMENT ? CFW_ERROR_INVALID_ARGUMENT
                                                : CFW_ERROR_INTERNAL;
    }
    offset += n;

    /* Caller headers. */
    for (i = 0; i < res->header_count; i++) {
        n = 0;
        st = cfw_http_header_line_format(&res->headers[i].name,
                                         &res->headers[i].value,
                                         buffer + offset, capacity - offset,
                                         &n);
        if (st != CFW_OK) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        offset += n;
    }

    /* Auto Content-Length. */
    if (need_length) {
        static const char cl[] = "Content-Length: ";
        if (cfw_dispatch_write(buffer, capacity, &offset, cl,
                               sizeof(cl) - 1) != CFW_OK ||
            cfw_dispatch_write_u64(buffer, capacity, &offset,
                                   declared_size) != CFW_OK ||
            cfw_dispatch_write(buffer, capacity, &offset, crlf,
                               sizeof(crlf) - 1) != CFW_OK) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
    }

    /* Terminating empty line. */
    if (cfw_dispatch_write(buffer, capacity, &offset, crlf,
                           sizeof(crlf) - 1) != CFW_OK) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    *written = offset;
    return CFW_OK;
}
