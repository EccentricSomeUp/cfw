/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_URI_H
#define CFW_URI_H

#include <cfw/status.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Percent-decoding for URI components (RFC 3986 §2.1).
 *
 * cfw_uri_decode converts %XX escape sequences to their byte values.
 * Decoding never expands input, so a destination buffer of `len` bytes
 * plus one byte for the terminating NUL always suffices. The output is
 * NUL-terminated when it fits; `out_len` reports the decoded length
 * (excluding the terminator).
 *
 * Path segments and other URI components keep '+' literal; pass
 * CFW_URI_DECODE_PLUS_AS_SPACE for application/x-www-form-urlencoded
 * components such as query strings.
 */

/* '+' decodes to SP (form-urlencoding semantics). Without this flag
 * '+' is copied verbatim. */
#define CFW_URI_DECODE_PLUS_AS_SPACE ((uint32_t)1u << 0)

/* A decoded (or literal) NUL byte is rejected. Use for any output
 * that will be consumed as a C string or embedded in a path. */
#define CFW_URI_DECODE_REJECT_NUL ((uint32_t)1u << 1)

/* A backslash — literal or percent-decoded — is rejected. Use for
 * filesystem paths, where '\\' is a separator on Windows. */
#define CFW_URI_DECODE_REJECT_BACKSLASH ((uint32_t)1u << 2)

/* Decodes src[0..len) into dst. `cap` is the dst capacity in bytes and
 * must be at least out_len + 1 so the result can be NUL-terminated;
 * smaller capacities yield CFW_ERROR_INVALID_ARGUMENT.
 *
 * Returns CFW_ERROR_INVALID_ARGUMENT on a malformed % escape (fewer
 * than two hex digits follow '%'), a truncated escape at end of input,
 * insufficient dst capacity, or a byte rejected by the flags. */
CFW_API cfw_status_t cfw_uri_decode(const char *src, size_t len,
                                   char *dst, size_t cap,
                                   size_t *out_len, uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif /* CFW_URI_H */
