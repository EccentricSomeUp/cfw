/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_WS_CLIENT_H
#define CFW_WS_CLIENT_H

#include <cfw/http.h>
#include <cfw/status.h>
#include <cfw/tls.h>
#include <cfw/time.h>
#include <cfw/ws.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * WebSocket client (RFC 6455, ADR 0071): the mirror image of the
 * server module in cfw/ws.h. Synchronous-style and single-threaded,
 * like cfw/http_client.h:
 *
 *   cfw_ws_client_connect() resolves, connects, optionally handshakes
 *   TLS (wss://), performs the HTTP Upgrade exchange, and validates
 *   the 101 response including Sec-WebSocket-Accept — all bounded by
 *   options.timeout. DNS resolution itself is blocking and NOT
 *   covered by the timeout (same contract as cfw_http_client).
 *
 *   cfw_ws_client_pump() is the pull-style event driver: it waits for
 *   socket/TLS readiness, feeds the incremental frame parser,
 *   auto-answers ping with pong, performs the close handshake, and
 *   invokes on_message once per complete reassembled message. Call it
 *   in a loop until it returns an error or is_closed() is true.
 *
 *   cfw_ws_client_send_*() writes one masked frame with a
 *   deadline-bounded blocking write; they are safe to call from
 *   on_message and between pump calls (single-threaded contract).
 *
 * Frame semantics mirror the server: 7/16/64-bit lengths,
 * fragmentation/reassembly up to max_message_size, UTF-8 validation
 * on text messages, control frames limited to 125 bytes and never
 * fragmented. Inbound frames MUST be unmasked and outbound frames are
 * always masked (client role, RFC 6455 section 5.1/5.3). No
 * extensions are negotiated.
 */

typedef struct cfw_ws_client cfw_ws_client_t;

/* Complete message delivered to on_message. `payload` borrows the
 * connection's assembly buffer and is valid only until the next
 * pump call that delivers another message (or until destroy); copy
 * it if it must outlive the call. opcode is CFW_WS_OPCODE_TEXT or
 * CFW_WS_OPCODE_BINARY. */
typedef cfw_status_t (*cfw_ws_client_message_fn)(
    cfw_ws_client_t *ws, const cfw_ws_message_t *message, void *user);

typedef struct {
    /* Server host: DNS name or numeric IP literal. Required. */
    const char *host;
    /* Server port. Required, nonzero. */
    uint16_t port;
    /* Request target: must start with '/' and satisfy the HTTP
     * request-target grammar (no SP/CTL bytes), e.g.
     * "/ws?room=1". Required. */
    const char *path;
    /* Additional request headers (e.g. Cookie or Authorization).
     * Names must be HTTP tokens and values must satisfy the field
     * grammar; framework-managed names are rejected: host, connection,
     * upgrade, transfer-encoding, keep-alive, te, trailer,
     * proxy-*, and every sec-websocket-* header. May be NULL. */
    const cfw_http_header_t *headers;
    size_t header_count;
    /* TLS context for wss://; NULL selects cleartext ws://. For
     * hostname verification set verify_peer/ca_file on the ctx —
     * the SNI name is always request->host. */
    cfw_tls_ctx_t *tls_ctx;
    /* Per-operation deadline (connect, handshake, TLS, each pump
     * wait, each send). 0 selects the default (10 s). nanoseconds
     * must be >= 0. */
    cfw_duration_t timeout;
    /* Maximum reassembled incoming message size; 0 selects 1 MiB.
     * Exceeding it fails the connection with close code 1009. */
    size_t max_message_size;
    /* Message callback; NULL discards incoming data messages after
     * validation (ping/pong/close are still handled). */
    cfw_ws_client_message_fn on_message;
    void *user;
} cfw_ws_client_options_t;

/* Connects and performs the opening handshake. On success *out owns
 * a connected client; destroy it with cfw_ws_client_destroy.
 * Returns CFW_ERROR_INVALID_ARGUMENT for missing/invalid options or
 * forbidden header names, CFW_ERROR_TIMEOUT when the deadline
 * expires, CFW_ERROR_IO on connect/TLS/HTTP failure or a non-101
 * response, CFW_ERROR_NOMEM on allocation failure. */
CFW_API cfw_status_t cfw_ws_client_connect(
    const cfw_ws_client_options_t *options, cfw_ws_client_t **out);
CFW_API void cfw_ws_client_destroy(cfw_ws_client_t *ws);

/* Nonzero once the close handshake completed or the transport
 * failed; the client is dead and pump/send return errors. */
CFW_API int cfw_ws_client_is_closed(const cfw_ws_client_t *ws);

/* Sends one data or control message. Data opcodes (TEXT/BINARY) may
 * exceed the frame size and are fragmented automatically; PING/PONG
 * payloads are limited to 125 bytes. CLOSE sends a close frame —
 * optionally carrying a status via cfw_ws_client_send_close —
 * and begins the closing handshake. No further sends are accepted
 * once a close frame has been sent. Returns
 * CFW_ERROR_INVALID_ARGUMENT for a bad opcode/oversized control
 * payload/NULL arguments/closed or closing connection,
 * CFW_ERROR_TIMEOUT or CFW_ERROR_IO on transport failure. */
CFW_API cfw_status_t cfw_ws_client_send(cfw_ws_client_t *ws,
                                       cfw_ws_opcode_t opcode,
                                       const void *data, size_t size);
CFW_API cfw_status_t cfw_ws_client_send_text(cfw_ws_client_t *ws,
                                             const void *data,
                                             size_t size);
CFW_API cfw_status_t cfw_ws_client_send_binary(cfw_ws_client_t *ws,
                                               const void *data,
                                               size_t size);
CFW_API cfw_status_t cfw_ws_client_send_ping(cfw_ws_client_t *ws,
                                             const void *data,
                                             size_t size);
CFW_API cfw_status_t cfw_ws_client_send_pong(cfw_ws_client_t *ws,
                                             const void *data,
                                             size_t size);
/* Sends a close frame with a status code (and optional UTF-8 reason
 * appended after the 2-byte status). status must satisfy
 * cfw_ws_close_status_ok semantics; 0 sends an empty close payload. */
CFW_API cfw_status_t cfw_ws_client_send_close(cfw_ws_client_t *ws,
                                              uint16_t status,
                                              const char *reason,
                                              size_t reason_size);

/* Services one round of connection activity: waits up to `timeout`
 * for socket/TLS readiness (NULL waits indefinitely), drains
 * decrypted TLS bytes, feeds the frame parser, auto-answers ping,
 * performs close handling, and delivers complete messages to
 * on_message. Returns CFW_OK on progress (including timeout-free
 * idle wakeups that produced no message), CFW_ERROR_TIMEOUT when
 * `timeout` elapsed with no I/O, or an IO/protocol status on
 * failure. Once is_closed() is true it returns
 * CFW_ERROR_INVALID_ARGUMENT. */
CFW_API cfw_status_t cfw_ws_client_pump(cfw_ws_client_t *ws,
                                        const cfw_duration_t *timeout);

#ifdef __cplusplus
}
#endif

#endif /* CFW_WS_CLIENT_H */
