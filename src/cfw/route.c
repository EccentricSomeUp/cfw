/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/route.h>

#include <string.h>

/* ------------------------------------------------------------------ */
/* Route storage                                                       */
/* ------------------------------------------------------------------ */

typedef struct cfw_route_segment {
    cfw_strview_t text; /* literal bytes, or parameter name */
    int is_param;
} cfw_route_segment_t;

struct cfw_route {
    cfw_http_method_t method;
    void *user;
    cfw_route_segment_t *segments;
    size_t segment_count;
    size_t param_count;
    cfw_route_t *next;
};

cfw_status_t cfw_router_init(cfw_router_t *router, cfw_arena_t *arena)
{
    if (router == NULL || arena == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    router->arena = arena;
    router->head = NULL;
    router->count = 0;
    return CFW_OK;
}

size_t cfw_router_count(const cfw_router_t *router)
{
    return router == NULL ? 0 : router->count;
}

/* ------------------------------------------------------------------ */
/* Pattern validation                                                  */
/* ------------------------------------------------------------------ */

static int cfw_route_is_param_name_char(unsigned char c, int first)
{
    if (c >= 'A' && c <= 'Z') {
        return 1;
    }
    if (c >= 'a' && c <= 'z') {
        return 1;
    }
    if (c == '_') {
        return 1;
    }
    if (!first && c >= '0' && c <= '9') {
        return 1;
    }
    return 0;
}

static int cfw_route_literal_char_ok(unsigned char c)
{
    /* Visible ASCII except '?' and '#', which cannot appear in a path. */
    return c >= 0x21 && c <= 0x7E && c != '?' && c != '#';
}

/* Validates one pattern segment [data,size). Returns 1 literal, 2 param,
 * 0 invalid. For params, *name receives the name view. */
static int cfw_route_validate_segment(const char *data, size_t size,
                                      cfw_strview_t *name)
{
    size_t i;
    if (size == 0) {
        return 0;
    }
    if (data[0] == ':') {
        if (size == 1) {
            return 0;
        }
        for (i = 1; i < size; i++) {
            if (!cfw_route_is_param_name_char((unsigned char)data[i],
                                              i == 1)) {
                return 0;
            }
        }
        name->data = data + 1;
        name->size = size - 1;
        return 2;
    }
    for (i = 0; i < size; i++) {
        if (!cfw_route_literal_char_ok((unsigned char)data[i])) {
            return 0;
        }
    }
    return 1;
}

/* Pass over the pattern body (after the leading '/') validating segments
 * and counting them. Returns segment count or (size_t)-1 on error. */
static size_t cfw_route_scan_pattern(const cfw_strview_t *pattern,
                                     size_t *param_count)
{
    size_t pos = 1; /* skip leading '/' */
    size_t count = 0;
    size_t params = 0;

    *param_count = 0;
    if (pattern->size == 1) {
        return 0; /* root pattern "/" */
    }
    for (;;) {
        size_t end = pos;
        cfw_strview_t name;
        int kind;
        while (end < pattern->size && pattern->data[end] != '/') {
            end++;
        }
        kind = cfw_route_validate_segment(pattern->data + pos, end - pos,
                                          &name);
        if (kind == 0) {
            return (size_t)-1;
        }
        if (kind == 2) {
            params++;
        }
        count++;
        if (end == pattern->size) {
            break;
        }
        pos = end + 1;
        if (pos == pattern->size) {
            /* Trailing '/' produces an empty segment: invalid. */
            return (size_t)-1;
        }
    }
    *param_count = params;
    return count;
}

cfw_status_t cfw_router_add(cfw_router_t *router, cfw_http_method_t method,
                            const cfw_strview_t *pattern, void *user,
                            const cfw_route_t **out_route)
{
    cfw_route_t *route;
    cfw_route_t *tail;
    char *pattern_copy;
    cfw_route_segment_t *segments;
    size_t param_count = 0;
    size_t segment_count;
    size_t pos;
    size_t si;
    size_t i;
    size_t j;
    cfw_status_t status;

    if (router == NULL || router->arena == NULL || pattern == NULL ||
        pattern->size == 0 || pattern->data == NULL ||
        pattern->data[0] != '/') {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (out_route != NULL) {
        *out_route = NULL;
    }

    segment_count = cfw_route_scan_pattern(pattern, &param_count);
    if (segment_count == (size_t)-1) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    /* Reject duplicate parameter names within one pattern: extraction by
     * name would be ambiguous. */
    {
        size_t pos_i = 1;
        for (i = 0; i < segment_count; i++) {
            size_t end_i = pos_i;
            while (end_i < pattern->size &&
                   pattern->data[end_i] != '/') {
                end_i++;
            }
            if (pattern->data[pos_i] == ':') {
                cfw_strview_t name_i;
                name_i.data = pattern->data + pos_i + 1;
                name_i.size = end_i - pos_i - 1;
                size_t pos_j = 1;
                for (j = 0; j < segment_count; j++) {
                    size_t end_j = pos_j;
                    while (end_j < pattern->size &&
                           pattern->data[end_j] != '/') {
                        end_j++;
                    }
                    if (j > i && pattern->data[pos_j] == ':' &&
                        end_j - pos_j - 1 == name_i.size &&
                        memcmp(pattern->data + pos_j + 1, name_i.data,
                               name_i.size) == 0) {
                        return CFW_ERROR_INVALID_ARGUMENT;
                    }
                    if (end_j == pattern->size) {
                        break;
                    }
                    pos_j = end_j + 1;
                }
            }
            if (end_i == pattern->size) {
                break;
            }
            pos_i = end_i + 1;
        }
    }

    status = cfw_arena_alloc(router->arena, pattern->size,
                             (void **)&pattern_copy);
    if (status != CFW_OK) {
        return status;
    }
    memcpy(pattern_copy, pattern->data, pattern->size);

    status = cfw_arena_alloc(router->arena,
                             segment_count * sizeof(cfw_route_segment_t),
                             (void **)&segments);
    if (status != CFW_OK) {
        return status;
    }
    status = cfw_arena_alloc(router->arena, sizeof(cfw_route_t),
                             (void **)&route);
    if (status != CFW_OK) {
        return status;
    }

    /* Fill segments; all strviews reference pattern_copy. */
    pos = 1;
    for (si = 0; si < segment_count; si++) {
        size_t end = pos;
        while (end < pattern->size && pattern->data[end] != '/') {
            end++;
        }
        if (pattern_copy[pos] == ':') {
            segments[si].is_param = 1;
            segments[si].text.data = pattern_copy + pos + 1;
            segments[si].text.size = end - pos - 1;
        } else {
            segments[si].is_param = 0;
            segments[si].text.data = pattern_copy + pos;
            segments[si].text.size = end - pos;
        }
        pos = end + 1;
    }

    route->method = method;
    route->user = user;
    route->segments = segments;
    route->segment_count = segment_count;
    route->param_count = param_count;
    route->next = NULL;

    /* Append to preserve registration-order priority. */
    if (router->head == NULL) {
        router->head = route;
    } else {
        for (tail = router->head; tail->next != NULL; tail = tail->next) {
        }
        tail->next = route;
    }
    router->count++;

    if (out_route != NULL) {
        *out_route = route;
    }
    return CFW_OK;
}

/* ------------------------------------------------------------------ */
/* Matching                                                            */
/* ------------------------------------------------------------------ */

cfw_status_t cfw_router_match(const cfw_router_t *router,
                              cfw_http_method_t method,
                              const cfw_strview_t *path,
                              cfw_route_param_t *params,
                              size_t param_capacity,
                              cfw_route_match_t *match)
{
    const cfw_route_t *route;

    if (router == NULL || match == NULL || path == NULL ||
        (path->size != 0 && path->data == NULL) ||
        (param_capacity != 0 && params == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    match->route = NULL;
    match->user = NULL;
    match->param_count = 0;
    match->params_written = 0;

    if (path->size == 0 || path->data[0] != '/') {
        return CFW_OK; /* no match */
    }

    for (route = router->head; route != NULL; route = route->next) {
        size_t rpos = 1;
        size_t si;
        size_t written = 0;
        int ok = 1;

        if (route->method != CFW_HTTP_METHOD_UNKNOWN &&
            method != CFW_HTTP_METHOD_UNKNOWN &&
            route->method != method) {
            continue;
        }

        for (si = 0; si < route->segment_count; si++) {
            const cfw_route_segment_t *seg = &route->segments[si];
            size_t e;
            cfw_strview_t value;
            if (rpos > path->size) {
                ok = 0;
                break;
            }
            e = rpos;
            while (e < path->size && path->data[e] != '/') {
                e++;
            }
            value.data = path->data + rpos;
            value.size = e - rpos;

            if (seg->is_param) {
                if (value.size == 0) {
                    ok = 0;
                    break;
                }
                if (written < param_capacity) {
                    params[written].name = seg->text;
                    params[written].value = value;
                }
                written++;
            } else {
                if (value.size != seg->text.size ||
                    memcmp(value.data, seg->text.data, value.size) != 0) {
                    ok = 0;
                    break;
                }
            }
            rpos = e + 1;
        }
        if (!ok) {
            continue;
        }

        /* No leftover segments allowed: rpos must have moved past the
         * path end, except the root path "/" which has none. */
        if (rpos <= path->size &&
            !(rpos == path->size && path->size == 1)) {
            continue;
        }

        match->route = route;
        match->user = route->user;
        match->param_count = route->param_count;
        match->params_written =
            route->param_count < param_capacity ? route->param_count
                                                : param_capacity;
        return CFW_OK;
    }

    return CFW_OK;
}

cfw_status_t cfw_route_match_param(const cfw_route_param_t *params,
                                   size_t params_written,
                                   const cfw_strview_t *name,
                                   cfw_strview_t *value)
{
    size_t i;
    if (params == NULL || name == NULL || value == NULL ||
        name->size == 0 || name->data == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < params_written; i++) {
        if (params[i].name.size == name->size &&
            memcmp(params[i].name.data, name->data, name->size) == 0) {
            *value = params[i].value;
            return CFW_OK;
        }
    }
    return CFW_ERROR_INVALID_ARGUMENT;
}
