/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * TLS server example: serves HTTPS via cfw_server_options_t.tls_ctx
 * (ADR 0058). Requires a CFW_ENABLE_TLS build; prints a notice and
 * exits otherwise so the example still compiles in the default
 * dependency-free build.
 *
 * Usage: cfw_example_tls_server [cert.pem] [key.pem]
 * Defaults: server.crt server.key in the working directory.
 * Test with: curl -k https://127.0.0.1:8443/hello
 */

#include <cfw/cfw.h>

#include <stdio.h>
#include <string.h>

#if CFW_HAVE_TLS
static cfw_status_t hello(cfw_request_t *req, cfw_response_t *res,
                          void *user)
{
    static const char body[] = "Hello over TLS!";
    (void)req;
    (void)user;
    res->status = CFW_HTTP_STATUS_OK;
    res->body.data = (const uint8_t *)body;
    res->body.size = sizeof(body) - 1;
    return CFW_OK;
}
#endif

int main(int argc, char **argv)
{
#if CFW_HAVE_TLS
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t dopt;
    cfw_tls_ctx_options_t topt;
    cfw_tls_ctx_t *tls_ctx = NULL;
    cfw_server_options_t sopt;
    cfw_server_t server;
    cfw_strview_t pattern;
    const char *cert = argc > 1 ? argv[1] : "server.crt";
    const char *key = argc > 2 ? argv[2] : "server.key";

    memset(&topt, 0, sizeof(topt));
    topt.role = CFW_TLS_SERVER;
    topt.cert_file = cert;
    topt.key_file = key;
    /* topt.ca_file + topt.verify_peer would enable mutual TLS. */
    if (cfw_tls_ctx_create(&topt, &tls_ctx) != CFW_OK) {
        fprintf(stderr, "cfw_example_tls_server: failed to load %s/%s\n",
                cert, key);
        return 1;
    }

    arena = cfw_arena_create(1u << 20);
    if (arena == NULL) {
        cfw_tls_ctx_destroy(tls_ctx);
        return 1;
    }
    if (cfw_router_init(&router, arena) != CFW_OK) {
        goto fail;
    }

    memset(&dopt, 0, sizeof(dopt));
    dopt.router = &router;
    dopt.handler = hello;
    dopt.max_middleware = 4;
    if (cfw_dispatch_init(&dispatch, arena, &dopt) != CFW_OK) {
        goto fail;
    }

    if (cfw_strview_from_cstr(&pattern, "/hello") != CFW_OK ||
        cfw_dispatch_add_route(&dispatch, CFW_HTTP_METHOD_GET, &pattern,
                               hello, NULL) != CFW_OK) {
        goto fail;
    }

    memset(&sopt, 0, sizeof(sopt));
    sopt.host = "127.0.0.1";
    sopt.port = 8443;
    sopt.dispatch = &dispatch;
    sopt.http.max_head_size = 8192;
    sopt.http.max_pending_size = 65536;
    sopt.max_out_size = 8192;
    sopt.max_connections = 128;
    sopt.max_headers = 32;
    sopt.max_params = 16;
    sopt.tls_ctx = tls_ctx;
    if (cfw_server_init(&server, &sopt) != CFW_OK) {
        goto fail;
    }

    fprintf(stderr,
            "cfw TLS server listening on https://%s:%u/hello\n",
            sopt.host, (unsigned)cfw_server_port(&server));

    cfw_server_run(&server); /* blocks; call cfw_server_stop() elsewhere */

    cfw_server_free(&server);
    cfw_tls_ctx_destroy(tls_ctx);
    cfw_arena_destroy(arena);
    return 0;

fail:
    cfw_tls_ctx_destroy(tls_ctx);
    cfw_arena_destroy(arena);
    return 1;
#else
    (void)argc;
    (void)argv;
    fprintf(stderr,
            "cfw_example_tls_server: built without TLS support; "
            "reconfigure with -DCFW_ENABLE_TLS=ON\n");
    return 1;
#endif
}
