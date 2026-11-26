/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_WS_H
#define CFW_WS_H

#include <cfw/dispatch.h>
#include <cfw/server.h>
#include <cfw/span.h>
#include <cfw/status.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * WebSocket support (RFC 6455, ADR 0035). Two layers:
 *
 * 1. Handshake helpers used inside the dispatch handler: validate the
 *    opening handshake and compute the Sec-WebSocket-Accept value for
 *    the 101 response.
 * 2. Connection object used inside the server upgrade hook
 *    (cfw_server_options_t.upgrade, ADR 0034) for the framed data
 *    path.
 *
 * Cleartext ws:// only for now; wss:// waits on the deferred TLS
 * decision (ADR 0030). No extensions are negotiated.
 */

/* Capacity of the Sec-WebSocket-Accept value including the trailing
 * NUL: base64 of a 20-byte SHA-1 is exactly 28 characters. */
#define CFW_WS_ACCEPT_KEY_CAPACITY 29u

/* Validates the RFC 6455 opening-handshake requirements on a
 * dispatched request: method GET, `Upgrade: websocket`, `Connection`
 * containing the `upgrade` token, `Sec-WebSocket-Version: 13`, and a
 * `Sec-WebSocket-Key` that is base64 decoding to exactly 16 bytes.
 * Returns CFW_OK for a well-formed handshake or a cfw_status_t error
 * otherwise; the handler maps failures to 400/426 as appropriate. */
CFW_API cfw_status_t cfw_ws_handshake_validate(const cfw_request_t *req);

/* Computes the Sec-WebSocket-Accept value for `key` — the raw
 * Sec-WebSocket-Key header value — and writes it NUL-terminated into
 * `out`, which must hold at least CFW_WS_ACCEPT_KEY_CAPACITY bytes.
 * Returns CFW_ERROR_INVALID_ARGUMENT for NULL/undersized arguments or
 * a malformed key. */
CFW_API cfw_status_t cfw_ws_accept_key(const char *key, size_t key_size,
                                       char *out, size_t out_size);

/* Convenience: extracts Sec-WebSocket-Key from the request headers and
 * computes the accept value into `out` (CFW_WS_ACCEPT_KEY_CAPACITY).
 * Fails when the header is absent or malformed; call
 * cfw_ws_handshake_validate first for full handshake checking. */
CFW_API cfw_status_t cfw_ws_handshake_accept(const cfw_request_t *req,
                                             char *out, size_t out_size);

/* ------------------------------------------------------------------ */
/* Connection object (RFC 6455 section 5)                              */
/* ------------------------------------------------------------------ */

typedef enum cfw_ws_opcode {
    CFW_WS_OPCODE_CONTINUATION = 0x0,
    CFW_WS_OPCODE_TEXT = 0x1,
    CFW_WS_OPCODE_BINARY = 0x2,
    CFW_WS_OPCODE_CLOSE = 0x8,
    CFW_WS_OPCODE_PING = 0x9,
    CFW_WS_OPCODE_PONG = 0xa
} cfw_ws_opcode_t;

typedef struct cfw_ws cfw_ws_t;

/* Complete message delivered to on_message. `payload` borrows the
 * connection's assembly buffer and is valid only until the next
 * cfw_ws_on_event call that delivers another message (or until
 * cfw_ws_destroy); copy it if it must outlive the call. opcode is
 * CFW_WS_OPCODE_TEXT or CFW_WS_OPCODE_BINARY — control frames and
 * fragmentation are handled inside the module. */
typedef struct cfw_ws_message {
    cfw_ws_opcode_t opcode;
    cfw_span_t payload;
} cfw_ws_message_t;

/* Invoked once per complete data message. Return CFW_OK to continue;
 * any error closes the connection (a close frame is attempted first). */
typedef cfw_status_t (*cfw_ws_message_fn)(cfw_ws_t *ws,
                                          const cfw_ws_message_t *message,
                                          void *user);

typedef struct cfw_ws_options {
    /* Message callback; NULL discards incoming data messages after
     * validation (ping/pong/close are still handled). */
    cfw_ws_message_fn on_message;
    void *user;
    /* Maximum reassembled message size in bytes; 0 selects 1 MiB.
     * Exceeding it fails the connection with close code 1009. */
    size_t max_message_size;
    /* Maximum payload per outgoing data frame in bytes; larger
     * messages are fragmented into continuation frames. 0 selects
     * 16 KiB. */
    size_t max_frame_payload;
} cfw_ws_options_t;

/* Creates a WebSocket protocol instance bound to an upgraded
 * connection (ADR 0034). Call inside the server upgrade hook, e.g.
 * on CFW_UPGRADE_EV_READY, then forward every subsequent hook event to
 * cfw_ws_on_event. Returns NULL on allocation failure. */
CFW_API cfw_ws_t *cfw_ws_create(cfw_upgraded_conn_t *conn,
                                const cfw_ws_options_t *options);
CFW_API void cfw_ws_destroy(cfw_ws_t *ws);

/* Services one upgrade-hook event: drains available input, parses
 * frames, answers ping with pong, performs the close handshake, and
 * delivers complete messages to on_message. The `events` argument is
 * the CFW_UPGRADE_EV_* mask from the hook and is advisory — input is
 * always drained so staged post-handshake bytes are not missed.
 * Returns CFW_OK while the connection lives; protocol violations are
 * handled internally (close frame + graceful shutdown) and also
 * return CFW_OK. */
CFW_API cfw_status_t cfw_ws_on_event(cfw_ws_t *ws, uint32_t events);

/* Sends a complete text or binary message, fragmented automatically
 * when larger than max_frame_payload. Returns CFW_ERROR_WOULDBLOCK
 * when the connection's send queue cannot accept the whole message —
 * nothing is queued and the caller may retry on a later event.
 * CFW_ERROR_INVALID_ARGUMENT for bad opcode/NULL/closed connection. */
CFW_API cfw_status_t cfw_ws_send(cfw_ws_t *ws, cfw_ws_opcode_t opcode,
                                 const void *data, size_t size);
CFW_API cfw_status_t cfw_ws_send_text(cfw_ws_t *ws, const void *data,
                                      size_t size);
CFW_API cfw_status_t cfw_ws_send_binary(cfw_ws_t *ws, const void *data,
                                        size_t size);

/* Sends a ping or pong control frame (payload <= 125 bytes).
 * CFW_ERROR_INVALID_ARGUMENT when the payload is too large or the
 * connection is closed/closing. */
CFW_API cfw_status_t cfw_ws_send_ping(cfw_ws_t *ws, const void *data,
                                      size_t size);
CFW_API cfw_status_t cfw_ws_send_pong(cfw_ws_t *ws, const void *data,
                                      size_t size);

/* Sends a close frame. status == 0 sends an empty payload (no status);
 * otherwise status is the 2-byte close code and reason (<= 123 bytes,
 * should be valid UTF-8 — it is validated) is appended. After a close
 * frame is queued the connection closes once output drains. */
CFW_API cfw_status_t cfw_ws_send_close(cfw_ws_t *ws, uint16_t status,
                                       const char *reason,
                                       size_t reason_size);

/* Nonzero once the close handshake has completed or the connection is
 * closing/closed; further sends fail. */
CFW_API int cfw_ws_is_closed(const cfw_ws_t *ws);

#ifdef __cplusplus
}
#endif

#endif /* CFW_WS_H */
