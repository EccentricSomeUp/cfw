/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_ROUTE_H
#define CFW_ROUTE_H

#include <cfw/arena.h>
#include <cfw/http.h>
#include <cfw/status.h>
#include <cfw/strview.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Route table for HTTP request dispatch (see ADR 0026).
 *
 * Patterns look like "/users/:id/posts/:pid": literal segments match
 * case-sensitively, ":name" segments capture one nonempty path segment.
 * Routes are stored in a caller-provided arena and evaluated in
 * registration order; the first match wins.
 *
 * Path inputs must be the raw path portion of a request target (use
 * cfw_http_target_path to extract it); percent-encoding is preserved.
 */

typedef struct cfw_route_param {
    cfw_strview_t name;
    cfw_strview_t value;
} cfw_route_param_t;

typedef struct cfw_route cfw_route_t;

typedef struct cfw_router {
    cfw_arena_t *arena;
    cfw_route_t *head;
    size_t count;
} cfw_router_t;

typedef struct cfw_route_match {
    /* NULL when no route matched. */
    const cfw_route_t *route;
    /* Application data supplied at registration. */
    void *user;
    /* Number of parameter entries written to the caller's params array. */
    size_t params_written;
    /* Total parameters declared by the matched route; may exceed
     * params_written when the caller's array was too small. */
    size_t param_count;
} cfw_route_match_t;

CFW_API cfw_status_t cfw_router_init(cfw_router_t *router,
                                     cfw_arena_t *arena);

/* Registers a route. `method` of CFW_HTTP_METHOD_UNKNOWN matches any
 * method. The pattern must start with '/', contain no empty segments,
 * and use param names of [A-Za-z_][A-Za-z0-9_]*. Returns the route via
 * `out_route` when non-NULL. */
CFW_API cfw_status_t cfw_router_add(cfw_router_t *router,
                                    cfw_http_method_t method,
                                    const cfw_strview_t *pattern, void *user,
                                    const cfw_route_t **out_route);

/* Finds the first route matching (method, path). Always returns CFW_OK
 * for valid arguments; inspect match->route for the result (NULL = 404).
 * Parameter values are written into `params` (up to `param_capacity`
 * entries) as strviews into `path`; they are retrievable via
 * cfw_route_match_param. Pass params=NULL/capacity=0 to skip extraction
 * — match->param_count still reports the route's total. */
CFW_API cfw_status_t cfw_router_match(const cfw_router_t *router,
                                      cfw_http_method_t method,
                                      const cfw_strview_t *path,
                                      cfw_route_param_t *params,
                                      size_t param_capacity,
                                      cfw_route_match_t *match);

/* Looks up a parameter by name (exact, case-sensitive) in a completed
 * match. `params`/`params_written` describe the array passed to
 * cfw_router_match. Returns CFW_OK and sets *value, or
 * CFW_ERROR_INVALID_ARGUMENT when absent or arguments invalid. */
CFW_API cfw_status_t cfw_route_match_param(
    const cfw_route_param_t *params, size_t params_written,
    const cfw_strview_t *name, cfw_strview_t *value);

CFW_API size_t cfw_router_count(const cfw_router_t *router);

#ifdef __cplusplus
}
#endif

#endif /* CFW_ROUTE_H */
