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

static int test_http_request_framing(void)
{
    static const struct {
        const char *head;
        cfw_http_body_framing_t framing;
        uint64_t length;
    } valid[] = {
        {"GET / HTTP/1.1\r\n\r\n", CFW_HTTP_BODY_NONE, 0},
        {"POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n",
         CFW_HTTP_BODY_CONTENT_LENGTH, 0},
        {"POST / HTTP/1.1\r\nContent-Length: 42\r\n\r\n",
         CFW_HTTP_BODY_CONTENT_LENGTH, 42},
        {"POST / HTTP/1.1\r\ncontent-length: 0042\r\n\r\n",
         CFW_HTTP_BODY_CONTENT_LENGTH, 42},
        {"POST / HTTP/1.1\r\nContent-Length: 7\r\nContent-Length: 7\r\n\r\n",
         CFW_HTTP_BODY_CONTENT_LENGTH, 7},
        {"POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
         CFW_HTTP_BODY_CHUNKED, 0},
        {"POST / HTTP/1.1\r\nTRANSFER-ENCODING: CHUNKED\r\n\r\n",
         CFW_HTTP_BODY_CHUNKED, 0},
        {"POST / HTTP/1.1\r\nX-Other: v\r\nContent-Length: 18446744073709551615\r\n\r\n",
         CFW_HTTP_BODY_CONTENT_LENGTH, UINT64_MAX},
    };
    static const char *invalid[] = {
        /* Conflicting or malformed Content-Length. */
        "POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 1x\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length:\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 18446744073709551616\r\n\r\n",
        /* Smuggling pair. */
        "POST / HTTP/1.1\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n",
        /* Unsupported or ambiguous transfer codings. */
        "POST / HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked, gzip\r\n\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n",
    };
    cfw_http_request_head_t head;
    cfw_http_message_framing_t framing;
    size_t i;

    for (i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        cfw_strview_t view = cfw_test_view(valid[i].head);
        CFW_TEST_ASSERT_EQ(cfw_http_request_head_parse(&view, &head), CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_http_request_framing(&head, &framing), CFW_OK);
        CFW_TEST_ASSERT_EQ((int)framing.framing, (int)valid[i].framing);
        CFW_TEST_ASSERT_EQ(framing.content_length, valid[i].length);
    }

    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        cfw_strview_t view = cfw_test_view(invalid[i]);
        CFW_TEST_ASSERT_EQ(cfw_http_request_head_parse(&view, &head), CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_http_request_framing(&head, &framing),
                           CFW_ERROR_INVALID_ARGUMENT);
    }

    CFW_TEST_ASSERT_EQ(cfw_http_request_framing(NULL, &framing),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

/* Feeds a complete chunked body in pieces of `piece` bytes, collecting all
 * decoded output. Returns finished status via *finished_out. */
static cfw_status_t cfw_test_chunked_feed(const char *body, size_t body_len,
                                          size_t piece, char *collected,
                                          size_t *collected_len, int *finished)
{
    cfw_http_chunked_decoder_t decoder;
    size_t offset = 0;
    cfw_status_t status = CFW_OK;

    *collected_len = 0;
    *finished = 0;
    cfw_http_chunked_decoder_init(&decoder);

    while (offset < body_len) {
        cfw_span_t input;
        cfw_span_t output;
        size_t consumed;
        size_t chunk = body_len - offset;
        if (chunk > piece) {
            chunk = piece;
        }
        cfw_span_init(&input, body + offset, chunk);
        status = cfw_http_chunked_decode(&decoder, &input, &output,
                                         &consumed, finished);
        if (status != CFW_OK) {
            return status;
        }
        if (output.size > 0) {
            memcpy(collected + *collected_len, output.data, output.size);
            *collected_len += output.size;
        }
        offset += consumed;
        if (consumed == 0) {
            return CFW_ERROR_INTERNAL;
        }
        if (*finished) {
            break;
        }
    }
    return status;
}

static int test_http_chunked_decode(void)
{
    static const struct {
        const char *body;
        const char *expected;
        size_t expected_len;
    } cases[] = {
        {"4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n", "Wikipedia", 9},
        {"0\r\n\r\n", "", 0},
        {"0\r\nX-Trailer: yes\r\n\r\n", "", 0},
        {"a;foo=bar\r\n0123456789\r\n0\r\n\r\n", "0123456789", 10},
        {"1\r\nX\r\n1\r\nY\r\n0\r\n\r\n", "XY", 2},
    };
    static const char *invalid[] = {
        "Z\r\n",                    /* bad hex */
        "4\nWiki\r\n0\r\n\r\n",     /* bare LF in size line */
        "4\r\nWikiXX\r\n0\r\n\r\n",/* missing CRLF after data */
        "4;\r\nWiki\r\n0\r\n\r\n",  /* empty extension */
        "0\r\nBad Trailer\r\n\r\n", /* trailer without colon */
        "0\r\nX: a\rb\r\n\r\n",     /* CTL in trailer value */
    };
    size_t i;
    size_t piece;

    /* Every case at every split size from 1 byte up. */
    for (piece = 1; piece <= 9; piece++) {
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            char collected[64];
            size_t collected_len;
            int finished;
            CFW_TEST_ASSERT_EQ(
                cfw_test_chunked_feed(cases[i].body, strlen(cases[i].body),
                                      piece, collected, &collected_len,
                                      &finished),
                CFW_OK);
            CFW_TEST_ASSERT_TRUE(finished);
            CFW_TEST_ASSERT_EQ(collected_len, cases[i].expected_len);
            CFW_TEST_ASSERT_EQ(memcmp(collected, cases[i].expected,
                                      cases[i].expected_len),
                               0);
        }
    }

    /* Invalid bodies must fail. */
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        cfw_http_chunked_decoder_t decoder;
        cfw_span_t input;
        cfw_span_t output;
        size_t consumed;
        int finished = 0;
        size_t len = strlen(invalid[i]);

        cfw_http_chunked_decoder_init(&decoder);
        cfw_span_init(&input, invalid[i], len);
        CFW_TEST_ASSERT_EQ(cfw_http_chunked_decode(&decoder, &input,
                                                    &output, &consumed,
                                                    &finished),
                           CFW_ERROR_INVALID_ARGUMENT);
    }

    /* Pipelined bytes after the terminal chunk are left unconsumed. */
    {
        static const char pipelined[] =
            "1\r\nA\r\n0\r\n\r\nGET /next HTTP/1.1\r\n\r\n";
        cfw_http_chunked_decoder_t decoder;
        cfw_span_t input;
        cfw_span_t output;
        size_t consumed;
        int finished = 0;
        const char *tail;

        cfw_http_chunked_decoder_init(&decoder);
        cfw_span_init(&input, pipelined, sizeof(pipelined) - 1);
        CFW_TEST_ASSERT_EQ(cfw_http_chunked_decode(&decoder, &input,
                                                    &output, &consumed,
                                                    &finished),
                           CFW_OK);
        CFW_TEST_ASSERT_TRUE(finished);
        CFW_TEST_ASSERT_EQ(consumed, 11);
        tail = pipelined + consumed;
        CFW_TEST_ASSERT_EQ(memcmp(tail, "GET /next", 9), 0);
        CFW_TEST_ASSERT_EQ(output.size, 1);
        CFW_TEST_ASSERT_EQ(output.data[0], 'A');
    }

    CFW_TEST_ASSERT_EQ(cfw_http_chunked_decode(NULL, NULL, NULL, NULL,
                                               NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_http_response_serialize(void)
{
    cfw_http_version_t version = {1, 1};
    char buffer[512];
    size_t written;
    cfw_http_header_t headers[3];
    cfw_strview_t name;
    cfw_strview_t value;

    /* Status line. */
    CFW_TEST_ASSERT_EQ(cfw_http_status_line_format(&version,
                                                    CFW_HTTP_STATUS_OK,
                                                    buffer, sizeof(buffer),
                                                    &written),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(written, 17);
    CFW_TEST_ASSERT_EQ(memcmp(buffer, "HTTP/1.1 200 OK\r\n", 17), 0);

    /* Header line with validation. */
    cfw_strview_from_cstr(&name, "Content-Type");
    cfw_strview_from_cstr(&value, "text/html");
    CFW_TEST_ASSERT_EQ(cfw_http_header_line_format(&name, &value, buffer,
                                                    sizeof(buffer), &written),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(memcmp(buffer, "Content-Type: text/html\r\n", 25),
                       0);

    /* Full response head. */
    cfw_strview_from_cstr(&headers[0].name, "Content-Type");
    cfw_strview_from_cstr(&headers[0].value, "text/html");
    cfw_strview_from_cstr(&headers[1].name, "Content-Length");
    cfw_strview_from_cstr(&headers[1].value, "5");
    cfw_strview_from_cstr(&headers[2].name, "Connection");
    cfw_strview_from_cstr(&headers[2].value, "close");
    CFW_TEST_ASSERT_EQ(cfw_http_response_head_format(
                           &version, CFW_HTTP_STATUS_OK, headers, 3, buffer,
                           sizeof(buffer), &written),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(memcmp(buffer,
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/html\r\n"
                              "Content-Length: 5\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              written),
                       0);

    /* Buffer too small fails without partial writes trusted. */
    CFW_TEST_ASSERT_EQ(cfw_http_response_head_format(
                           &version, CFW_HTTP_STATUS_OK, headers, 3, buffer,
                           10, &written),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_http_header_line_format(&name, &value, buffer,
                                                    5, &written),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* Invalid inputs rejected. */
    {
        cfw_strview_t bad_name;
        cfw_strview_t bad_value;
        cfw_http_version_t bad_version = {10, 1};

        cfw_strview_from_cstr(&bad_name, "Bad Name");
        cfw_strview_from_cstr(&bad_value, "x");
        CFW_TEST_ASSERT_EQ(cfw_http_header_line_format(
                               &bad_name, &bad_value, buffer,
                               sizeof(buffer), &written),
                           CFW_ERROR_INVALID_ARGUMENT);

        cfw_strview_from_cstr(&bad_name, "X-Bad");
        cfw_strview_from_cstr(&bad_value, "a\rb");
        CFW_TEST_ASSERT_EQ(cfw_http_header_line_format(
                               &bad_name, &bad_value, buffer,
                               sizeof(buffer), &written),
                           CFW_ERROR_INVALID_ARGUMENT);

        CFW_TEST_ASSERT_EQ(cfw_http_status_line_format(
                               &bad_version, CFW_HTTP_STATUS_OK, buffer,
                               sizeof(buffer), &written),
                           CFW_ERROR_INVALID_ARGUMENT);
        CFW_TEST_ASSERT_EQ(cfw_http_status_line_format(
                               &version, (cfw_http_status_t)299, buffer,
                               sizeof(buffer), &written),
                           CFW_ERROR_INVALID_ARGUMENT);
        CFW_TEST_ASSERT_EQ(cfw_http_response_head_format(
                               &version, CFW_HTTP_STATUS_OK, NULL, 1, buffer,
                               sizeof(buffer), &written),
                           CFW_ERROR_INVALID_ARGUMENT);
    }

    /* Body-allowance rules. */
    CFW_TEST_ASSERT_EQ(cfw_http_response_has_body(CFW_HTTP_METHOD_GET,
                                                  CFW_HTTP_STATUS_OK), 1);
    CFW_TEST_ASSERT_EQ(cfw_http_response_has_body(CFW_HTTP_METHOD_HEAD,
                                                  CFW_HTTP_STATUS_OK), 0);
    CFW_TEST_ASSERT_EQ(cfw_http_response_has_body(CFW_HTTP_METHOD_GET,
                                                  CFW_HTTP_STATUS_NO_CONTENT),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_http_response_has_body(CFW_HTTP_METHOD_GET,
                                                  CFW_HTTP_STATUS_NOT_MODIFIED),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_http_response_has_body(CFW_HTTP_METHOD_GET,
                                                  CFW_HTTP_STATUS_CONTINUE),
                       0);
    return 0;
}

static int test_http_head_reader(void)
{
    static const char request[] =
        "POST /submit HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 4\r\n"
        "\r\n"
        "BODY";
    cfw_http_head_reader_t reader;
    cfw_strview_t head;
    cfw_span_t input;
    cfw_http_request_head_t parsed;
    size_t consumed;
    size_t i;

    /* Feed byte-by-byte; head completes exactly at the terminator and the
     * body bytes are reported unconsumed. */
    CFW_TEST_ASSERT_EQ(cfw_http_head_reader_init(&reader, 4096), CFW_OK);
    for (i = 0; i < sizeof(request) - 1; i++) {
        cfw_span_init(&input, request + i, 1);
        CFW_TEST_ASSERT_EQ(cfw_http_head_reader_feed(&reader, &input,
                                                      &consumed),
                           CFW_OK);
        if (!cfw_http_head_reader_is_complete(&reader)) {
            CFW_TEST_ASSERT_EQ(consumed, 1);
        } else {
            break;
        }
    }
    CFW_TEST_ASSERT_TRUE(cfw_http_head_reader_is_complete(&reader));
    /* The completing feed consumed only the last terminator byte. */
    CFW_TEST_ASSERT_EQ(consumed, 1);

    CFW_TEST_ASSERT_EQ(cfw_http_head_reader_head(&reader, &head), CFW_OK);
    CFW_TEST_ASSERT_EQ(head.size, sizeof(request) - 1 - 4);
    {
        cfw_strview_t view = cfw_test_view(request);
        view.size = head.size;
        CFW_TEST_ASSERT_EQ(cfw_http_request_head_parse(&view, &parsed),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(parsed.request_line.method, CFW_HTTP_METHOD_POST);
    }

    /* Feeding after completion consumes nothing. */
    cfw_span_init(&input, "XXXX", 4);
    CFW_TEST_ASSERT_EQ(cfw_http_head_reader_feed(&reader, &input, &consumed),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(consumed, 0);
    cfw_http_head_reader_free(&reader);

    /* One-shot feed with head + pipelined tail. */
    {
        static const char two[] =
            "GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n";
        CFW_TEST_ASSERT_EQ(cfw_http_head_reader_init(&reader, 4096), CFW_OK);
        cfw_span_init(&input, two, sizeof(two) - 1);
        CFW_TEST_ASSERT_EQ(cfw_http_head_reader_feed(&reader, &input,
                                                      &consumed),
                           CFW_OK);
        CFW_TEST_ASSERT_TRUE(cfw_http_head_reader_is_complete(&reader));
        CFW_TEST_ASSERT_EQ(consumed, 19);
        CFW_TEST_ASSERT_EQ(memcmp(two + consumed, "GET /b", 6), 0);
        cfw_http_head_reader_free(&reader);
    }

    /* Oversized head is rejected. */
    {
        char big[300];
        memset(big, 'A', sizeof(big));
        memcpy(big + sizeof(big) - 4, "\r\n\r\n", 4);
        CFW_TEST_ASSERT_EQ(cfw_http_head_reader_init(&reader, 128), CFW_OK);
        cfw_span_init(&input, big, sizeof(big));
        /* First feed fills the buffer to the limit without a terminator. */
        CFW_TEST_ASSERT_EQ(cfw_http_head_reader_feed(&reader, &input,
                                                      &consumed),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(consumed, 128);
        /* Any further byte proves the head can never complete. */
        CFW_TEST_ASSERT_EQ(cfw_http_head_reader_feed(&reader, &input,
                                                      &consumed),
                           CFW_ERROR_INVALID_ARGUMENT);
        cfw_http_head_reader_free(&reader);
    }

    /* Exact-limit head is accepted. */
    {
        /* "GET / HTTP/1.1\r\n\r\n" is 18 bytes; pad a header to reach 64. */
        char exact[64];
        size_t base = strlen("GET / HTTP/1.1\r\nX: ");
        memcpy(exact, "GET / HTTP/1.1\r\nX: ", base);
        memset(exact + base, 'a', sizeof(exact) - base - 4);
        memcpy(exact + sizeof(exact) - 4, "\r\n\r\n", 4);
        CFW_TEST_ASSERT_EQ(cfw_http_head_reader_init(&reader, 64), CFW_OK);
        cfw_span_init(&input, exact, sizeof(exact));
        CFW_TEST_ASSERT_EQ(cfw_http_head_reader_feed(&reader, &input,
                                                      &consumed),
                           CFW_OK);
        CFW_TEST_ASSERT_TRUE(cfw_http_head_reader_is_complete(&reader));
        cfw_http_head_reader_free(&reader);
    }

    CFW_TEST_ASSERT_EQ(cfw_http_head_reader_init(NULL, 10),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_http_head_reader_init(&reader, 0),
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
    {"http_request_framing", test_http_request_framing},
    {"http_chunked_decode", test_http_chunked_decode},
    {"http_response_serialize", test_http_response_serialize},
    {"http_head_reader", test_http_head_reader},
    {NULL, NULL},
};
