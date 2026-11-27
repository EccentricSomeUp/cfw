/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Coverage-guided fuzz target for the private WebSocket protocol
 * helpers (ADR 0077): the UTF-8 validator that guards text-frame
 * payloads, the SHA-1 implementation behind the accept-key
 * handshake, and the close-status allowlist. Links ws_proto.c
 * directly, like the pollset compile fixtures.
 */

#include "private/ws_proto.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Incremental UTF-8 validation at several split points derived
     * from the input, exercising both the fast path and the
     * multi-byte state machine. */
    if (size > 0) {
        size_t split = size / 2;
        size_t third = size / 3;

        (void)cfw_ws_utf8_valid(data, size);
        if (split > 0 && split < size) {
            (void)cfw_ws_utf8_valid(data, split);
            (void)cfw_ws_utf8_valid(data + split, size - split);
        }
        if (third > 0 && third < size) {
            (void)cfw_ws_utf8_valid(data + third, size - third);
        }
    }

    /* SHA-1 accept-key primitive over arbitrary input. */
    {
        cfw_ws_sha1_t ctx;
        uint8_t digest[20];

        cfw_ws_sha1_init(&ctx);
        cfw_ws_sha1_update(&ctx, data, size);
        cfw_ws_sha1_final(&ctx, digest);
        (void)digest;
    }

    /* Close-status validation over 16-bit windows of the input. */
    {
        size_t i;
        size_t limit = size < 64 ? size : 64;
        for (i = 0; i + 1 < limit; i += 2) {
            uint16_t status =
                (uint16_t)(((uint16_t)data[i] << 8) | (uint16_t)data[i + 1]);
            (void)cfw_ws_close_status_ok(status);
        }
    }
    return 0;
}
