/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Access-log example: formats cfw_request_record_t (ADR 0055) into the
 * de-facto "combined" log format on stderr:
 *
 *   127.0.0.1:51234 - - [26/Nov/2026:12:00:00 +0000] "GET /hello HTTP/1.1"
 *     200 1234 5678
 *
 * The date bracket uses the request's completion wall time via
 * cfw_time_wall_now + cfw_http_date_format is NOT used here — HTTP-date
 * is second-granularity without timezone; instead we print the raw
 * unix seconds to keep the example dependency-free. Peer address,
 * method, target, status, request/response bytes, and duration come
 * straight from the record. Records are borrowed views: everything is
 * copied into the line before the callback returns.
 *
 * Usage: access_log [port]
 */

#include <cfw/cfw.h>

#include <stdio.h>
#include <stdlib.h>
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

static const char *method_name(cfw_http_method_t method)
{
    return cfw_http_method_name(method);
}

static void access_log_done(const cfw_request_record_t *record,
                            void *user)
{
    int64_t duration_us = 0;
    int64_t duration_ns = 0;
    const char *peer = "-";
    char peer_buf[80];
    (void)user;

    if (cfw_duration_to_nanos(&record->duration, &duration_ns) != CFW_OK) {
        duration_ns = 0;
    }
    duration_us = duration_ns / 1000;

    if (record->remote_addr != NULL && record->remote_addr->size != 0 &&
        record->remote_addr->size < sizeof(peer_buf)) {
        memcpy(peer_buf, record->remote_addr->data,
               record->remote_addr->size);
        peer_buf[record->remote_addr->size] = '\0';
        peer = peer_buf;
    }

    /* status 0 means the connection ended before any response was
     * produced (abort/timeout); it is logged verbatim. */
    {
        const char *mn = method_name(record->method);
        fprintf(stderr,
                "%s - - \"%s %.*s HTTP/%u.%u\" %d %llu %llu %lldus%s\n",
                peer, mn != NULL ? mn : "?",
                (int)record->target.size,
                (const char *)record->target.data,
                (unsigned)record->version.major,
                (unsigned)record->version.minor,
                record->status,
                (unsigned long long)record->request_bytes,
                (unsigned long long)record->response_bytes,
                (long long)duration_us,
                record->upgraded ? " UPGRADED" : "");
    }
}

int main(int argc, char **argv)
{
    uint16_t port = argc > 1 ? (uint16_t)atoi(argv[1]) : 8080;
    cfw_arena_t *arena = cfw_arena_create(1u << 20);
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t dopt;
    cfw_server_options_t sopt;
    cfw_strview_t pattern;
    cfw_server_t server;

    if (arena == NULL || cfw_router_init(&router, arena) != CFW_OK) {
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
    sopt.port = port;
    sopt.dispatch = &dispatch;
    sopt.http.max_head_size = 8192;
    sopt.http.max_body_size = 65536;
    sopt.http.max_pending_size = 65536;
    sopt.max_out_size = 8192;
    sopt.max_connections = 128;
    sopt.max_headers = 32;
    sopt.max_params = 16;
    sopt.request_done = access_log_done;
    if (cfw_server_init(&server, &sopt) != CFW_OK) {
        cfw_arena_destroy(arena);
        return 1;
    }

    fprintf(stderr, "cfw access-log server on http://%s:%u/hello\n",
            sopt.host, (unsigned)cfw_server_port(&server));
    fflush(stderr);

    cfw_server_run(&server);
    cfw_server_free(&server);
    cfw_arena_destroy(arena);
    return 0;
}
