/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_HTTP_H
#define CFW_HTTP_H

#include <cfw/export.h>
#include <cfw/status.h>
#include <cfw/strview.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * HTTP/1.x primitives. Parsing functions operate on cfw_strview_t inputs
 * and never read past the view bounds. Unknown or malformed tokens are
 * reported distinctly from valid-but-uncommon values so callers can choose
 * between rejecting and tolerating input.
 */

typedef enum cfw_http_method {
    CFW_HTTP_METHOD_UNKNOWN = 0,
    CFW_HTTP_METHOD_GET,
    CFW_HTTP_METHOD_HEAD,
    CFW_HTTP_METHOD_POST,
    CFW_HTTP_METHOD_PUT,
    CFW_HTTP_METHOD_DELETE,
    CFW_HTTP_METHOD_CONNECT,
    CFW_HTTP_METHOD_OPTIONS,
    CFW_HTTP_METHOD_TRACE,
    CFW_HTTP_METHOD_PATCH
} cfw_http_method_t;

typedef struct cfw_http_version {
    uint8_t major;
    uint8_t minor;
} cfw_http_version_t;

typedef enum cfw_http_status {
    CFW_HTTP_STATUS_CONTINUE = 100,
    CFW_HTTP_STATUS_SWITCHING_PROTOCOLS = 101,
    CFW_HTTP_STATUS_OK = 200,
    CFW_HTTP_STATUS_CREATED = 201,
    CFW_HTTP_STATUS_ACCEPTED = 202,
    CFW_HTTP_STATUS_NO_CONTENT = 204,
    CFW_HTTP_STATUS_MOVED_PERMANENTLY = 301,
    CFW_HTTP_STATUS_FOUND = 302,
    CFW_HTTP_STATUS_NOT_MODIFIED = 304,
    CFW_HTTP_STATUS_BAD_REQUEST = 400,
    CFW_HTTP_STATUS_UNAUTHORIZED = 401,
    CFW_HTTP_STATUS_FORBIDDEN = 403,
    CFW_HTTP_STATUS_NOT_FOUND = 404,
    CFW_HTTP_STATUS_METHOD_NOT_ALLOWED = 405,
    CFW_HTTP_STATUS_REQUEST_TIMEOUT = 408,
    CFW_HTTP_STATUS_CONFLICT = 409,
    CFW_HTTP_STATUS_PAYLOAD_TOO_LARGE = 413,
    CFW_HTTP_STATUS_URI_TOO_LONG = 414,
    CFW_HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE = 415,
    CFW_HTTP_STATUS_TOO_MANY_REQUESTS = 429,
    CFW_HTTP_STATUS_INTERNAL_SERVER_ERROR = 500,
    CFW_HTTP_STATUS_NOT_IMPLEMENTED = 501,
    CFW_HTTP_STATUS_BAD_GATEWAY = 502,
    CFW_HTTP_STATUS_SERVICE_UNAVAILABLE = 503,
    CFW_HTTP_STATUS_GATEWAY_TIMEOUT = 504
} cfw_http_status_t;

typedef struct cfw_http_header {
    cfw_strview_t name;
    cfw_strview_t value;
} cfw_http_header_t;

typedef struct cfw_http_request_line {
    cfw_http_method_t method;
    cfw_strview_t target;
    cfw_http_version_t version;
} cfw_http_request_line_t;

/* Parses one request line (no trailing CRLF) in origin, absolute,
 * authority, or asterisk form. The target is validated only for
 * control characters and whitespace; form-specific validation belongs to
 * the routing layer. */
CFW_API cfw_status_t cfw_http_request_line_parse(
    const cfw_strview_t *line, cfw_http_request_line_t *request_line);

typedef struct cfw_http_request_head {
    cfw_http_request_line_t request_line;
    /* Header section including its terminating empty line; iterate with
     * cfw_http_headers_next, which stops at the empty line. */
    cfw_strview_t headers;
} cfw_http_request_head_t;

/* Parses a complete request head: request line, header lines, and the
 * mandatory empty-line terminator. The input must contain exactly the head
 * bytes and nothing after the final CRLF. */
CFW_API cfw_status_t cfw_http_request_head_parse(
    const cfw_strview_t *head, cfw_http_request_head_t *request_head);

