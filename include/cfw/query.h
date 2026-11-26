/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_QUERY_H
#define CFW_QUERY_H

#include <cfw/span.h>
#include <cfw/status.h>
#include <cfw/strview.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Query-string parsing (application/x-www-form-urlencoded, ADR 0042).
 *
 * cfw_query_parse decodes a query component — the text after '?' in a
 * request-target, obtainable via cfw_http_target_path's sibling —
 * into name/value pairs. Both names and values are percent-decoded
 * ('+' becomes SP, %XX becomes the byte) into caller-provided scratch;
 * the returned strviews reference that scratch and remain valid until
 * the caller reuses it.
 *
 * Grammar: pairs are separated by '&'; each pair is 'name', 'name=',
 * or 'name=value' (the first '=' splits; later '=' bytes are literal
 * value data). Empty segments ('&&', leading/trailing '&') are
 * skipped. A '%' not followed by two hex digits is a parse error.
 */

typedef struct cfw_query_pair {
    cfw_strview_t name;
    /* Empty view when the pair carried no '=' or an empty value. */
    cfw_strview_t value;
} cfw_query_pair_t;

/* Parses `query` into up to `pair_capacity` pairs stored in `pairs`.
 *
 * `scratch` receives the decoded bytes and must be at least
 * `query->size` bytes — decoding never produces more bytes than the
 * input consumed them from. Pass a buffer of query->size (or larger)
 * to guarantee success; a smaller buffer fails with
 * CFW_ERROR_INVALID_ARGUMENT before any pair is stored.
 *
 * `count_out` (required) receives the number of pairs stored.
 * `dropped_out` (optional, may be NULL) receives the number of pairs
 * beyond `pair_capacity` that were parsed but not stored.
 *
 * Returns CFW_OK on success (including an empty query, which yields
 * zero pairs) or CFW_ERROR_INVALID_ARGUMENT on NULL/undersized
 * arguments or malformed percent-encoding. */
CFW_API cfw_status_t cfw_query_parse(const cfw_strview_t *query,
                                     cfw_query_pair_t *pairs,
                                     size_t pair_capacity,
                                     char *scratch, size_t scratch_size,
                                     size_t *count_out,
                                     size_t *dropped_out);

#ifdef __cplusplus
}
#endif

#endif /* CFW_QUERY_H */
