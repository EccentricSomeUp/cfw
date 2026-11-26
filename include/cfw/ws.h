/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_WS_H
#define CFW_WS_H

#include <cfw/dispatch.h>
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

#ifdef __cplusplus
}
#endif

#endif /* CFW_WS_H */
