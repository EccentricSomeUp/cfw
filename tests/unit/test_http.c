/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <string.h>

static cfw_strview_t cfw_test_view(const char *text)
{
    cfw_strview_t view;
    cfw_strview_init(&view, text, strlen(text));
    return view;
}

static int test_http_method_names_and_parse(void)
{
    cfw_strview_t token;
    cfw_http_method_t method;

    CFW_TEST_ASSERT_STR_EQ(cfw_http_method_name(CFW_HTTP_METHOD_GET), "GET");
    CFW_TEST_ASSERT_STR_EQ(cfw_http_method_name(CFW_HTTP_METHOD_DELETE),
                           "DELETE");
    CFW_TEST_ASSERT(cfw_http_method_name(CFW_HTTP_METHOD_UNKNOWN) == NULL);

    token = cfw_test_view("POST");
    CFW_TEST_ASSERT_EQ(cfw_http_method_parse(&token, &method), CFW_OK);
    CFW_TEST_ASSERT_EQ(method, CFW_HTTP_METHOD_POST);

    token = cfw_test_view("post"); /* methods are case-sensitive */
    CFW_TEST_ASSERT_EQ(cfw_http_method_parse(&token, &method), CFW_OK);
    CFW_TEST_ASSERT_EQ(method, CFW_HTTP_METHOD_UNKNOWN);

    token = cfw_test_view("FOO");
    CFW_TEST_ASSERT_EQ(cfw_http_method_parse(&token, &method), CFW_OK);
    CFW_TEST_ASSERT_EQ(method, CFW_HTTP_METHOD_UNKNOWN);

    CFW_TEST_ASSERT_EQ(cfw_http_method_parse(NULL, &method),
                       CFW_ERROR_INVALID_ARGUMENT);
    token = cfw_test_view("GET");
    CFW_TEST_ASSERT_EQ(cfw_http_method_parse(&token, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_http_version_parse_and_format(void)
{
    cfw_strview_t token;
    cfw_http_version_t version;
    char buffer[32];

    token = cfw_test_view("HTTP/1.1");
    CFW_TEST_ASSERT_EQ(cfw_http_version_parse(&token, &version), CFW_OK);
    CFW_TEST_ASSERT_EQ(version.major, 1);
    CFW_TEST_ASSERT_EQ(version.minor, 1);
    CFW_TEST_ASSERT_EQ(cfw_http_version_format(&version, buffer,
                                               sizeof(buffer)),
                       CFW_OK);
    CFW_TEST_ASSERT_STR_EQ(buffer, "HTTP/1.1");

    token = cfw_test_view("HTTP/2.0");
    CFW_TEST_ASSERT_EQ(cfw_http_version_parse(&token, &version), CFW_OK);
    CFW_TEST_ASSERT_EQ(version.major, 2);

    /* Malformed inputs must fail closed. */
    {
        static const char *bad[] = {
            "HTTP/1", "HTTP/1.", "HTTP/.1", "HTTP/1.1x", "http/1.1",
            "HTTP/01.1", "HTTP/1.01", "HTTP/999.1", "1.1", "",
        };
        size_t i;
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            token = cfw_test_view(bad[i]);
            CFW_TEST_ASSERT_EQ(cfw_http_version_parse(&token, &version),
                               CFW_ERROR_INVALID_ARGUMENT);
        }
    }

    CFW_TEST_ASSERT_EQ(cfw_http_version_format(NULL, buffer, sizeof(buffer)),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_http_version_format(&version, NULL, 4),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_http_version_format(&version, buffer, 4),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_http_status_reason_and_parse(void)
{
    cfw_http_status_t status;

    CFW_TEST_ASSERT_STR_EQ(cfw_http_status_reason(CFW_HTTP_STATUS_OK), "OK");
    CFW_TEST_ASSERT_STR_EQ(
        cfw_http_status_reason(CFW_HTTP_STATUS_NOT_FOUND), "Not Found");
    CFW_TEST_ASSERT(cfw_http_status_reason((cfw_http_status_t)299) == NULL);

    CFW_TEST_ASSERT_EQ(cfw_http_status_parse(200, &status), CFW_OK);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_OK);
    CFW_TEST_ASSERT_EQ(cfw_http_status_parse(404, &status), CFW_OK);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_NOT_FOUND);
    CFW_TEST_ASSERT_EQ(cfw_http_status_parse(418, &status),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_http_status_parse(200, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static cfw_strview_t cfw_test_view_n(const char *text, size_t size)
{
    cfw_strview_t view;
    cfw_strview_init(&view, text, size);
    return view;
}

static int test_http_headers_next_basic(void)
{
    static const char block[] =
        "Host: example.com\r\n"
        "Content-Type: text/html\r\n"
        "X-Empty:\r\n"
        "X-Spaces:   padded value  \r\n";
    cfw_strview_t view = cfw_test_view_n(block, sizeof(block) - 1);
    cfw_http_header_t header;
    size_t cursor = 0;
    int done = 0;

    CFW_TEST_ASSERT_EQ(cfw_http_headers_next(&view, &cursor, &header, &done),
                       CFW_OK);
    CFW_TEST_ASSERT_FALSE(done);
    CFW_TEST_ASSERT_EQ(header.name.size, 4);
    CFW_TEST_ASSERT(memcmp(header.name.data, "Host", 4) == 0);
    CFW_TEST_ASSERT_EQ(header.value.size, 11);
    CFW_TEST_ASSERT(memcmp(header.value.data, "example.com", 11) == 0);

    CFW_TEST_ASSERT_EQ(cfw_http_headers_next(&view, &cursor, &header, &done),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(header.name.size, 12);
    CFW_TEST_ASSERT(memcmp(header.name.data, "Content-Type", 12) == 0);

    CFW_TEST_ASSERT_EQ(cfw_http_headers_next(&view, &cursor, &header, &done),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(header.name.size, 7);
    CFW_TEST_ASSERT_EQ(header.value.size, 0);

    CFW_TEST_ASSERT_EQ(cfw_http_headers_next(&view, &cursor, &header, &done),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(header.value.size, 12);
    CFW_TEST_ASSERT(memcmp(header.value.data, "padded value", 12) == 0);

    CFW_TEST_ASSERT_EQ(cfw_http_headers_next(&view, &cursor, &header, &done),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(done);
    CFW_TEST_ASSERT_EQ(cursor, sizeof(block) - 1);
    return 0;
}

static int test_http_headers_next_empty_block(void)
{
    cfw_strview_t view = cfw_test_view_n("", 0);
    cfw_http_header_t header;
    size_t cursor = 0;
    int done = 0;

    CFW_TEST_ASSERT_EQ(cfw_http_headers_next(&view, &cursor, &header, &done),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(done);

    /* A block that is only the header terminator also reports done. */
    view = cfw_test_view("\r\n");
    cursor = 0;
    done = 0;
    CFW_TEST_ASSERT_EQ(cfw_http_headers_next(&view, &cursor, &header, &done),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(done);
    return 0;
}

static int test_http_headers_next_rejects_malformed(void)
{
    static const char *bad[] = {
        "NoColon\r\n",
        "Bad Name: v\r\n",
        "Name:value\nNext: v\r\n",   /* bare LF */
        "Name: v\rX\r\n",            /* bare CR */
        " Name: v\r\n",              /* obs-fold leading space */
        "\tName: v\r\n",             /* obs-fold leading tab */
        "Name: v",                   /* unterminated line */
        ": v\r\n",                   /* empty name */
        "Name\x01: v\r\n",           /* invalid tchar */
    };
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        cfw_strview_t view = cfw_test_view(bad[i]);
        cfw_http_header_t header;
        size_t cursor = 0;
        int done = 0;
        cfw_status_t status =
            cfw_http_headers_next(&view, &cursor, &header, &done);
        CFW_TEST_ASSERT_EQ(status, CFW_ERROR_INVALID_ARGUMENT);
    }
    return 0;
}

static int test_http_headers_next_invalid_args(void)
{
    cfw_strview_t view = cfw_test_view("A: b\r\n");
    cfw_http_header_t header;
    size_t cursor = 0;
    int done = 0;

    CFW_TEST_ASSERT_EQ(cfw_http_headers_next(NULL, &cursor, &header, &done),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_http_headers_next(&view, NULL, &header, &done),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_http_headers_next(&view, &cursor, NULL, &done),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_http_headers_next(&view, &cursor, &header, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);

    cursor = view.size + 1;
    CFW_TEST_ASSERT_EQ(cfw_http_headers_next(&view, &cursor, &header, &done),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_http_request_line_parse(void)
{
    cfw_http_request_line_t req;
    cfw_strview_t line;

    line = cfw_test_view("GET /index.html HTTP/1.1");
    CFW_TEST_ASSERT_EQ(cfw_http_request_line_parse(&line, &req), CFW_OK);
    CFW_TEST_ASSERT_EQ(req.method, CFW_HTTP_METHOD_GET);
    CFW_TEST_ASSERT_EQ(req.target.size, 11);
    CFW_TEST_ASSERT(memcmp(req.target.data, "/index.html", 11) == 0);
    CFW_TEST_ASSERT_EQ(req.version.major, 1);
    CFW_TEST_ASSERT_EQ(req.version.minor, 1);

    line = cfw_test_view("POST http://example.com/a?b=c HTTP/1.0");
    CFW_TEST_ASSERT_EQ(cfw_http_request_line_parse(&line, &req), CFW_OK);
    CFW_TEST_ASSERT_EQ(req.method, CFW_HTTP_METHOD_POST);
    CFW_TEST_ASSERT_EQ(req.version.minor, 0);

    line = cfw_test_view("OPTIONS * HTTP/1.1");
    CFW_TEST_ASSERT_EQ(cfw_http_request_line_parse(&line, &req), CFW_OK);
    CFW_TEST_ASSERT_EQ(req.target.size, 1);

    line = cfw_test_view("CONNECT example.com:443 HTTP/1.1");
    CFW_TEST_ASSERT_EQ(cfw_http_request_line_parse(&line, &req), CFW_OK);
    CFW_TEST_ASSERT_EQ(req.method, CFW_HTTP_METHOD_CONNECT);

    /* Percent-encoded and UTF-8 targets are permitted. */
    line = cfw_test_view("GET /%20caf\xC3\xA9 HTTP/1.1");
    CFW_TEST_ASSERT_EQ(cfw_http_request_line_parse(&line, &req), CFW_OK);

    /* Malformed request lines fail closed. */
    {
        static const char *bad[] = {
            "",
            "GET",
            "GET /",
            "GET  / HTTP/1.1",      /* double space */
            "GET /  HTTP/1.1",      /* double space */
            " GET / HTTP/1.1",      /* leading space */
            "GET / HTTP/1.1 ",      /* trailing space */
            "GET / HTTP/1.1 extra", /* extra token */
            "get / HTTP/1.1",       /* lowercase method */
            "FOO / HTTP/1.1",       /* unknown method */
            "GET /path\tHTTP/1.1",  /* tab instead of space splits wrong */
            "GET /\x01 HTTP/1.1",   /* control char in target */
        };
        size_t i;
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            line = cfw_test_view(bad[i]);
            CFW_TEST_ASSERT_EQ(cfw_http_request_line_parse(&line, &req),
                               CFW_ERROR_INVALID_ARGUMENT);
        }
    }

    CFW_TEST_ASSERT_EQ(cfw_http_request_line_parse(NULL, &req),
                       CFW_ERROR_INVALID_ARGUMENT);
    line = cfw_test_view("GET / HTTP/1.1");
    CFW_TEST_ASSERT_EQ(cfw_http_request_line_parse(&line, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_http_request_head_parse(void)
{
    cfw_http_request_head_t head;
    cfw_http_header_t header;
    size_t cursor;
    int done;

    /* Minimal head: no headers. */
    {
        cfw_strview_t view = cfw_test_view("GET / HTTP/1.1\r\n\r\n");
        CFW_TEST_ASSERT_EQ(cfw_http_request_head_parse(&view, &head), CFW_OK);
        CFW_TEST_ASSERT_EQ(head.request_line.method, CFW_HTTP_METHOD_GET);
        cursor = 0;
        CFW_TEST_ASSERT_EQ(cfw_http_headers_next(&head.headers, &cursor,
                                                 &header, &done),
                           CFW_OK);
        CFW_TEST_ASSERT_TRUE(done);
    }

    /* Head with headers. */
    {
        cfw_strview_t view = cfw_test_view(
            "POST /submit?a=1 HTTP/1.1\r\n"
            "Host: example.com\r\n"
            "Content-Length: 4\r\n"
            "\r\n");
        int seen_host = 0;
        int seen_length = 0;

        CFW_TEST_ASSERT_EQ(cfw_http_request_head_parse(&view, &head), CFW_OK);
        CFW_TEST_ASSERT_EQ(head.request_line.method, CFW_HTTP_METHOD_POST);
        CFW_TEST_ASSERT_EQ(head.request_line.target.size, 11);

        cursor = 0;
        for (;;) {
            CFW_TEST_ASSERT_EQ(cfw_http_headers_next(&head.headers, &cursor,
                                                     &header, &done),
                               CFW_OK);
            if (done) {
                break;
            }
            if (header.name.size == 4 &&
                memcmp(header.name.data, "Host", 4) == 0) {
                seen_host = 1;
            }
            if (header.name.size == 14 &&
                memcmp(header.name.data, "Content-Length", 14) == 0) {
                seen_length = 1;
                CFW_TEST_ASSERT_EQ(header.value.size, 1);
                CFW_TEST_ASSERT_EQ(header.value.data[0], '4');
            }
        }
        CFW_TEST_ASSERT_TRUE(seen_host);
        CFW_TEST_ASSERT_TRUE(seen_length);
    }

    /* Malformed heads fail closed. */
    {
        static const char *bad[] = {
            "",
            "GET / HTTP/1.1",
            "GET / HTTP/1.1\r\n",          /* missing terminator */
            "GET / HTTP/1.1\n\n",          /* LF only */
            "GET / HTTP/1.1\r\nBad Header\r\n\r\n",
            "GET / HTTP/1.1\r\n\r\nGARBAGE",
            "GET / HTTP/1.1\r\nA: b\r\n\r\nC: d\r\n\r\n",
            "BAD / HTTP/1.1\r\n\r\n",
        };
        size_t i;
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            cfw_strview_t view = cfw_test_view(bad[i]);
            CFW_TEST_ASSERT_EQ(cfw_http_request_head_parse(&view, &head),
                               CFW_ERROR_INVALID_ARGUMENT);
        }
    }

    CFW_TEST_ASSERT_EQ(cfw_http_request_head_parse(NULL, &head),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

const cfw_test_case cfw_test_http_cases[] = {
    {"http_method_names_and_parse", test_http_method_names_and_parse},
    {"http_version_parse_and_format", test_http_version_parse_and_format},
    {"http_status_reason_and_parse", test_http_status_reason_and_parse},
    {"http_headers_next_basic", test_http_headers_next_basic},
    {"http_headers_next_empty_block", test_http_headers_next_empty_block},
    {"http_headers_next_rejects_malformed",
     test_http_headers_next_rejects_malformed},
    {"http_headers_next_invalid_args", test_http_headers_next_invalid_args},
    {"http_request_line_parse", test_http_request_line_parse},
    {"http_request_head_parse", test_http_request_head_parse},
    {NULL, NULL},
};
