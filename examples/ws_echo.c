/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Minimal WebSocket echo server (RFC 6455, ADR 0035): GET /ws performs
 * the opening handshake inside the dispatch handler, then the server
 * upgrade hook (ADR 0034) owns the framed data path. Every complete
 * message is echoed back with its original opcode.
 *
 * Cleartext ws:// only — wss:// waits on the deferred TLS decision
 * (ADR 0030). Point a browser or `websocat`/`wscat` at
 * ws://127.0.0.1:8080/ws to try it.
 */

#include <cfw/cfw.h>

#include <stdio.h>
#include <string.h>

/* Echoes each complete message verbatim (text stays text, binary
 * stays binary). The payload span is borrowed — cfw_ws_send copies it
 * into the connection queue before returning. */
static cfw_status_t ws_on_message(cfw_ws_t *ws,
                                  const cfw_ws_message_t *msg, void *user)
{
    (void)user;
    return cfw_ws_send(ws, msg->opcode, msg->payload.data,
                       msg->payload.size);
}

/* Dispatch handler for GET /ws: validates the opening handshake and
 * returns 101 with the accept key. The response head is serialized
 * synchronously after this returns, so a static buffer for the
 * Sec-WebSocket-Accept value is sufficient (handlers never interleave
 * on the single event-loop thread). */
static cfw_status_t ws_handler(cfw_request_t *req, cfw_response_t *res,
                               void *user)
{
    static char accept[CFW_WS_ACCEPT_KEY_CAPACITY];
    cfw_strview_t name;
    cfw_strview_t value;
    (void)user;

    if (cfw_ws_handshake_validate(req) != CFW_OK ||
        cfw_ws_handshake_accept(req, accept, sizeof(accept)) != CFW_OK) {
        res->status = CFW_HTTP_STATUS_BAD_REQUEST;
        return CFW_OK;
    }

    res->status = CFW_HTTP_STATUS_SWITCHING_PROTOCOLS;
    cfw_strview_from_cstr(&name, "Upgrade");
    cfw_strview_from_cstr(&value, "websocket");
    cfw_response_header_add(res, &name, &value);
    cfw_strview_from_cstr(&name, "Connection");
    cfw_strview_from_cstr(&value, "Upgrade");
    cfw_response_header_add(res, &name, &value);
    cfw_strview_from_cstr(&name, "Sec-WebSocket-Accept");
    cfw_strview_init(&value, accept, strlen(accept));
    cfw_response_header_add(res, &name, &value);
    return CFW_OK;
}

/* Server upgrade hook: creates the per-connection cfw_ws_t on READY,
 * forwards every later event to it, and destroys it on the final
 * CFW_UPGRADE_EV_CLOSE so no state leaks when the socket dies. */
static cfw_status_t ws_upgrade(cfw_upgraded_conn_t *conn, uint32_t events,
                               void *user)
{
    cfw_ws_t *ws;
    (void)user;

    if (events == CFW_UPGRADE_EV_CLOSE) {
        ws = cfw_upgraded_user(conn);
        if (ws != NULL) {
            cfw_ws_destroy(ws);
        }
        return CFW_OK;
    }
    if (events == CFW_UPGRADE_EV_READY) {
        cfw_ws_options_t options;
        memset(&options, 0, sizeof(options));
        options.on_message = ws_on_message;
        ws = cfw_ws_create(conn, &options);
        if (ws == NULL) {
            return CFW_ERROR_INTERNAL;
        }
        cfw_upgraded_set_user(conn, ws);
        return CFW_OK;
    }
    ws = cfw_upgraded_user(conn);
    if (ws != NULL) {
        (void)cfw_ws_on_event(ws, events);
    }
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
    dopt.handler = ws_handler;
    dopt.max_middleware = 4;
    if (cfw_dispatch_init(&dispatch, arena, &dopt) != CFW_OK) {
        cfw_arena_destroy(arena);
        return 1;
    }
    if (cfw_strview_from_cstr(&pattern, "/ws") != CFW_OK ||
        cfw_dispatch_add_route(&dispatch, CFW_HTTP_METHOD_GET, &pattern,
                               ws_handler, NULL) != CFW_OK) {
        cfw_arena_destroy(arena);
        return 1;
    }

    memset(&sopt, 0, sizeof(sopt));
    sopt.host = "127.0.0.1";
    sopt.port = 8080;
    sopt.dispatch = &dispatch;
    sopt.http.max_head_size = 8192;
    sopt.http.max_pending_size = 65536;
    sopt.max_out_size = 65536;
    sopt.max_connections = 128;
    sopt.max_headers = 32;
    sopt.max_params = 16;
    sopt.upgrade = ws_upgrade;
    if (cfw_server_init(&server, &sopt) != CFW_OK) {
        cfw_arena_destroy(arena);
        return 1;
    }

    fprintf(stderr,
            "cfw ws echo server listening on ws://%s:%u/ws\n",
            sopt.host, (unsigned)cfw_server_port(&server));

    cfw_server_run(&server); /* blocks; call cfw_server_stop() elsewhere */

    cfw_server_free(&server);
    cfw_arena_destroy(arena);
    return 0;
}
