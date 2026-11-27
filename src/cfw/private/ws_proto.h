/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_PRIVATE_WS_PROTO_H
#define CFW_PRIVATE_WS_PROTO_H

#include <stddef.h>
#include <stdint.h>

/*
 * Internal RFC 6455 protocol helpers shared by the server module
 * (cfw/ws.h, src/cfw/ws.c) and the client module (cfw/ws_client.h,
 * src/cfw/ws_client.c). Not part of the public API.
 */

/* SHA-1 (FIPS 180-4) — used only for the RFC 6455 accept key, which
 * is a protocol transform, not a security boundary. */
typedef struct {
    uint32_t h[5];
    uint64_t total;
    uint8_t block[64];
    size_t block_len;
} cfw_ws_sha1_t;

void cfw_ws_sha1_init(cfw_ws_sha1_t *ctx);
void cfw_ws_sha1_update(cfw_ws_sha1_t *ctx, const uint8_t *data,
                        size_t size);
void cfw_ws_sha1_final(cfw_ws_sha1_t *ctx, uint8_t out[20]);

/* RFC 3629 UTF-8 validation: rejects overlong forms, surrogates,
 * values above U+10FFFF, and truncated sequences. */
int cfw_ws_utf8_valid(const uint8_t *s, size_t n);

/* Close status codes the endpoint fails on (RFC 6455 section 7.4.1
 * and later registrations): undefined/invalid ranges. */
int cfw_ws_close_status_ok(uint16_t status);

#endif /* CFW_PRIVATE_WS_PROTO_H */