typedef enum cfw_http_body_framing {
    CFW_HTTP_BODY_NONE = 0,
    CFW_HTTP_BODY_CONTENT_LENGTH,
    CFW_HTTP_BODY_CHUNKED
} cfw_http_body_framing_t;

typedef struct cfw_http_message_framing {
    cfw_http_body_framing_t framing;
    /* Valid only when framing == CFW_HTTP_BODY_CONTENT_LENGTH. */
    uint64_t content_length;
} cfw_http_message_framing_t;

/* Determines request body framing from a parsed head using
 * request-smuggling-resistant rules:
 * - Content-Length must be 1*DIGIT; multiple occurrences must carry the
 *   same numeric value.
 * - Transfer-Encoding must appear at most once and its value must be
 *   exactly "chunked" (case-insensitive).
 * - Content-Length combined with Transfer-Encoding is rejected.
 * Returns CFW_ERROR_INVALID_ARGUMENT on any violation; the caller maps it
 * to 400 Bad Request. */
CFW_API cfw_status_t cfw_http_request_framing(
    const cfw_http_request_head_t *request_head,
    cfw_http_message_framing_t *framing);

/* Incremental chunked-transfer decoder. Consumes framing bytes and yields
 * decoded body bytes as a zero-copy view into the input; a single call may
 * return at most one contiguous body span, stopping at chunk boundaries so
 * the caller re-invokes with the remaining input. */

#define CFW_HTTP_CHUNKED_MAX_LINE 4096u

typedef enum cfw_http_chunked_state {
    CFW_HTTP_CHUNKED_STATE_SIZE = 0,
    CFW_HTTP_CHUNKED_STATE_DATA,
    CFW_HTTP_CHUNKED_STATE_DATA_CRLF,
    CFW_HTTP_CHUNKED_STATE_TRAILER,
    CFW_HTTP_CHUNKED_STATE_DONE
} cfw_http_chunked_state_t;

typedef struct cfw_http_chunked_decoder {
    cfw_http_chunked_state_t state;
    uint64_t remaining;
    int cr_pending;
    int done;
    size_t line_len;
    char line_buf[CFW_HTTP_CHUNKED_MAX_LINE];
} cfw_http_chunked_decoder_t;

CFW_API void cfw_http_chunked_decoder_init(cfw_http_chunked_decoder_t *decoder);

/* Feeds input bytes to the decoder. On return:
 * - output receives the decoded body bytes produced by this call (a view
 *   into input; empty when only framing bytes were consumed);
 * - consumed receives the number of input bytes used — bytes past it
 *   belong to the next pipelined message;
 * - finished is set to 1 once the terminal chunk and trailers are done.
 * The decoder must not be called again after finished == 1. Chunk-size and
 * trailer lines are bounded by CFW_HTTP_CHUNKED_MAX_LINE; chunk extensions
 * are accepted but must be printable ASCII. Trailer fields must satisfy the
 * same grammar as header fields. Bare LF and malformed hex sizes are
 * rejected with CFW_ERROR_INVALID_ARGUMENT. */
CFW_API cfw_status_t cfw_http_chunked_decode(
    cfw_http_chunked_decoder_t *decoder, const cfw_span_t *input,
    cfw_span_t *output, size_t *consumed, int *finished);

CFW_API cfw_status_t cfw_http_headers_next(const cfw_strview_t *block,
                                           size_t *cursor,
                                           cfw_http_header_t *header,
                                           int *done);

CFW_API const char *cfw_http_method_name(cfw_http_method_t method);
CFW_API cfw_status_t cfw_http_method_parse(const cfw_strview_t *token,
                                           cfw_http_method_t *method);
CFW_API cfw_status_t cfw_http_version_format(const cfw_http_version_t *version,
                                             char *buffer, size_t capacity);
CFW_API cfw_status_t cfw_http_version_parse(const cfw_strview_t *token,
                                            cfw_http_version_t *version);
CFW_API const char *cfw_http_status_reason(cfw_http_status_t status);
CFW_API cfw_status_t cfw_http_status_parse(uint32_t code,
                                           cfw_http_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* CFW_HTTP_H */
