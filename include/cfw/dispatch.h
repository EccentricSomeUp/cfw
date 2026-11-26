/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_DISPATCH_H
#define CFW_DISPATCH_H

#include <cfw/arena.h>
#include <cfw/http.h>
#include <cfw/http1.h>
#include <cfw/route.h>
#include <cfw/span.h>
#include <cfw/status.h>
#include <cfw/strview.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Request dispatch: ties the router (ADR 0026) to an ordered middleware
 * chain, a terminal handler, error mapping, and response serialization
 * (ADR 0027).
 *
 * The dispatch object is stateless configuration — safe to share across
 * connections. All per-request state lives in caller-provided scratch
 * (header/param arrays) and the request/response structs, valid only
 * for the duration of cfw_dispatch_request.
 *
 * Middleware ordering: registered first = outermost. It sees the request
 * first and the response last. Cancellation: return CFW_OK without
 * calling next() to short-circuit; return any error status to abort into
 * the error handler. next() must be called at most once per middleware.
 */

typedef struct cfw_request cfw_request_t;
typedef struct cfw_response cfw_response_t;
typedef struct cfw_dispatch cfw_dispatch_t;
typedef struct cfw_dispatch_frame cfw_dispatch_frame_t;

typedef cfw_status_t (*cfw_dispatch_next_t)(cfw_dispatch_frame_t *frame);

typedef cfw_status_t (*cfw_middleware_fn)(cfw_request_t *req,
                                          cfw_response_t *res,
                                          cfw_dispatch_next_t next,
                                          cfw_dispatch_frame_t *frame,
                                          void *user);

typedef cfw_status_t (*cfw_handler_fn)(cfw_request_t *req,
                                       cfw_response_t *res, void *user);

typedef struct cfw_dispatch_middleware {
    cfw_middleware_fn fn;
    void *user;
} cfw_dispatch_middleware_t;

/* Per-route handler binding installed by cfw_dispatch_add_route. When a
 * matched route carries a binding (match.ex), it overrides the dispatch
 * default handler. WARNING: dispatch interprets ANY non-NULL match.ex
 * as this type — do not register routes with cfw_router_add_ex using a
 * different ex layout on a router served by cfw_dispatch. */
typedef struct cfw_route_binding {
    cfw_handler_fn handler;
    void *user;
} cfw_route_binding_t;

/* Fills `res` for a failed dispatch. `status_hint` is the framework's
 * suggested status (400/404/500); implementations may override it. */
typedef void (*cfw_dispatch_error_fn)(cfw_request_t *req,
                                      cfw_response_t *res,
                                      cfw_http_status_t status_hint,
                                      void *user);

typedef struct cfw_request {
    /* Parsed request line fields (views into transport storage). */
    cfw_http_method_t method;
    cfw_strview_t target;
    cfw_http_version_t version;
    /* Path component of the target for routing ("" when absent). */
    cfw_strview_t path;
    /* Parsed header fields; entries beyond `header_capacity` are
     * counted in `headers_dropped` and not stored. */
    const cfw_http_header_t *headers;
    size_t header_count;
    size_t headers_dropped;
    /* Complete decoded body (may be empty). */
    cfw_span_t body;
    /* Route match result; match.route == NULL means no route matched
     * (dispatch then invoked the error handler with 404). */
    cfw_route_match_t match;
    /* Extracted route parameters (views into the request target);
     * look up by name with cfw_route_match_param. */
    const cfw_route_param_t *params;
    size_t params_written;
    /* Application data registered with the matched route. */
    void *route_user;
} cfw_request_t;

typedef struct cfw_response {
    cfw_http_status_t status;
    /* Caller-provided header array; middleware/handlers append up to
     * header_capacity via cfw_response_header_add. */
    cfw_http_header_t *headers;
    size_t header_count;
    size_t header_capacity;
    /* Single body span queued by the caller after head serialization.
     * Suppressed automatically for HEAD/1xx/204/304 responses. */
    cfw_span_t body;
    /* Application data for the terminal handler/error handler. */
    void *user;
} cfw_response_t;

