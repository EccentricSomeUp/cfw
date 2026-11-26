/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Minimal cfw server: one GET route, default dispatch options, and a
 * blocking cfw_server_run loop. Mirrors the README quickstart; kept
 * compilable by the default build so the documented API cannot drift.
 */

#include <cfw/cfw.h>

#include <stdio.h>
#include <string.h>

static cfw_status_t hello(cfw_request_t *req, cfw_response_t *res,
                          void *user)
{
    static const char body[] = "Hello, cfw!";
    (void)req;
    (void)user;
    res->status = CFW_HTTP_STATUS_OK;
    res->body.data = (const uint8_t *)body;
    res->body.size = sizeof(body) - 1;
    return CFW_OK;
}

int main(void)
{
    cfw_arena_t *arena = cfw_arena_create(1u << 20);
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t dopt = {0};
    cfw_server_options_t sopt = {0};
    cfw_server_t server;
    cfw_strview_t pattern;

    if (arena == NULL) {
        return 1;
    }

    if (cfw_router_init(&router, arena) != CFW_OK) {
        cfw_arena_destroy(arena);
        return 1;
    }

    memset(&dopt, 0, sizeof(dopt));
    dopt.router = &router;
    dopt.handler = hello;
    dopt.max_middleware = 4;
    if (cfw_dispatch_init(&dispatch, arena, &dopt) != CFW_OK) {
        cfw_arena_destroy(arena);
        return 1;
    }

    if (cfw_strview_from_cstr(&pattern, "/hello") != CFW_OK ||
        cfw_dispatch_add_route(&dispatch, CFW_HTTP_METHOD_GET, &pattern,
                               hello, NULL) != CFW_OK) {
        cfw_arena_destroy(arena);
        return 1;
    }

    memset(&sopt, 0, sizeof(sopt));
    sopt.host = "127.0.0.1";
    sopt.port = 8080;
    sopt.dispatch = &dispatch;
    sopt.http.max_head_size = 8192;
    sopt.http.max_pending_size = 65536;
    sopt.max_out_size = 8192;
    sopt.max_connections = 128;
    sopt.max_headers = 32;
    sopt.max_params = 16;
    if (cfw_server_init(&server, &sopt) != CFW_OK) {
        cfw_arena_destroy(arena);
        return 1;
    }

    fprintf(stderr, "cfw minimal server listening on http://%s:%u/hello\n",
            sopt.host, (unsigned)cfw_server_port(&server));

    cfw_server_run(&server); /* blocks; call cfw_server_stop() elsewhere */

    cfw_server_free(&server);
    cfw_arena_destroy(arena);
    return 0;
}
