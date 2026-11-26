/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_MULTIPART_H
#define CFW_MULTIPART_H

#include <cfw/span.h>
#include <cfw/status.h>
#include <cfw/strview.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * multipart/form-data parsing (RFC 7578/2046, ADR 0043).
 *
 * cfw_multipart_boundary extracts the boundary parameter from a
 * Content-Type header value; cfw_multipart_parse splits a buffered
 * request body into parts. Both operate on caller-provided views —
 * no allocation. Parsed name/filename values are unescaped
 * (quoted-string '\"' and '\\') into caller scratch; part bodies are
 * zero-copy views into the request body.
 *
 * This module parses buffered bodies (cfw_request_t.body). Bodies
 * delivered through the server's upload-streaming hooks are not
 * visible here; applications needing multipart over streams must
 * parse incrementally themselves.
 */

typedef struct cfw_multipart_part {
    /* Content-Disposition 'name' parameter, unescaped. Empty when the
     * part carried no usable Content-Disposition. */
    cfw_strview_t name;
    /* Content-Disposition 'filename' parameter, unescaped. Empty when
     * absent. */
    cfw_strview_t filename;
    /* Raw Content-Type header value of the part (still encoded),
     * empty when absent. */
    cfw_strview_t content_type;
    /* The part's raw header block (name/value lines, without the
     * terminating blank line), for application-specific headers. */
    cfw_strview_t header_block;
    /* The part body — a view into the request body, excluding the
     * CRLF that precedes the next boundary delimiter. */
    cfw_span_t body;
} cfw_multipart_part_t;

/* Extracts the boundary parameter from a Content-Type field value
 * such as "multipart/form-data; boundary=abc". The boundary may be a
 * bare token or a quoted-string; it is returned unquoted but NOT
 * unescaped into scratch — quoted boundary values containing escapes
 * are rejected (RFC 2046 boundaries never need them). Fails with
 * CFW_ERROR_INVALID_ARGUMENT when no boundary parameter exists, it is
 * empty, longer than 70 bytes, or the value is malformed. */
CFW_API cfw_status_t cfw_multipart_boundary(const cfw_strview_t *content_type,
                                            cfw_strview_t *boundary);

/* Parses `body` into up to `part_capacity` parts.
 *
 * `scratch` receives unescaped name/filename bytes and must be at
 * least `body->size` bytes — unescaped parameters never exceed the
 * header bytes they came from, and headers are a subset of the body.
 * A smaller buffer fails with CFW_ERROR_INVALID_ARGUMENT before any
 * part is stored.
 *
 * `count_out` (required) receives the number of parts stored.
 * `dropped_out` (optional) receives parts beyond capacity.
 *
 * The grammar requires the closing "--boundary--" delimiter; a body
 * that ends mid-part or lacks the close delimiter fails with
 * CFW_ERROR_INVALID_ARGUMENT. Transport padding (SP/HTAB) around
 * delimiters and a preamble/epilogue are tolerated per RFC 2046.
 * Returns CFW_OK on success. */
CFW_API cfw_status_t cfw_multipart_parse(
    const cfw_span_t *body, const cfw_strview_t *boundary,
    cfw_multipart_part_t *parts, size_t part_capacity, char *scratch,
    size_t scratch_size, size_t *count_out, size_t *dropped_out);

#ifdef __cplusplus
}
#endif

#endif /* CFW_MULTIPART_H */
