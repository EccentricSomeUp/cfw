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
