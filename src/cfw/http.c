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

static int cfw_http_field_value_valid(const char *data, size_t size)
{
    size_t i;
    for (i = 0; i < size; i++) {
        unsigned char c = (unsigned char)data[i];
        if ((c < 0x20 && c != '\t') || c == 0x7F) {
            return 0;
        }
    }
    return 1;
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

    /* Field values allow VCHAR, obs-text, SP, and HTAB; other control
     * characters (including a bare CR that survived line splitting) are
     * rejected. */
    if (!cfw_http_field_value_valid(line->data + value_start,
                                    value_end - value_start)) {
        return CFW_ERROR_INVALID_ARGUMENT;
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

cfw_status_t cfw_http_request_head_parse(const cfw_strview_t *head,
                                         cfw_http_request_head_t *request_head)
{
    size_t i;
    size_t eol;
    cfw_strview_t request_line_view;
    cfw_strview_t headers_view;
    size_t cursor;
    cfw_http_header_t header;
    int done;
    cfw_status_t status;

    if (head == NULL || head->data == NULL || request_head == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (head->size != 0 && head->data == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    /* Locate the CRLF ending the request line. */
    eol = head->size;
    for (i = 0; i < head->size; i++) {
        if (head->data[i] == '\r') {
            if (i + 1 >= head->size || head->data[i + 1] != '\n') {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
            eol = i;
            break;
        }
        if (head->data[i] == '\n') {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
    }
    if (eol == head->size) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (eol == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    request_line_view.data = head->data;
    request_line_view.size = eol;

    status = cfw_http_request_line_parse(&request_line_view,
                                         &request_head->request_line);
    if (status != CFW_OK) {
        return status;
    }

    /* Everything after the request-line CRLF is the header section; it must
     * be at least the two-byte empty-line terminator. */
    if (eol + 2 >= head->size) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    headers_view.data = head->data + eol + 2;
    headers_view.size = head->size - eol - 2;

    /* Walk every header line; the iterator stops at the empty line, after
     * which only the final CRLF may remain. */
    cursor = 0;
    for (;;) {
        status = cfw_http_headers_next(&headers_view, &cursor, &header,
                                       &done);
        if (status != CFW_OK) {
            return status;
        }
        if (done) {
            break;
        }
    }

    if (cursor + 2 != headers_view.size ||
        headers_view.data[cursor] != '\r' ||
        headers_view.data[cursor + 1] != '\n') {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    request_head->headers = headers_view;
    return CFW_OK;
}

static cfw_status_t cfw_http_parse_content_length(const cfw_strview_t *value,
                                                  uint64_t *out)
{
    uint64_t result = 0;
    size_t i;

    if (value->size == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < value->size; i++) {
        unsigned char c = (unsigned char)value->data[i];
        uint64_t digit;
        if (c < '0' || c > '9') {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        digit = (uint64_t)(c - '0');
        if (result > (UINT64_MAX - digit) / 10) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        result = result * 10 + digit;
    }
    *out = result;
    return CFW_OK;
}

cfw_status_t cfw_http_request_framing(
    const cfw_http_request_head_t *request_head,
    cfw_http_message_framing_t *framing)
{
    cfw_strview_t headers_block;
    cfw_strview_t content_length_name;
    cfw_strview_t transfer_encoding_name;
    cfw_strview_t chunked_token;
    size_t cursor;
    cfw_http_header_t header;
    int done;
    int have_length = 0;
    int have_chunked = 0;
    uint64_t content_length = 0;
    uint64_t parsed_length;
    int equal;
    cfw_status_t status;

    if (request_head == NULL || framing == NULL ||
        request_head->headers.data == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    framing->framing = CFW_HTTP_BODY_NONE;
    framing->content_length = 0;

    headers_block = request_head->headers;
    cursor = 0;
    cfw_strview_from_cstr(&content_length_name, "content-length");
    cfw_strview_from_cstr(&transfer_encoding_name, "transfer-encoding");
    cfw_strview_from_cstr(&chunked_token, "chunked");

    for (;;) {
        status = cfw_http_headers_next(&headers_block, &cursor, &header,
                                       &done);
        if (status != CFW_OK) {
            return status;
        }
        if (done) {
            break;
        }

        if (cfw_strview_equal_nocase(&header.name, &content_length_name,
                                     &equal) != CFW_OK) {
            return CFW_ERROR_INTERNAL;
        }
        if (equal) {
            status = cfw_http_parse_content_length(&header.value,
                                                   &parsed_length);
            if (status != CFW_OK) {
                return status;
            }
            if (have_length && parsed_length != content_length) {
                /* Conflicting duplicate Content-Length: smuggling vector. */
                return CFW_ERROR_INVALID_ARGUMENT;
            }
            have_length = 1;
            content_length = parsed_length;
            continue;
        }

        if (cfw_strview_equal_nocase(&header.name, &transfer_encoding_name,
                                     &equal) != CFW_OK) {
            return CFW_ERROR_INTERNAL;
        }
        if (equal) {
            if (have_chunked) {
                /* Multiple Transfer-Encoding headers are ambiguous. */
                return CFW_ERROR_INVALID_ARGUMENT;
            }
            have_chunked = 1;
            if (cfw_strview_equal_nocase(&header.value, &chunked_token,
                                         &equal) != CFW_OK) {
                return CFW_ERROR_INTERNAL;
            }
            if (!equal) {
                /* Only a sole "chunked" coding is supported. */
                return CFW_ERROR_INVALID_ARGUMENT;
            }
            continue;
        }
    }

    if (have_length && have_chunked) {
        /* Content-Length + Transfer-Encoding is a classic smuggling pair. */
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    if (have_chunked) {
        framing->framing = CFW_HTTP_BODY_CHUNKED;
    } else if (have_length) {
        framing->framing = CFW_HTTP_BODY_CONTENT_LENGTH;
        framing->content_length = content_length;
    }
    return CFW_OK;
}

/* ------------------------------------------------------------------ */
/* Chunked transfer decoder                                            */
/* ------------------------------------------------------------------ */

void cfw_http_chunked_decoder_init(cfw_http_chunked_decoder_t *decoder)
{
    if (decoder == NULL) {
        return;
    }
    decoder->state = CFW_HTTP_CHUNKED_STATE_SIZE;
    decoder->remaining = 0;
    decoder->cr_pending = 0;
    decoder->done = 0;
    decoder->line_len = 0;
}

/* Accumulates input into the line buffer until a CRLF-terminated line is
 * complete. Returns 1 when a line is ready (content in line_buf, length in
 * line_len, CRLF consumed), 0 when more input is needed, or a negative
 * cfw_status_t-style error via *status. Advances *pos past consumed bytes. */
static int cfw_http_chunked_read_line(cfw_http_chunked_decoder_t *decoder,
                                      const cfw_span_t *input, size_t *pos,
                                      cfw_status_t *status)
{
    size_t avail = input->size - *pos;
    size_t i;
    size_t nl;

    /* Find the next LF within the remaining input. */
    nl = avail;
    for (i = 0; i < avail; i++) {
        if (input->data[*pos + i] == '\n') {
            nl = i;
            break;
        }
    }
    {
        if (nl == avail) {
            /* No complete line; buffer what we have. */
            if (avail > CFW_HTTP_CHUNKED_MAX_LINE - decoder->line_len) {
                *status = CFW_ERROR_INVALID_ARGUMENT;
                return -1;
            }
            memcpy(decoder->line_buf + decoder->line_len, input->data + *pos,
                   avail);
            decoder->line_len += avail;
            *pos += avail;
            return 0;
        }

        /* nl is the offset of '\n' within the remaining input. The byte
         * before it must be '\r', which may sit in the buffer or input. */
        if (nl == 0) {
            /* '\n' is the first unconsumed byte: the CR must be the last
             * buffered byte. */
            if (decoder->line_len == 0 ||
                decoder->line_buf[decoder->line_len - 1] != '\r') {
                *status = CFW_ERROR_INVALID_ARGUMENT;
                return -1;
            }
            decoder->line_len--;
        } else {
            if (input->data[*pos + nl - 1] != '\r') {
                *status = CFW_ERROR_INVALID_ARGUMENT;
                return -1;
            }
            if (decoder->line_len + nl - 1 >
                CFW_HTTP_CHUNKED_MAX_LINE) {
                *status = CFW_ERROR_INVALID_ARGUMENT;
                return -1;
            }
            memcpy(decoder->line_buf + decoder->line_len,
                   input->data + *pos, nl - 1);
            decoder->line_len += nl - 1;
        }
        *pos += nl + 1;
        return 1;
    }
}

static int cfw_http_hex_digit_value(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static cfw_status_t cfw_http_chunked_parse_size(cfw_http_chunked_decoder_t *d,
                                                uint64_t *out)
{
    size_t i;
    size_t hex_len = d->line_len;
    uint64_t value = 0;
    int have_hex = 0;

    /* Split off optional ";extensions"; the hex part must be nonempty. */
    for (i = 0; i < d->line_len; i++) {
        if (d->line_buf[i] == ';') {
            hex_len = i;
            break;
        }
    }
    if (hex_len == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    for (i = 0; i < hex_len; i++) {
        int digit = cfw_http_hex_digit_value((unsigned char)d->line_buf[i]);
        if (digit < 0) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        have_hex = 1;
        if (value > (UINT64_MAX - (uint64_t)digit) / 16) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        value = value * 16 + (uint64_t)digit;
    }
    if (!have_hex) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    /* Extension bytes must be printable ASCII and nonempty after ';'. */
    if (hex_len < d->line_len) {
        if (hex_len + 1 == d->line_len) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        for (i = hex_len + 1; i < d->line_len; i++) {
            unsigned char c = (unsigned char)d->line_buf[i];
            if (c < 0x20 || c > 0x7E) {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
        }
    }

    *out = value;
    return CFW_OK;
}

cfw_status_t cfw_http_chunked_decode(cfw_http_chunked_decoder_t *decoder,
                                     const cfw_span_t *input,
                                     cfw_span_t *output, size_t *consumed,
                                     int *finished)
{
    size_t pos = 0;
    const uint8_t *output_data = NULL;
    size_t output_size = 0;
    cfw_status_t status;

    if (decoder == NULL || input == NULL || output == NULL ||
        consumed == NULL || finished == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if ((input->size != 0 && input->data == NULL) || decoder->done) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    output->data = NULL;
    output->size = 0;
    *consumed = 0;
    *finished = 0;

    while (pos < input->size && !decoder->done) {
        switch (decoder->state) {
        case CFW_HTTP_CHUNKED_STATE_SIZE:
        case CFW_HTTP_CHUNKED_STATE_TRAILER: {
            int is_trailer =
                decoder->state == CFW_HTTP_CHUNKED_STATE_TRAILER;
            int got;
            got = cfw_http_chunked_read_line(decoder, input, &pos, &status);
            if (got < 0) {
                return status;
            }
            if (got == 0) {
                goto out;
            }
            if (is_trailer) {
                if (decoder->line_len == 0) {
                    decoder->state = CFW_HTTP_CHUNKED_STATE_DONE;
                    decoder->done = 1;
                    *finished = 1;
                    goto out;
                }
                {
                    cfw_strview_t line;
                    cfw_http_header_t trailer;
                    line.data = decoder->line_buf;
                    line.size = decoder->line_len;
                    status = cfw_http_parse_header_line(&line, &trailer);
                    decoder->line_len = 0;
                    if (status != CFW_OK) {
                        return status;
                    }
                }
                break;
            }
            {
                uint64_t size;
                status = cfw_http_chunked_parse_size(decoder, &size);
                decoder->line_len = 0;
                if (status != CFW_OK) {
                    return status;
                }
                if (size == 0) {
                    decoder->state = CFW_HTTP_CHUNKED_STATE_TRAILER;
                } else {
                    decoder->remaining = size;
                    decoder->state = CFW_HTTP_CHUNKED_STATE_DATA;
                }
            }
            break;
        }
        case CFW_HTTP_CHUNKED_STATE_DATA: {
            size_t avail = input->size - pos;
            size_t n;
            if (output_data != NULL) {
                /* A second contiguous body span would be needed; leave the
                 * rest for the next call. */
                goto out;
            }
            n = avail < decoder->remaining
                    ? avail
                    : (size_t)decoder->remaining;
            output_data = input->data + pos;
            output_size = n;
            pos += n;
            decoder->remaining -= (uint64_t)n;
            if (decoder->remaining == 0) {
                decoder->state = CFW_HTTP_CHUNKED_STATE_DATA_CRLF;
            }
            break;
        }
        case CFW_HTTP_CHUNKED_STATE_DATA_CRLF:
            if (decoder->cr_pending) {
                if (input->data[pos] != '\n') {
                    return CFW_ERROR_INVALID_ARGUMENT;
                }
                decoder->cr_pending = 0;
                pos++;
                decoder->state = CFW_HTTP_CHUNKED_STATE_SIZE;
                break;
            }
            if (pos + 1 >= input->size) {
                /* Only one byte left; it must be CR, LF comes next call. */
                if (input->data[pos] != '\r') {
                    return CFW_ERROR_INVALID_ARGUMENT;
                }
                decoder->cr_pending = 1;
                pos++;
                goto out;
            }
            if (input->data[pos] != '\r' || input->data[pos + 1] != '\n') {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
            pos += 2;
            decoder->state = CFW_HTTP_CHUNKED_STATE_SIZE;
            break;
        case CFW_HTTP_CHUNKED_STATE_DONE:
        default:
            goto out;
        }
    }

out:
    output->data = output_data;
    output->size = output_size;
    *consumed = pos;
    return CFW_OK;
}

/* ------------------------------------------------------------------ */
/* Response serialization                                              */
/* ------------------------------------------------------------------ */

cfw_status_t cfw_http_status_line_format(const cfw_http_version_t *version,
                                         cfw_http_status_t status,
                                         char *buffer, size_t capacity,
                                         size_t *written)
{
    const char *reason;
    int n;

    if (version == NULL || buffer == NULL || written == NULL ||
        version->major > 9 || version->minor > 9) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    reason = cfw_http_status_reason(status);
    if (reason == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    n = snprintf(buffer, capacity, "HTTP/%u.%u %u %s\r\n",
                 (unsigned)version->major, (unsigned)version->minor,
                 (unsigned)status, reason);
    if (n < 0 || (size_t)n >= capacity) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    *written = (size_t)n;
    return CFW_OK;
}

static int cfw_http_field_name_valid(const cfw_strview_t *name)
{
    size_t i;
    if (name->size == 0) {
        return 0;
    }
    for (i = 0; i < name->size; i++) {
        if (!cfw_http_is_tchar((unsigned char)name->data[i])) {
            return 0;
        }
    }
    return 1;
}

cfw_status_t cfw_http_header_line_format(const cfw_strview_t *name,
                                         const cfw_strview_t *value,
                                         char *buffer, size_t capacity,
                                         size_t *written)
{
    size_t need;
    size_t off = 0;

    if (name == NULL || value == NULL || buffer == NULL || written == NULL ||
        capacity == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if ((name->size != 0 && name->data == NULL) ||
        (value->size != 0 && value->data == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (!cfw_http_field_name_valid(name) ||
        !cfw_http_field_value_valid(value->data, value->size)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    /* "Name: value\r\n" — fixed overhead is 4 bytes (": " + CRLF). */
    if (name->size > SIZE_MAX - value->size - 4) {
        return CFW_ERROR_OVERFLOW;
    }
    need = name->size + value->size + 4;
    if (need > capacity) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    memcpy(buffer + off, name->data, name->size);
    off += name->size;
    buffer[off++] = ':';
    buffer[off++] = ' ';
    if (value->size != 0) {
        memcpy(buffer + off, value->data, value->size);
        off += value->size;
    }
    buffer[off++] = '\r';
    buffer[off++] = '\n';

    *written = off;
    return CFW_OK;
}

cfw_status_t cfw_http_response_head_format(
    const cfw_http_version_t *version, cfw_http_status_t status,
    const cfw_http_header_t *headers, size_t header_count,
    char *buffer, size_t capacity, size_t *written)
{
    size_t offset = 0;
    size_t n;
    size_t i;
    cfw_status_t st;

    if (buffer == NULL || written == NULL ||
        (header_count != 0 && headers == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    st = cfw_http_status_line_format(version, status, buffer, capacity,
                                     &n);
    if (st != CFW_OK) {
        return st;
    }
    offset += n;

    for (i = 0; i < header_count; i++) {
        st = cfw_http_header_line_format(&headers[i].name,
                                         &headers[i].value,
                                         buffer + offset,
                                         capacity - offset, &n);
        if (st != CFW_OK) {
            return st;
        }
        offset += n;
    }

    if (capacity - offset < 2) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    buffer[offset++] = '\r';
    buffer[offset++] = '\n';

    *written = offset;
    return CFW_OK;
}

int cfw_http_response_has_body(cfw_http_method_t method,
                               cfw_http_status_t status)
{
    if (method == CFW_HTTP_METHOD_HEAD) {
        return 0;
    }
    if (status >= 100 && status <= 199) {
        return 0;
    }
    if (status == CFW_HTTP_STATUS_NO_CONTENT ||
        status == CFW_HTTP_STATUS_NOT_MODIFIED) {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Incremental head reader                                             */
/* ------------------------------------------------------------------ */

cfw_status_t cfw_http_head_reader_init(cfw_http_head_reader_t *reader,
                                       size_t max_size)
{
    cfw_status_t status;

    if (reader == NULL || max_size == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    status = cfw_buffer_init(&reader->buffer);
    if (status != CFW_OK) {
        return status;
    }
    reader->max_size = max_size;
    reader->complete = 0;
    reader->head_size = 0;
    return CFW_OK;
}

void cfw_http_head_reader_free(cfw_http_head_reader_t *reader)
{
    if (reader == NULL) {
        return;
    }
    cfw_buffer_free(&reader->buffer);
    reader->buffer.data = NULL;
    reader->buffer.size = 0;
    reader->buffer.capacity = 0;
    reader->complete = 0;
    reader->head_size = 0;
}

static int cfw_http_head_find_end(const cfw_buffer_t *buffer, size_t start,
                                  size_t *end_out)
{
    size_t i;
    if (buffer->size < 4 || start + 4 > buffer->size) {
        return 0;
    }
    for (i = start; i + 3 < buffer->size; i++) {
        if (buffer->data[i] == '\r' && buffer->data[i + 1] == '\n' &&
            buffer->data[i + 2] == '\r' && buffer->data[i + 3] == '\n') {
            *end_out = i + 4;
            return 1;
        }
    }
    return 0;
}

cfw_status_t cfw_http_head_reader_feed(cfw_http_head_reader_t *reader,
                                       const cfw_span_t *input,
                                       size_t *consumed)
{
    size_t old_size;
    size_t scan_start;
    size_t append_len;
    size_t end;
    size_t extra;
    cfw_status_t status;

    if (reader == NULL || consumed == NULL ||
        (input->size != 0 && input->data == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    *consumed = 0;

    if (reader->complete) {
        return CFW_OK;
    }

    old_size = reader->buffer.size;
    if (old_size >= reader->max_size) {
        /* A head of exactly max_size bytes without a terminator can never
         * complete. */
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    /* Only the bytes needed to reach max_size are appended; anything past
     * the limit cannot belong to a valid head. */
    append_len = input->size;
    if (append_len > reader->max_size - old_size) {
        append_len = reader->max_size - old_size;
    }
    status = cfw_buffer_append(&reader->buffer, input->data, append_len);
    if (status != CFW_OK) {
        return status;
    }

    /* A terminator not seen before must start within the last 3 bytes of
     * the previous buffer content. */
    scan_start = old_size > 3 ? old_size - 3 : 0;
    if (cfw_http_head_find_end(&reader->buffer, scan_start, &end)) {
        /* Drop bytes past the terminator; they belong to the caller. */
        extra = reader->buffer.size - end;
        reader->buffer.size = end;
        reader->complete = 1;
        reader->head_size = end;
        *consumed = append_len - extra;
        return CFW_OK;
    }

    *consumed = append_len;
    return CFW_OK;
}

void cfw_http_head_reader_reset(cfw_http_head_reader_t *reader)
{
    if (reader == NULL) {
        return;
    }
    cfw_buffer_clear(&reader->buffer);
    reader->complete = 0;
    reader->head_size = 0;
}

int cfw_http_head_reader_is_complete(const cfw_http_head_reader_t *reader)
{
    return reader != NULL && reader->complete;
}

cfw_status_t cfw_http_head_reader_head(const cfw_http_head_reader_t *reader,
                                       cfw_strview_t *head)
{
    if (reader == NULL || head == NULL || !reader->complete) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    head->data = (const char *)reader->buffer.data;
    head->size = reader->head_size;
    return CFW_OK;
}

cfw_status_t cfw_http_target_path(const cfw_strview_t *target,
                                  cfw_strview_t *path)
{
    size_t start;
    size_t end;
    size_t i;

    if (target == NULL || path == NULL || target->data == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (target->size == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    if (target->data[0] == '/') {
        /* origin-form */
        start = 0;
    } else {
        /* absolute-form: scheme "://" authority path. Anything else
         * (authority-form, asterisk-form) has no path. */
        size_t scheme_end = target->size;
        for (i = 0; i + 2 < target->size; i++) {
            if (target->data[i] == ':' && target->data[i + 1] == '/' &&
                target->data[i + 2] == '/') {
                scheme_end = i + 3;
                break;
            }
            if (target->data[i] == '/' || target->data[i] == '?') {
                break;
            }
        }
        if (scheme_end == target->size) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        start = scheme_end;
        while (start < target->size && target->data[start] != '/' &&
               target->data[start] != '?') {
            start++;
        }
        if (start == target->size || target->data[start] == '?') {
            /* Absolute-form without a path component has effective path
             * "/", but no '/' byte exists to reference; report invalid
             * and let the caller substitute the root path. */
            return CFW_ERROR_INVALID_ARGUMENT;
        }
    }

    /* Trim at '?' — the query is not part of the path. */
    end = start;
    while (end < target->size && target->data[end] != '?') {
        end++;
    }
    path->data = target->data + start;
    path->size = end - start;
    return CFW_OK;
}

cfw_status_t cfw_http_date_format(int64_t unix_seconds, char *out,
                                  size_t capacity)
{
    static const char *const weekday_names[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
    };
    static const char *const month_names[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    int64_t days;
    int64_t secs_of_day;
    int64_t z;
    int64_t era;
    int64_t doe;
    int64_t yoe;
    int64_t year;
    int64_t doy;
    int64_t mp;
    int64_t day;
    int64_t month;
    int64_t hour;
    int64_t minute;
    int64_t second;
    int64_t weekday;
    int written;

    if (out == NULL || capacity < 30) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    /* Restrict to years 1970..9999 (IMF-fixdate four-digit year). */
    if (unix_seconds < 0 || unix_seconds > INT64_C(253402300799)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    days = unix_seconds / 86400;
    secs_of_day = unix_seconds % 86400;

    /* Civil-from-days (Howard Hinnant's algorithm); z is days since
     * 1970-01-01 shifted to the civil epoch 0000-03-01. */
    z = days + 719468;
    era = z / 146097;
    doe = z - era * 146097; /* [0, 146096] */
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    year = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp + (mp < 10 ? 3 : -9);
    year += month <= 2;
    /* weekday: 1970-01-01 was Thursday (4 = Thu in Sun=0 indexing). */
    weekday = (days + 4) % 7;

    hour = secs_of_day / 3600;
    minute = (secs_of_day % 3600) / 60;
    second = secs_of_day % 60;

    written = snprintf(out, capacity,
                       "%s, %02lld %s %04lld %02lld:%02lld:%02lld GMT",
                       weekday_names[weekday],
                       (long long)day, month_names[month - 1],
                       (long long)year, (long long)hour,
                       (long long)minute, (long long)second);
    if (written != 29) {
        return CFW_ERROR_UNAVAILABLE;
    }
    return CFW_OK;
}
