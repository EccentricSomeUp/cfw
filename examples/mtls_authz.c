/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Mutual-TLS authorization example (ADR 0063): requires a client
 * certificate at the TLS layer and then authorizes the request by
 * comparing the SHA-256 fingerprint of the peer certificate's DER
 * encoding against an allowlist entry — the pattern req->tls_conn
 * exists for.
 *
 * Requires a CFW_ENABLE_TLS build; prints a notice and exits
 * otherwise so the example still compiles in the default
 * dependency-free build.
 *
 * Usage: cfw_example_mtls_authz <server.crt> <server.key> <ca.crt>
 *                                 <allowed-sha256-hex> [port]
 *
 * The allowed fingerprint is the hex SHA-256 of the client cert's
 * DER form, e.g.:
 *   openssl x509 -in client.crt -outform der | sha256sum
 * Test with:
 *   curl --cert client.crt --key client.key --cacert ca.crt \
 *        https://127.0.0.1:8443/secret
 */

#include <cfw/cfw.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CFW_HAVE_TLS

/* Allowed client-cert SHA-256, decoded from hex at startup. */
static unsigned char cfw_example_allowed_fp[32];

static int cfw_example_ct_eq(const unsigned char *a,
                             const unsigned char *b, size_t n)
{
    unsigned char diff = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static cfw_status_t secret(cfw_request_t *req, cfw_response_t *res,
                           void *user)
{
    static const char ok_body[] = "mtls-authz-ok";
    static const char deny_body[] = "mtls-authz-denied";
    unsigned char der[8192];
    size_t der_len = 0;
    unsigned char digest[32];
    int allowed;
    (void)user;

    /* No TLS session (e.g. tls_detect cleartext fallback) or no
     * client certificate -> deny. */
    if (req->tls_conn == NULL ||
        cfw_tls_conn_peer_cert_der(req->tls_conn, der, sizeof(der),
                                   &der_len) != CFW_OK ||
        der_len == 0) {
        res->status = CFW_HTTP_STATUS_FORBIDDEN;
        res->body.data = (const uint8_t *)deny_body;
        res->body.size = sizeof(deny_body) - 1;
        return CFW_OK;
    }

    cfw_sha256(der, der_len, digest);
    allowed = cfw_example_ct_eq(digest, cfw_example_allowed_fp,
                                sizeof(digest));

    res->status = allowed ? CFW_HTTP_STATUS_OK
                          : CFW_HTTP_STATUS_FORBIDDEN;
    res->body.data = (const uint8_t *)(allowed ? ok_body : deny_body);
    res->body.size = allowed ? sizeof(ok_body) - 1
                             : sizeof(deny_body) - 1;
    return CFW_OK;
}

#endif /* CFW_HAVE_TLS */

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
    cfw_strview_t fp_hex;
    uint16_t port = 8443;

    if (argc < 5) {
        fprintf(stderr,
                "usage: %s <server.crt> <server.key> <ca.crt> "
                "<allowed-sha256-hex> [port]\n", argv[0]);
        return 1;
    }
    if (argc > 5) {
        port = (uint16_t)atoi(argv[5]);
    }

    /* Decode the allowed fingerprint; reject anything that is not
     * exactly 64 hex chars. */
    {
        cfw_strview_from_cstr(&fp_hex, argv[4]);
        if (fp_hex.size != 64 ||
            cfw_hex_decode(&fp_hex, cfw_example_allowed_fp,
                           sizeof(cfw_example_allowed_fp),
                           NULL) != CFW_OK) {
            fprintf(stderr, "%s: invalid fingerprint (want 64 hex "
                    "chars)\n", argv[0]);
            return 1;
        }
    }

    memset(&topt, 0, sizeof(topt));
    topt.role = CFW_TLS_SERVER;
    topt.cert_file = argv[1];
    topt.key_file = argv[2];
    topt.ca_file = argv[3];
    topt.verify_peer = 1; /* mandatory client certificate */
    if (cfw_tls_ctx_create(&topt, &tls_ctx) != CFW_OK) {
        fprintf(stderr, "%s: failed to load TLS credentials\n", argv[0]);
        return 1;
    }

    arena = cfw_arena_create(1u << 20);
    if (arena == NULL) {
        goto fail_ctx;
    }
    if (cfw_router_init(&router, arena) != CFW_OK) {
        goto fail_arena;
    }

    memset(&dopt, 0, sizeof(dopt));
    dopt.router = &router;
    dopt.handler = secret;
    dopt.max_middleware = 4;
    if (cfw_dispatch_init(&dispatch, arena, &dopt) != CFW_OK) {
        goto fail_arena;
    }

    if (cfw_strview_from_cstr(&pattern, "/secret") != CFW_OK ||
        cfw_dispatch_add_route(&dispatch, CFW_HTTP_METHOD_GET,
                               &pattern, secret, NULL) != CFW_OK) {
        goto fail_arena;
    }

    memset(&sopt, 0, sizeof(sopt));
    sopt.host = "127.0.0.1";
    sopt.port = port;
    sopt.dispatch = &dispatch;
    sopt.http.max_head_size = 8192;
    sopt.http.max_pending_size = 65536;
    sopt.max_out_size = 8192;
    sopt.max_connections = 128;
    sopt.max_headers = 32;
    sopt.max_params = 16;
    sopt.tls_ctx = tls_ctx;
    if (cfw_server_init(&server, &sopt) != CFW_OK) {
        goto fail_arena;
    }

    fprintf(stderr,
            "%s: mTLS server listening on https://%s:%u/secret\n",
            argv[0], sopt.host, (unsigned)cfw_server_port(&server));

    cfw_server_run(&server); /* blocks; call cfw_server_stop() elsewhere */

    cfw_server_free(&server);
    cfw_tls_ctx_destroy(tls_ctx);
    cfw_arena_destroy(arena);
    return 0;

fail_arena:
    cfw_arena_destroy(arena);
fail_ctx:
    cfw_tls_ctx_destroy(tls_ctx);
    return 1;
#else
    (void)argc;
    (void)argv;
    fprintf(stderr,
            "%s: built without TLS support; reconfigure with "
            "-DCFW_ENABLE_TLS=ON\n", argv[0]);
    return 1;
#endif
}
