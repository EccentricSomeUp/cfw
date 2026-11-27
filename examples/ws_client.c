/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Minimal WebSocket client (RFC 6455 client role, ADR 0071):
 * connects to a ws:// (or wss:// with --tls) endpoint, optionally
 * sends one text message, then prints every incoming message until
 * the peer closes.
 *
 * Usage: cfw_example_ws_client <host> <port> <path> [--tls <ca.pem>]
 *                              [message]
 * Example: cfw_example_ws_client 127.0.0.1 9000 /ws "hello"
 * Pair with: cfw_example_ws_echo 9000
 */

#include <cfw/cfw.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cfw_status_t ws_on_message(cfw_ws_client_t *ws,
                                  const cfw_ws_message_t *msg,
                                  void *user)
{
    (void)ws;
    (void)user;
    if (msg->opcode == CFW_WS_OPCODE_TEXT) {
        printf("text: %.*s\n", (int)msg->payload.size,
               (const char *)msg->payload.data);
    } else {
        printf("binary: %zu bytes\n", msg->payload.size);
    }
    fflush(stdout);
    return CFW_OK;
}

int main(int argc, char **argv)
{
    cfw_ws_client_options_t opts;
    cfw_ws_client_t *ws = NULL;
    cfw_tls_ctx_t *tls_ctx = NULL;
    const char *message = NULL;
    const char *ca_file = NULL;
    int i;

    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <host> <port> <path> [--tls <ca.pem>] "
                "[message]\n",
                argv[0]);
        return 2;
    }

    memset(&opts, 0, sizeof(opts));
    opts.host = argv[1];
    {
        long port = strtol(argv[2], NULL, 10);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "invalid port: %s\n", argv[2]);
            return 2;
        }
        opts.port = (uint16_t)port;
    }
    opts.path = argv[3];
    opts.on_message = ws_on_message;
    {
        cfw_duration_t d;
        cfw_duration_from_seconds(10, &d);
        opts.timeout = d;
    }

    i = 4;
    if (i + 1 < argc && strcmp(argv[i], "--tls") == 0) {
        cfw_tls_ctx_options_t topts;
        ca_file = argv[i + 1];
        i += 2;
        memset(&topts, 0, sizeof(topts));
        topts.role = CFW_TLS_CLIENT;
        topts.ca_file = ca_file;
        topts.verify_peer = 1;
        if (cfw_tls_ctx_create(&topts, &tls_ctx) != CFW_OK) {
            fprintf(stderr, "failed to create TLS client context\n");
            return 1;
        }
        opts.tls_ctx = tls_ctx;
    }
    if (i < argc) {
        message = argv[i];
    }

    if (cfw_ws_client_connect(&opts, &ws) != CFW_OK) {
        fprintf(stderr, "connect failed\n");
        cfw_tls_ctx_destroy(tls_ctx);
        return 1;
    }
    fprintf(stderr, "connected to %s://%s:%u%s\n",
            opts.tls_ctx != NULL ? "wss" : "ws", opts.host,
            (unsigned)opts.port, opts.path);

    if (message != NULL) {
        cfw_status_t st =
            cfw_ws_client_send_text(ws, message, strlen(message));
        if (st != CFW_OK) {
            fprintf(stderr, "send failed\n");
            cfw_ws_client_destroy(ws);
            cfw_tls_ctx_destroy(tls_ctx);
            return 1;
        }
    }

    /* Pump until the peer closes or an error ends the connection. */
    while (!cfw_ws_client_is_closed(ws)) {
        cfw_status_t st = cfw_ws_client_pump(ws, NULL);
        if (st != CFW_OK && st != CFW_ERROR_TIMEOUT) {
            fprintf(stderr, "pump failed: %s\n", cfw_status_string(st));
            break;
        }
    }

    cfw_ws_client_destroy(ws);
    cfw_tls_ctx_destroy(tls_ctx);
    return 0;
}
