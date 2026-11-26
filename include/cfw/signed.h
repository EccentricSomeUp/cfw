/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_SIGNED_H
#define CFW_SIGNED_H

#include <cfw/export.h>
#include <cfw/status.h>
#include <cfw/strview.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Signed tokens (ADR 0048): an authenticated-envelope format for
 * cookies, CSRF tokens, password-reset tokens, and similar
 * caller-bound payloads.
 *
 * Wire format: "v1." + base64(payload) + "." + base64(HMAC-SHA256).
 * The MAC covers the literal prefix "v1." + base64(payload), binding
 * the version and the encoded payload; the payload itself is never
 * interpreted by this module. Both base64 segments use canonical
 * RFC 4648 padded form (cfw/codec.h), which is safe inside cookie
 * values and header fields.
 *
 * This provides integrity and authenticity, NOT confidentiality —
 * the payload is merely base64-encoded. Do not put secrets in the
 * payload. The format carries no expiry or nonce; callers embed
 * timestamps/nonces in the payload and enforce policy themselves.
 *
 * `key` is the HMAC key (any length; see cfw_hmac_sha256). Keep it
 * secret and stable — rotating the key invalidates all outstanding
 * tokens.
 */

/* Required `cap` for cfw_signed_encode of a `payload_size`-byte
 * payload (prefix + base64 + '.' + 44-char MAC + NUL), or SIZE_MAX
 * on overflow. Independent of the key. */
CFW_API size_t cfw_signed_encode_bound(size_t payload_size);

/* Encodes `payload` into a NUL-terminated signed token in `out`.
 * `written` (optional) excludes the terminator. Returns
 * CFW_ERROR_OVERFLOW when `cap` is undersized,
 * CFW_ERROR_INVALID_ARGUMENT for NULL `out`, NULL `key` with
 * key_size > 0, or NULL `payload` with payload_size > 0. */
CFW_API cfw_status_t cfw_signed_encode(const void *key, size_t key_size,
                                       const void *payload,
                                       size_t payload_size, char *out,
                                       size_t cap, size_t *written);

/* Verifies `token` and recovers the payload into `payload_out`
 * (at most `payload_cap` bytes; `payload_out` may be NULL only when
 * the payload is empty). `payload_size` (optional) receives the
 * decoded length. Returns CFW_OK on success;
 * CFW_ERROR_INVALID_ARGUMENT for malformed tokens or MAC mismatch
 * (deliberately indistinguishable), CFW_ERROR_OVERFLOW when
 * `payload_cap` is too small. Timing: the MAC comparison is
 * constant-time; the parse work is proportional to token length. */
CFW_API cfw_status_t cfw_signed_decode(const void *key, size_t key_size,
                                       const cfw_strview_t *token,
                                       unsigned char *payload_out,
                                       size_t payload_cap,
                                       size_t *payload_size);

#ifdef __cplusplus
}
#endif

#endif /* CFW_SIGNED_H */