typedef struct cfw_dispatch_options {
    cfw_router_t *router;
    /* Terminal handler invoked after the last middleware. */
    cfw_handler_fn handler;
    void *handler_user;
    /* Optional error mapper; NULL selects the default (status hint,
     * empty body). */
    cfw_dispatch_error_fn error_handler;
    void *error_user;
    /* Maximum middleware entries; must be > 0. The chain storage is
     * allocated once from `arena` at init. */
    size_t max_middleware;
} cfw_dispatch_options_t;

struct cfw_dispatch {
    cfw_router_t *router;
    cfw_dispatch_middleware_t *middleware;
    size_t middleware_count;
    size_t middleware_capacity;
    cfw_handler_fn handler;
    void *handler_user;
    cfw_dispatch_error_fn error_handler;
    void *error_user;
    cfw_arena_t *arena;
};

/* Initializes dispatch. `arena` (a cfw_arena_t*) must outlive the
 * dispatch; middleware registrations are stored there. Returns
 * CFW_ERROR_INVALID_ARGUMENT when router/handler are NULL. */
CFW_API cfw_status_t cfw_dispatch_init(cfw_dispatch_t *dispatch,
                                       cfw_arena_t *arena,
                                       const cfw_dispatch_options_t *options);

/* Appends a middleware to the chain; entries run in registration order
 * (first registered = outermost). Fails with CFW_ERROR_INVALID_ARGUMENT
 * when the chain is full (max_middleware) or fn is NULL. */
CFW_API cfw_status_t cfw_dispatch_use(cfw_dispatch_t *dispatch,
                                      cfw_middleware_fn fn, void *user);

/* Convenience registration binding a per-route handler. Equivalent to
 * cfw_router_add_ex with the binding as the route's extension pointer;
 * `user` is stored both as route user data and inside the binding for
 * the handler. The binding is allocated from the dispatch arena. */
CFW_API cfw_status_t cfw_dispatch_add_route(cfw_dispatch_t *dispatch,
                                            cfw_http_method_t method,
                                            const cfw_strview_t *pattern,
                                            cfw_handler_fn handler,
                                            void *user);

/* Runs one request through routing, middleware, and the handler.
 *
 * Inputs:
 * - head: parsed request head (from cfw_http1_event_t.head or
 *   cfw_http_request_head_parse); its strviews must outlive the call.
 * - body: complete decoded body span (empty when none).
 * - headers/param scratch: caller arrays; headers beyond capacity are
 *   dropped and counted. params beyond capacity truncate extraction
 *   (match.param_count still reports the total).
 * - res: output response. The caller must set res->headers and
 *   res->header_capacity to its scratch array before the call; the
 *   function resets status (200), header_count, body, and user.
 *
 * Returns CFW_OK when a response was produced (including error-mapped
 * ones); CFW_ERROR_INVALID_ARGUMENT on invalid arguments. Routing,
 * framing, and handler failures are reported through the error handler,
 * not the return status. */
CFW_API cfw_status_t cfw_dispatch_request(
    const cfw_dispatch_t *dispatch, const cfw_http_request_head_t *head,
    const cfw_span_t *body, cfw_http_header_t *headers,
    size_t header_capacity, cfw_route_param_t *params,
    size_t param_capacity, cfw_response_t *res);

/* Appends a header to a response; fails with CFW_ERROR_NOMEM when
 * header_capacity is reached. */
CFW_API cfw_status_t cfw_response_header_add(cfw_response_t *res,
                                             const cfw_strview_t *name,
                                             const cfw_strview_t *value);

/* Serializes the response head (status line, headers, terminating CRLF)
 * into `buffer`. Automatically appends Content-Length when the body is
 * non-empty, no Content-Length/Transfer-Encoding header exists, and the
 * request method/status permit a body. Returns CFW_OK with *written set,
 * or CFW_ERROR_INVALID_ARGUMENT when the buffer is too small or a header
 * is malformed. The body span is NOT included — queue it separately. */
CFW_API cfw_status_t cfw_dispatch_response_head_format(
    const cfw_request_t *req, const cfw_response_t *res, char *buffer,
    size_t capacity, size_t *written);

/* Looks up a request header by name (ASCII case-insensitive). Returns
 * CFW_OK and sets *value, or CFW_ERROR_INVALID_ARGUMENT. */
CFW_API cfw_status_t cfw_request_header(const cfw_request_t *req,
                                        const cfw_strview_t *name,
                                        cfw_strview_t *value);

#ifdef __cplusplus
}
#endif

#endif /* CFW_DISPATCH_H */
