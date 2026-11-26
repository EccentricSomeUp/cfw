/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/http.h>

#include <stdio.h>
#include <string.h>

static int cfw_strview_equals_cstr(const cfw_strview_t *view, const char *text)
{
    size_t len = 0;
    int equal;
    cfw_strview_t expected;
    cfw_status_t status;

    while (text[len] != '\0') {
        len++;
    }
    status = cfw_strview_init(&expected, text, len);
    if (status != CFW_OK) {
        return 0;
    }
    status = cfw_strview_equal(view, &expected, &equal);
    return status == CFW_OK && equal;
}

const char *cfw_http_method_name(cfw_http_method_t method)
{
    switch (method) {
    case CFW_HTTP_METHOD_GET:
        return "GET";
    case CFW_HTTP_METHOD_HEAD:
        return "HEAD";
    case CFW_HTTP_METHOD_POST:
        return "POST";
    case CFW_HTTP_METHOD_PUT:
        return "PUT";
    case CFW_HTTP_METHOD_DELETE:
        return "DELETE";
    case CFW_HTTP_METHOD_CONNECT:
        return "CONNECT";
    case CFW_HTTP_METHOD_OPTIONS:
        return "OPTIONS";
    case CFW_HTTP_METHOD_TRACE:
        return "TRACE";
    case CFW_HTTP_METHOD_PATCH:
        return "PATCH";
    case CFW_HTTP_METHOD_UNKNOWN:
    default:
        return NULL;
    }
}

