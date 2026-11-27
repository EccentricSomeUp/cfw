/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Static file serving example: mounts cfw_static_serve (ADR 0036)
 * as dispatch middleware so GET/HEAD requests under /static/ stream
 * files from ./www — with Last-Modified, ETag, Accept-Ranges, and
 * conditional-request (If-None-Match etc.) plus single-range Range
 * support (ADRs 0039/0040). Everything else falls through to the
 * router.
 *
 * Usage: cfw_example_static_files [docroot]
 * Default docroot: ./www — create it and drop files in, e.g.
 *   mkdir -p www && echo '<h1>hi</h1>' > www/index.html
 * Then: curl http://127.0.0.1:8080/static/index.html
 *       curl -H 'Range: bytes=0-4' http://127.0.0.1:8080/static/index.html
 */

#include <cfw/cfw.h>

#include <stdio.h>
#include <string.h>

static cfw_status_t hello(cfw_request_t *req, cfw_response_t *res,
                          void *user)
{
    static const char body[] = "dynamic route still works";
    (void)req;
    (void)user;
    res->status = CFW_HTTP_STATUS_OK;
    res->body.data = (const uint8_t *)body;
    res->body.size = sizeof(body) - 1;
    return CFW_OK;
}

int main(int argc, char **argv)
{
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t dopt;
    cfw_static_mount_t mount;
    cfw_static_options_t sopt;
    cfw_server_options_t servopt;
    cfw_server_t server;
    cfw_strview_t pattern;
    const char *root = argc > 1 ? argv[1] : "./www";
    int ok = 0;

    arena = cfw_arena_create(1u << 20);
    if (arena == NULL) {
        return 1;
    }
    if (cfw_router_init(&router, arena) != CFW_OK) {
        goto fail;
    }

    memset(&dopt, 0, sizeof(dopt));
    dopt.router = &router;
    dopt.handler = hello;
    /* One slot for the static middleware plus headroom for future
     * middleware entries. */
    dopt.max_middleware = 4;
    if (cfw_dispatch_init(&dispatch, arena, &dopt) != CFW_OK) {
        goto fail;
    }

    memset(&sopt, 0, sizeof(sopt));
    sopt.root = root;
    sopt.url_prefix = "/static";
    sopt.max_concurrent = 8;
    sopt.buffer_size = 16384;
    sopt.arena = arena;
    if (cfw_static_mount_init(&mount, &sopt) != CFW_OK) {
        goto fail;
    }
    if (cfw_dispatch_use(&dispatch, cfw_static_serve, &mount) != CFW_OK) {
        goto fail;
    }

    if (cfw_strview_from_cstr(&pattern, "/hello") != CFW_OK ||
        cfw_dispatch_add_route(&dispatch, CFW_HTTP_METHOD_GET, &pattern,
                               hello, NULL) != CFW_OK) {
        goto fail;
    }

    memset(&servopt, 0, sizeof(servopt));
    servopt.host = "127.0.0.1";
    servopt.port = 8080;
    servopt.dispatch = &dispatch;
    servopt.http.max_head_size = 8192;
    servopt.http.max_pending_size = 65536;
    servopt.max_out_size = 8192;
    servopt.max_connections = 128;
    servopt.max_headers = 32;
    servopt.max_params = 16;
    if (cfw_server_init(&server, &servopt) != CFW_OK) {
        goto fail;
    }

    fprintf(stderr,
            "cfw static file server listening on http://%s:%u\n"
            "  files:  http://%s:%u/static/<path>  (docroot: %s)\n"
            "  route:  http://%s:%u/hello\n",
            servopt.host, (unsigned)cfw_server_port(&server),
            servopt.host, (unsigned)cfw_server_port(&server), root,
            servopt.host, (unsigned)cfw_server_port(&server));

    cfw_server_run(&server); /* blocks; call cfw_server_stop() elsewhere */

    ok = 1;

fail:
    cfw_arena_destroy(arena);
    return ok ? 0 : 1;
}
