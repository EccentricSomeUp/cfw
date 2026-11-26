/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_COOKIE_H
#define CFW_COOKIE_H

#include <cfw/status.h>
#include <cfw/strview.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Cookie parsing and serialization (RFC 6265 / RFC 9110 §5.6, ADR 0045).
 *
 * Request side: cfw_cookie_parse splits a Cookie header value into
 * name/value pairs. Values are NOT percent-decoded (cookies do not
 * use URI encoding); surrounding DQUOTEs are stripped from the
 * returned view. All views reference the header storage — zero-copy,
 * no allocation.
 *
 * Response side: cfw_cookie_serialize renders a Set-Cookie header
 * value into caller-provided storage. Attribute fields left empty or
 * negative are omitted.
 */

typedef struct cfw_cookie_pair {
    cfw_strview_t name;
    /* Empty view when the pair had an empty value. Surrounding
     * DQUOTEs (when both present) are excluded from the view. */
    cfw_strview_t value;
} cfw_cookie_pair_t;

/* Parses the value of a Cookie header: cookie-pair *( ";" cookie-pair ).
 * Empty segments (e.g. ";;") are skipped. Each nonempty segment must
 * contain '='; the name must be a nonempty RFC 9110 token and the
 * value must consist of cookie-octets (optionally DQUOTE-wrapped).
 * Malformed segments fail the whole parse with
 * CFW_ERROR_INVALID_ARGUMENT.
 *
 * Up to `pair_capacity` pairs are stored in `pairs`; extras are
 * counted in `dropped_out` (optional). `count_out` (required)
 * receives the stored count. Returns CFW_OK including for an empty
 * header value (zero pairs). */
CFW_API cfw_status_t cfw_cookie_parse(const cfw_strview_t *header,
                                      cfw_cookie_pair_t *pairs,
                                      size_t pair_capacity,
                                      size_t *count_out,
                                      size_t *dropped_out);

typedef enum cfw_cookie_samesite {
    CFW_COOKIE_SAMESITE_UNSET = 0,
    CFW_COOKIE_SAMESITE_LAX,
    CFW_COOKIE_SAMESITE_STRICT,
    CFW_COOKIE_SAMESITE_NONE
} cfw_cookie_samesite_t;

typedef struct cfw_cookie {
    /* Required: name must be a nonempty token, value must be
     * cookie-octets (or empty). */
    cfw_strview_t name;
    cfw_strview_t value;
    /* Optional attributes; empty views / negative values omit the
     * attribute. path should start with '/' when set. domain is a
     * host name (no leading dot — RFC 6265 ignores it anyway). */
    cfw_strview_t path;
    cfw_strview_t domain;
    /* Seconds until expiry; negative omits Max-Age. */
    int64_t max_age_seconds;
    /* Absolute expiry as Unix seconds for the Expires attribute;
     * negative omits it. Both Max-Age and Expires may be set (Max-Age
     * takes precedence per RFC 6265 §5.3). */
    int64_t expires_unix;
    int secure;      /* nonzero emits "; Secure" */
    int httponly;    /* nonzero emits "; HttpOnly" */
    int partitioned; /* nonzero emits "; Partitioned" (CHIPS) */
    cfw_cookie_samesite_t samesite;
} cfw_cookie_t;

/* Renders `cookie` as a Set-Cookie header value into `dst`
 * (NUL-terminated, `written` excludes the terminator). Returns
 * CFW_ERROR_INVALID_ARGUMENT on invalid name/value/path/domain
 * characters, an out-of-range samesite/expires value, undersized
 * `dst`, or NULL arguments. A value containing characters outside
 * cookie-octets is rejected rather than quoted — callers must supply
 * cookie-safe values (use cfw_uri_decode with appropriate flags to
 * sanitize untrusted input first). */
CFW_API cfw_status_t cfw_cookie_serialize(const cfw_cookie_t *cookie,
                                          char *dst, size_t cap,
                                          size_t *written);

#ifdef __cplusplus
}
#endif

#endif /* CFW_COOKIE_H */