cfw_status_t cfw_http_method_parse(const cfw_strview_t *token,
                                   cfw_http_method_t *method)
{
    static const struct {
        const char *name;
        cfw_http_method_t method;
    } methods[] = {
        {"GET", CFW_HTTP_METHOD_GET},
        {"HEAD", CFW_HTTP_METHOD_HEAD},
        {"POST", CFW_HTTP_METHOD_POST},
        {"PUT", CFW_HTTP_METHOD_PUT},
        {"DELETE", CFW_HTTP_METHOD_DELETE},
        {"CONNECT", CFW_HTTP_METHOD_CONNECT},
        {"OPTIONS", CFW_HTTP_METHOD_OPTIONS},
        {"TRACE", CFW_HTTP_METHOD_TRACE},
        {"PATCH", CFW_HTTP_METHOD_PATCH},
    };
    size_t i;

    if (token == NULL || token->data == NULL || method == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    for (i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        if (cfw_strview_equals_cstr(token, methods[i].name)) {
            *method = methods[i].method;
            return CFW_OK;
        }
    }

    *method = CFW_HTTP_METHOD_UNKNOWN;
    return CFW_OK;
}

cfw_status_t cfw_http_version_format(const cfw_http_version_t *version,
                                     char *buffer, size_t capacity)
{
    int written;

    if (version == NULL || buffer == NULL || capacity == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    written = snprintf(buffer, capacity, "HTTP/%u.%u",
                       (unsigned)version->major, (unsigned)version->minor);
    if (written < 0 || (size_t)written >= capacity) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    return CFW_OK;
}

cfw_status_t cfw_http_version_parse(const cfw_strview_t *token,
                                    cfw_http_version_t *version)
{
    size_t i;
    uint32_t major = 0;
    uint32_t minor = 0;
    int digits_major = 0;
    int digits_minor = 0;

    if (token == NULL || token->data == NULL || version == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    /* Expected form: "HTTP/" DIGIT+ "." DIGIT+ */
    if (token->size < 8 || token->data[0] != 'H' || token->data[1] != 'T' ||
        token->data[2] != 'T' || token->data[3] != 'P' || token->data[4] != '/' ||
        token->data[5] < '0' || token->data[5] > '9') {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    i = 5;
    {
        size_t major_start = i;
        while (i < token->size && token->data[i] >= '0' && token->data[i] <= '9') {
            major = major * 10 + (uint32_t)(token->data[i] - '0');
            if (major > 100) {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
            digits_major++;
            i++;
        }
        if (digits_major == 0 || i >= token->size ||
            token->data[i] != '.' ||
            (digits_major > 1 && token->data[major_start] == '0')) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
    }
    i++;
    {
        size_t minor_start = i;
        while (i < token->size && token->data[i] >= '0' && token->data[i] <= '9') {
            minor = minor * 10 + (uint32_t)(token->data[i] - '0');
            if (minor > 100) {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
            digits_minor++;
            i++;
        }
        if (digits_minor == 0 || i != token->size ||
            (digits_minor > 1 && token->data[minor_start] == '0')) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
    }
    if (major > UINT8_MAX || minor > UINT8_MAX) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    version->major = (uint8_t)major;
    version->minor = (uint8_t)minor;
    return CFW_OK;
}

const char *cfw_http_status_reason(cfw_http_status_t status)
{
    switch (status) {
    case CFW_HTTP_STATUS_CONTINUE:
        return "Continue";
    case CFW_HTTP_STATUS_SWITCHING_PROTOCOLS:
        return "Switching Protocols";
    case CFW_HTTP_STATUS_OK:
        return "OK";
    case CFW_HTTP_STATUS_CREATED:
        return "Created";
    case CFW_HTTP_STATUS_ACCEPTED:
        return "Accepted";
    case CFW_HTTP_STATUS_NO_CONTENT:
        return "No Content";
    case CFW_HTTP_STATUS_MOVED_PERMANENTLY:
        return "Moved Permanently";
    case CFW_HTTP_STATUS_FOUND:
        return "Found";
    case CFW_HTTP_STATUS_NOT_MODIFIED:
        return "Not Modified";
    case CFW_HTTP_STATUS_BAD_REQUEST:
        return "Bad Request";
    case CFW_HTTP_STATUS_UNAUTHORIZED:
        return "Unauthorized";
    case CFW_HTTP_STATUS_FORBIDDEN:
        return "Forbidden";
    case CFW_HTTP_STATUS_NOT_FOUND:
        return "Not Found";
    case CFW_HTTP_STATUS_METHOD_NOT_ALLOWED:
        return "Method Not Allowed";
    case CFW_HTTP_STATUS_REQUEST_TIMEOUT:
        return "Request Timeout";
    case CFW_HTTP_STATUS_CONFLICT:
        return "Conflict";
    case CFW_HTTP_STATUS_PAYLOAD_TOO_LARGE:
        return "Payload Too Large";
    case CFW_HTTP_STATUS_URI_TOO_LONG:
        return "URI Too Long";
    case CFW_HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE:
        return "Unsupported Media Type";
    case CFW_HTTP_STATUS_TOO_MANY_REQUESTS:
        return "Too Many Requests";
    case CFW_HTTP_STATUS_INTERNAL_SERVER_ERROR:
        return "Internal Server Error";
    case CFW_HTTP_STATUS_NOT_IMPLEMENTED:
        return "Not Implemented";
    case CFW_HTTP_STATUS_BAD_GATEWAY:
        return "Bad Gateway";
    case CFW_HTTP_STATUS_SERVICE_UNAVAILABLE:
        return "Service Unavailable";
    case CFW_HTTP_STATUS_GATEWAY_TIMEOUT:
        return "Gateway Timeout";
    default:
        return NULL;
    }
}

cfw_status_t cfw_http_status_parse(uint32_t code, cfw_http_status_t *status)
{
    if (status == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    switch (code) {
    case 100: *status = CFW_HTTP_STATUS_CONTINUE; return CFW_OK;
    case 101: *status = CFW_HTTP_STATUS_SWITCHING_PROTOCOLS; return CFW_OK;
    case 200: *status = CFW_HTTP_STATUS_OK; return CFW_OK;
    case 201: *status = CFW_HTTP_STATUS_CREATED; return CFW_OK;
    case 202: *status = CFW_HTTP_STATUS_ACCEPTED; return CFW_OK;
    case 204: *status = CFW_HTTP_STATUS_NO_CONTENT; return CFW_OK;
    case 301: *status = CFW_HTTP_STATUS_MOVED_PERMANENTLY; return CFW_OK;
    case 302: *status = CFW_HTTP_STATUS_FOUND; return CFW_OK;
    case 304: *status = CFW_HTTP_STATUS_NOT_MODIFIED; return CFW_OK;
    case 400: *status = CFW_HTTP_STATUS_BAD_REQUEST; return CFW_OK;
    case 401: *status = CFW_HTTP_STATUS_UNAUTHORIZED; return CFW_OK;
    case 403: *status = CFW_HTTP_STATUS_FORBIDDEN; return CFW_OK;
    case 404: *status = CFW_HTTP_STATUS_NOT_FOUND; return CFW_OK;
    case 405: *status = CFW_HTTP_STATUS_METHOD_NOT_ALLOWED; return CFW_OK;
    case 408: *status = CFW_HTTP_STATUS_REQUEST_TIMEOUT; return CFW_OK;
    case 409: *status = CFW_HTTP_STATUS_CONFLICT; return CFW_OK;
    case 413: *status = CFW_HTTP_STATUS_PAYLOAD_TOO_LARGE; return CFW_OK;
    case 414: *status = CFW_HTTP_STATUS_URI_TOO_LONG; return CFW_OK;
    case 415: *status = CFW_HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE; return CFW_OK;
    case 429: *status = CFW_HTTP_STATUS_TOO_MANY_REQUESTS; return CFW_OK;
    case 500: *status = CFW_HTTP_STATUS_INTERNAL_SERVER_ERROR; return CFW_OK;
    case 501: *status = CFW_HTTP_STATUS_NOT_IMPLEMENTED; return CFW_OK;
    case 502: *status = CFW_HTTP_STATUS_BAD_GATEWAY; return CFW_OK;
    case 503: *status = CFW_HTTP_STATUS_SERVICE_UNAVAILABLE; return CFW_OK;
    case 504: *status = CFW_HTTP_STATUS_GATEWAY_TIMEOUT; return CFW_OK;
    default:
        return CFW_ERROR_INVALID_ARGUMENT;
    }
}

/* ------------------------------------------------------------------ */
/* Header block parsing                                                */
/* ------------------------------------------------------------------ */

static int cfw_http_is_tchar(unsigned char c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9')) {
        return 1;
    }
    return strchr("!#$%&'*+-.^_`|~", (int)c) != NULL;
}

static int cfw_http_is_ows(unsigned char c)
{
    return c == ' ' || c == '\t';
}

static cfw_status_t cfw_http_parse_header_line(const cfw_strview_t *line,
                                               cfw_http_header_t *header)
{
    size_t i;
    size_t colon = line->size;
    size_t name_end;
    size_t value_start;
    size_t value_end;

    for (i = 0; i < line->size; i++) {
        if (line->data[i] == ':') {
            colon = i;
            break;
        }
        if (!cfw_http_is_tchar((unsigned char)line->data[i])) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
    }
    if (colon == 0 || colon == line->size) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    name_end = colon;
    value_start = colon + 1;
    value_end = line->size;

    while (value_start < value_end &&
           cfw_http_is_ows((unsigned char)line->data[value_start])) {
        value_start++;
    }
    while (value_end > value_start &&
           cfw_http_is_ows((unsigned char)line->data[value_end - 1])) {
        value_end--;
    }

    header->name.data = line->data;
    header->name.size = name_end;
    header->value.data = line->data + value_start;
    header->value.size = value_end - value_start;
    return CFW_OK;
}

cfw_status_t cfw_http_headers_next(const cfw_strview_t *block,
                                   size_t *cursor,
                                   cfw_http_header_t *header, int *done)
{
    size_t pos;
    size_t line_end;
    cfw_strview_t line;
    cfw_status_t status;

    if (block == NULL || cursor == NULL || header == NULL || done == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (block->size != 0 && block->data == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (*cursor > block->size) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    *done = 0;
    pos = *cursor;

    if (pos >= block->size) {
        *done = 1;
        return CFW_OK;
    }

    /* An empty line terminates the header section. */
    if (pos + 1 < block->size && block->data[pos] == '\r' &&
        block->data[pos + 1] == '\n') {
        *done = 1;
        return CFW_OK;
    }

    /* Find the CRLF terminating this header line. Bare CR or LF inside the
     * block is malformed; obs-fold (leading SP/HT) is rejected. A line not
     * terminated by CRLF within the block is incomplete input. */
    line_end = block->size;
    {
        size_t i;
        int terminated = 0;
        for (i = pos; i < block->size; i++) {
            unsigned char c = (unsigned char)block->data[i];
            if (c == '\r') {
                if (i + 1 >= block->size || block->data[i + 1] != '\n') {
                    return CFW_ERROR_INVALID_ARGUMENT;
                }
                line_end = i;
                terminated = 1;
                break;
            }
            if (c == '\n') {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
        }
        if (!terminated) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
    }

    /* Reject obs-fold: a header line must not start with SP/HT. */
    if (cfw_http_is_ows((unsigned char)block->data[pos])) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    line.data = block->data + pos;
    line.size = line_end - pos;

    status = cfw_http_parse_header_line(&line, header);
    if (status != CFW_OK) {
        return status;
    }

    /* Advance past the CRLF. */
    if (line_end + 2 <= block->size) {
        *cursor = line_end + 2;
    } else {
        *cursor = line_end;
    }
    return CFW_OK;
}

cfw_status_t cfw_http_request_line_parse(const cfw_strview_t *line,
                                         cfw_http_request_line_t *request_line)
{
    size_t i;
    size_t sp1;
    size_t sp2;
    cfw_strview_t method_token;
    cfw_strview_t version_token;
    cfw_strview_t target_view;
    cfw_http_method_t method;
    cfw_status_t status;

    if (line == NULL || line->data == NULL || request_line == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    /* Exactly two single SP separators: METHOD SP TARGET SP VERSION */
    sp1 = line->size;
    sp2 = line->size;
    for (i = 0; i < line->size; i++) {
        if (line->data[i] == ' ') {
            if (sp1 == line->size) {
                sp1 = i;
            } else if (sp2 == line->size) {
                sp2 = i;
            } else {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
        } else if ((unsigned char)line->data[i] <= 0x20 ||
                   (unsigned char)line->data[i] == 0x7F) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
    }
    if (sp1 == line->size || sp2 == line->size || sp2 + 1 >= line->size ||
        sp1 == 0 || sp2 == sp1 + 1) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    method_token.data = line->data;
    method_token.size = sp1;
    target_view.data = line->data + sp1 + 1;
    target_view.size = sp2 - sp1 - 1;
    version_token.data = line->data + sp2 + 1;
    version_token.size = line->size - sp2 - 1;

    status = cfw_http_method_parse(&method_token, &method);
    if (status != CFW_OK || method == CFW_HTTP_METHOD_UNKNOWN) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    status = cfw_http_version_parse(&version_token,
                                    &request_line->version);
    if (status != CFW_OK) {
        return status;
    }

    request_line->method = method;
    request_line->target = target_view;
    return CFW_OK;
}
