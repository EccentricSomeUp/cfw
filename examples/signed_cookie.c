/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Signed-cookie example: issues an HMAC-signed session cookie on the
 * first visit (cfw_signed_encode + cfw_cookie_serialize) and reads it
 * back on subsequent requests (cfw_cookie_parse + cfw_signed_decode).
 *
 * The signing key below is a placeholder for demonstration only —
 * real deployments must load a secret from the environment or a
 * secrets manager and keep it stable across restarts (rotating it
 * invalidates every outstanding token).
 */

#include <cfw/cfw.h>

#include <stdio.h>
#include <string.h>

static const char cfw_example_signing_key[] =
    "cfw-example-signing-key-REPLACE-ME";

/* First Cookie header value, or NULL. Mirrors the private helper in
 * ws.c; there is no public header-lookup API yet. */
static const cfw_strview_t *cookie_header(const cfw_request_t *req)
{
    static const char name[] = "cookie";
    cfw_strview_t wanted;
    size_t i;
    int eq = 0;

    cfw_strview_from_cstr(&wanted, name);
    for (i = 0; i < req->header_count; i++) {
        if (cfw_strview_equal_nocase(&req->headers[i].name, &wanted,
                                     &eq) == CFW_OK &&
            eq) {
            return &req->headers[i].value;
        }
    }
    return NULL;
}

static cfw_status_t index_handler(cfw_request_t *req, cfw_response_t *res,
                                  void *user)
{
    static char body[512];
    static char set_cookie[256];
    static const char payload[] = "demo-user-001";
    const cfw_strview_t *cookie_hdr;
    cfw_cookie_pair_t pairs[8];
    size_t pair_count = 0;
    size_t i;
    int found = 0;
    (void)user;

    res->status = CFW_HTTP_STATUS_OK;
    res->header_count = 0;

    cookie_hdr = cookie_header(req);
    if (cookie_hdr != NULL &&
        cfw_cookie_parse(cookie_hdr, pairs, 8, &pair_count, NULL) == CFW_OK) {
        for (i = 0; i < pair_count; i++) {
            cfw_strview_t name;
            int eq = 0;
            cfw_strview_from_cstr(&name, "session");
            if (cfw_strview_equal(&pairs[i].name, &name, &eq) == CFW_OK &&
                eq) {
                unsigned char session[128];
                size_t session_len = 0;
                cfw_strview_t token = pairs[i].value;
                if (cfw_signed_decode(cfw_example_signing_key,
                                      sizeof(cfw_example_signing_key) - 1,
                                      &token, session, sizeof(session),
                                      &session_len) == CFW_OK) {
                    session[session_len] = '\0';
                    snprintf(body, sizeof(body),
                             "session accepted: %s", (char *)session);
                    res->body.data = (const uint8_t *)body;
                    res->body.size = strlen(body);
                    return CFW_OK;
                }
                found = 1; /* cookie present but invalid */
                break;
            }
        }
    }

    /* No valid session: mint a signed cookie and Set-Cookie it. */
    {
        char token[256];
        size_t token_len = 0;
        cfw_cookie_t cookie;
        size_t set_len = 0;
        static cfw_http_header_t headers[1];

        memset(&cookie, 0, sizeof(cookie));
        if (cfw_signed_encode(cfw_example_signing_key,
                              sizeof(cfw_example_signing_key) - 1, payload,
                              sizeof(payload) - 1, token, sizeof(token),
                              &token_len) != CFW_OK) {
            res->status = CFW_HTTP_STATUS_INTERNAL_SERVER_ERROR;
            return CFW_OK;
        }
        cfw_strview_init(&cookie.name, "session", 7);
        cfw_strview_init(&cookie.value, token, token_len);
        cfw_strview_init(&cookie.path, "/", 1);
        cookie.max_age_seconds = 3600;
        cookie.httponly = 1;
        cookie.samesite = CFW_COOKIE_SAMESITE_STRICT;
        if (cfw_cookie_serialize(&cookie, set_cookie, sizeof(set_cookie),
                                 &set_len) != CFW_OK) {
            res->status = CFW_HTTP_STATUS_INTERNAL_SERVER_ERROR;
            return CFW_OK;
        }
        cfw_strview_init(&headers[0].name, "Set-Cookie",
                         sizeof("Set-Cookie") - 1);
        cfw_strview_init(&headers[0].value, set_cookie, set_len);
        res->headers = headers;
        res->header_count = 1;

        if (found) {
            snprintf(body, sizeof(body),
                     "invalid session cookie; reissued");
        } else {
            snprintf(body, sizeof(body),
                     "no session cookie; issued signed cookie");
        }
    }
    res->body.data = (const uint8_t *)body;
    res->body.size = strlen(body);
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
    if (cfw_router_init(&router, arena) != CFW_OK ||
        cfw_strview_from_cstr(&pattern, "/") != CFW_OK) {
        cfw_arena_destroy(arena);
        return 1;
    }
    memset(&dopt, 0, sizeof(dopt));
    dopt.router = &router;
    dopt.handler = index_handler;
    dopt.max_middleware = 4;
    if (cfw_dispatch_init(&dispatch, arena, &dopt) != CFW_OK ||
        cfw_dispatch_add_route(&dispatch, CFW_HTTP_METHOD_GET, &pattern,
                               index_handler, NULL) != CFW_OK) {
        cfw_arena_destroy(arena);
        return 1;
    }
    memset(&sopt, 0, sizeof(sopt));
    sopt.host = "127.0.0.1";
    sopt.port = 8081;
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

    fprintf(stderr,
            "cfw signed-cookie example on http://%s:%u/ "
            "(curl -c jar -b jar to see the round trip)\n",
            sopt.host, (unsigned)cfw_server_port(&server));

    cfw_server_run(&server);

    cfw_server_free(&server);
    cfw_arena_destroy(arena);
    return 0;
}
