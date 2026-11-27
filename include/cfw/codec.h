/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_CODEC_H
#define CFW_CODEC_H

#include <cfw/export.h>
#include <cfw/status.h>
#include <cfw/strview.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Byte-to-text codecs (ADR 0047): canonical RFC 4648 base64 (with '='
 * padding) and hex. Intended for tokens, signatures, cookies, and
 * debug output — not for bulk data.
 *
 * Encoders write NUL-terminated output; `written` (optional, may be
 * NULL) excludes the terminator. The *_encode_bound helpers return
 * the `cap` an encoder requires (encoded length plus the terminator),
 * or SIZE_MAX when the length cannot be represented.
 *
 * Decoders are strict: malformed input fails with
 * CFW_ERROR_INVALID_ARGUMENT and an undersized output buffer with
 * CFW_ERROR_OVERFLOW. `written` always receives the decoded length on
 * success.
 *
 * Base64 decoding requires canonical form: length a multiple of 4,
 * '=' padding only at the end (at most two), and zero pad bits. Hex
 * decoding accepts upper- and lowercase digits and requires an even
 * number of digits. An empty input decodes to zero bytes; `out` may
 * be NULL only when the decoded size is zero.
 */

/* Required `cap` for cfw_base64_encode of `data_size` bytes
 * (4*ceil(n/3) chars + NUL), or SIZE_MAX on overflow. */
CFW_API size_t cfw_base64_encode_bound(size_t data_size);

/* Encodes data[0..data_size) as canonical padded base64 into
 * NUL-terminated `out`. Returns CFW_ERROR_OVERFLOW when `cap` is
 * smaller than cfw_base64_encode_bound(data_size),
 * CFW_ERROR_INVALID_ARGUMENT for a NULL `out`, or NULL `data` with
 * data_size > 0. */
CFW_API cfw_status_t cfw_base64_encode(const void *data, size_t data_size,
                                       char *out, size_t cap,
                                       size_t *written);

/* Decodes canonical padded base64 `in` into `out` (at most `cap`
 * bytes; `out` may be NULL only when the decoded size is 0). Returns
 * CFW_ERROR_INVALID_ARGUMENT for malformed input or a NULL `in`,
 * CFW_ERROR_OVERFLOW when `cap` is too small. `written` (optional)
 * receives the decoded length. */
CFW_API cfw_status_t cfw_base64_decode(const cfw_strview_t *in,
                                       unsigned char *out, size_t cap,
                                       size_t *written);

/* Base64url (RFC 4648 section 5, ADR 0059): the URL-safe alphabet
 * ('-' and '_' instead of '+' and '/') for tokens that travel in URLs,
 * query strings, or headers where '+'/'/'/'=' are awkward. The
 * canonical encoder emits NO padding; the decoder accepts unpadded
 * input and, for interoperability, '='-padded input whose total
 * length is a multiple of 4. Same strictness rules as base64 decode:
 * canonical pad bits must be zero and malformed input fails with
 * CFW_ERROR_INVALID_ARGUMENT. */

/* Required `cap` for cfw_base64url_encode of `data_size` bytes
 * (unpadded base64url length + NUL), or SIZE_MAX on overflow. */
CFW_API size_t cfw_base64url_encode_bound(size_t data_size);

/* Encodes data[0..data_size) as unpadded base64url into
 * NUL-terminated `out`. Same error contract as cfw_base64_encode. */
CFW_API cfw_status_t cfw_base64url_encode(const void *data,
                                          size_t data_size, char *out,
                                          size_t cap, size_t *written);

/* Decodes base64url `in` (unpadded, or '='-padded to a multiple of
 * 4) into `out` (at most `cap` bytes; `out` may be NULL only when the
 * decoded size is 0). Same error contract as cfw_base64_decode. */
CFW_API cfw_status_t cfw_base64url_decode(const cfw_strview_t *in,
                                          unsigned char *out, size_t cap,
                                          size_t *written);

/* Required `cap` for cfw_hex_encode of `data_size` bytes
 * (2*data_size chars + NUL), or SIZE_MAX on overflow. */
CFW_API size_t cfw_hex_encode_bound(size_t data_size);

/* Encodes data[0..data_size) as lowercase hex into NUL-terminated
 * `out`. Same error contract as cfw_base64_encode. */
CFW_API cfw_status_t cfw_hex_encode(const void *data, size_t data_size,
                                    char *out, size_t cap,
                                    size_t *written);

/* Decodes hex `in` (either case, even length) into `out` (at most
 * `cap` bytes; `out` may be NULL only when the decoded size is 0).
 * Same error contract as cfw_base64_decode. `written` (optional)
 * receives the decoded length. */
CFW_API cfw_status_t cfw_hex_decode(const cfw_strview_t *in,
                                    unsigned char *out, size_t cap,
                                    size_t *written);

#ifdef __cplusplus
}
#endif

#endif /* CFW_CODEC_H */
