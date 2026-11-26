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

static int cfw_test_view_is(const cfw_strview_t *view, const char *text)
{
    return view->size == strlen(text) &&
           memcmp(view->data, text, view->size) == 0;
}

static int cfw_test_span_is(const cfw_span_t *span, const char *text)
{
    return span->size == strlen(text) &&
           memcmp(span->data, text, span->size) == 0;
}

static int test_multipart_boundary(void)
{
    cfw_strview_t ct;
    cfw_strview_t b;

    ct = cfw_test_view("multipart/form-data; boundary=abc123");
    CFW_TEST_ASSERT_EQ(cfw_multipart_boundary(&ct, &b), CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&b, "abc123"));

    /* Quoted boundary, extra params, case-insensitive name. */
    ct = cfw_test_view("Multipart/Form-Data; charset=utf-8; "
                       "BOUNDARY=\"Aa Bb-1_2\"; foo=bar");
    CFW_TEST_ASSERT_EQ(cfw_multipart_boundary(&ct, &b), CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&b, "Aa Bb-1_2"));

    /* Missing, empty, over-long, and malformed boundaries fail. */
    ct = cfw_test_view("multipart/form-data");
    CFW_TEST_ASSERT_EQ(cfw_multipart_boundary(&ct, &b),
                       CFW_ERROR_INVALID_ARGUMENT);
    ct = cfw_test_view("multipart/form-data; boundary=");
    CFW_TEST_ASSERT_EQ(cfw_multipart_boundary(&ct, &b),
                       CFW_ERROR_INVALID_ARGUMENT);
    ct = cfw_test_view("multipart/form-data; boundary=\"\"");
    CFW_TEST_ASSERT_EQ(cfw_multipart_boundary(&ct, &b),
                       CFW_ERROR_INVALID_ARGUMENT);
    {
        char longb[80];
        memset(longb, 'x', sizeof(longb) - 1);
        longb[sizeof(longb) - 1] = '\0';
        {
            char ctbuf[128];
            snprintf(ctbuf, sizeof(ctbuf),
                     "multipart/form-data; boundary=%s", longb);
            ct = cfw_test_view(ctbuf);
            CFW_TEST_ASSERT_EQ(cfw_multipart_boundary(&ct, &b),
                               CFW_ERROR_INVALID_ARGUMENT);
        }
    }
    ct = cfw_test_view("multipart/form-data; boundary=\"unterm");
    CFW_TEST_ASSERT_EQ(cfw_multipart_boundary(&ct, &b),
                       CFW_ERROR_INVALID_ARGUMENT);
    ct = cfw_test_view("multipart/form-data; boundary=a\\b");
    CFW_TEST_ASSERT_EQ(cfw_multipart_boundary(&ct, &b),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

/* Builds a canonical two-part multipart body. */
static void cfw_test_mp_body(char *out, size_t cap)
{
    snprintf(out, cap,
             "--XYZ\r\n"
             "Content-Disposition: form-data; name=\"field1\"\r\n"
             "\r\n"
             "value1\r\n"
             "--XYZ\r\n"
             "Content-Disposition: form-data; name=\"file\"; "
             "filename=\"a.txt\"\r\n"
             "Content-Type: text/plain\r\n"
             "\r\n"
             "file contents\r\n"
             "--XYZ--\r\n");
}

static int test_multipart_parse_basic(void)
{
    char raw[1024];
    cfw_span_t body;
    cfw_strview_t boundary;
    cfw_multipart_part_t parts[4];
    char scratch[2048];
    size_t count = 0;
    size_t dropped = 0;

    cfw_test_mp_body(raw, sizeof(raw));
    cfw_span_init(&body, raw, strlen(raw));
    boundary = cfw_test_view("XYZ");

    CFW_TEST_ASSERT_EQ(cfw_multipart_parse(&body, &boundary, parts, 4,
                                           scratch, sizeof(scratch),
                                           &count, &dropped),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 2);
    CFW_TEST_ASSERT_EQ(dropped, 0);

    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&parts[0].name, "field1"));
    CFW_TEST_ASSERT_EQ(parts[0].filename.size, 0);
    CFW_TEST_ASSERT_EQ(parts[0].content_type.size, 0);
    CFW_TEST_ASSERT_TRUE(cfw_test_span_is(&parts[0].body, "value1"));
    CFW_TEST_ASSERT_TRUE(parts[0].header_block.size != 0);

    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&parts[1].name, "file"));
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&parts[1].filename,
                                          "a.txt"));
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&parts[1].content_type,
                                          "text/plain"));
    CFW_TEST_ASSERT_TRUE(cfw_test_span_is(&parts[1].body,
                                          "file contents"));
    return 0;
}

static int test_multipart_parse_preamble_epilogue_padding(void)
{
    static const char raw[] =
        "preamble junk\r\nmore junk\r\n"
        "--XYZ \t\r\n"
        "Content-Disposition: form-data; name=\"a\"\r\n"
        "\r\n"
        "A\r\n"
        "--XYZ-- \t\r\n"
        "epilogue is ignored";
    cfw_span_t body;
    cfw_strview_t boundary;
    cfw_multipart_part_t parts[2];
    char scratch[256];
    size_t count = 0;

    cfw_span_init(&body, raw, sizeof(raw) - 1);
    boundary = cfw_test_view("XYZ");
    CFW_TEST_ASSERT_EQ(cfw_multipart_parse(&body, &boundary, parts, 2,
                                           scratch, sizeof(scratch),
                                           &count, NULL),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 1);
    CFW_TEST_ASSERT_TRUE(cfw_test_span_is(&parts[0].body, "A"));
    return 0;
}

static int test_multipart_parse_quoted_escapes(void)
{
    static const char raw[] =
        "--XYZ\r\n"
        "Content-Disposition: form-data; name=plain; "
        "filename=\"quo\\\"ted.txt\"; extra=ignored\r\n"
        "\r\n"
        "x\r\n"
        "--XYZ--";
    cfw_span_t body;
    cfw_strview_t boundary;
    cfw_multipart_part_t parts[2];
    char scratch[256];
    size_t count = 0;

    cfw_span_init(&body, raw, sizeof(raw) - 1);
    boundary = cfw_test_view("XYZ");
    CFW_TEST_ASSERT_EQ(cfw_multipart_parse(&body, &boundary, parts, 2,
                                           scratch, sizeof(scratch),
                                           &count, NULL),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 1);
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&parts[0].name, "plain"));
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&parts[0].filename,
                                          "quo\"ted.txt"));
    return 0;
}

static int test_multipart_parse_no_headers_and_empty_body(void)
{
    static const char raw[] =
        "--XYZ\r\n"
        "\r\n"
        "headerless body\r\n"
        "--XYZ\r\n"
        "Content-Disposition: form-data; name=\"e\"\r\n"
        "\r\n"
        "\r\n"
        "--XYZ--";
    cfw_span_t body;
    cfw_strview_t boundary;
    cfw_multipart_part_t parts[4];
    char scratch[256];
    size_t count = 0;

    cfw_span_init(&body, raw, sizeof(raw) - 1);
    boundary = cfw_test_view("XYZ");
    CFW_TEST_ASSERT_EQ(cfw_multipart_parse(&body, &boundary, parts, 4,
                                           scratch, sizeof(scratch),
                                           &count, NULL),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 2);
    /* First part has no headers: name empty, raw block empty. */
    CFW_TEST_ASSERT_EQ(parts[0].name.size, 0);
    CFW_TEST_ASSERT_EQ(parts[0].header_block.size, 0);
    CFW_TEST_ASSERT_TRUE(cfw_test_span_is(&parts[0].body,
                                          "headerless body"));
    /* Second part body is empty (blank line then delimiter). */
    CFW_TEST_ASSERT_EQ(parts[1].body.size, 0);
    return 0;
}

static int test_multipart_parse_boundary_prefix(void)
{
    /* A body containing "--XYZabc" (boundary as a prefix of a longer
     * token) must not terminate the part. */
    static const char raw[] =
        "--XYZ\r\n"
        "Content-Disposition: form-data; name=\"f\"; "
        "filename=\"x.bin\"\r\n"
        "\r\n"
        "data \r\n--XYZabc more data\r\n"
        "--XYZ--";
    cfw_span_t body;
    cfw_strview_t boundary;
    cfw_multipart_part_t parts[2];
    char scratch[256];
    size_t count = 0;

    cfw_span_init(&body, raw, sizeof(raw) - 1);
    boundary = cfw_test_view("XYZ");
    CFW_TEST_ASSERT_EQ(cfw_multipart_parse(&body, &boundary, parts, 2,
                                           scratch, sizeof(scratch),
                                           &count, NULL),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 1);
    CFW_TEST_ASSERT_TRUE(cfw_test_span_is(
        &parts[0].body, "data \r\n--XYZabc more data"));
    return 0;
}

static int test_multipart_parse_malformed(void)
{
    static const char *const bad[] = {
        "no delimiters at all",
        ("--XYZ\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\n"
         "body without close"),
        "--XYZ\r\nContent-Disposition: form-data; name=\"a\"",
        "--XYZ\r\nnot a header line\r\n\r\nx\r\n--XYZ--",
    };
    cfw_span_t body;
    cfw_strview_t boundary;
    cfw_multipart_part_t parts[2];
    char scratch[512];
    size_t count = 0;
    size_t i;

    boundary = cfw_test_view("XYZ");
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        cfw_span_init(&body, bad[i], strlen(bad[i]));
        CFW_TEST_ASSERT_EQ(cfw_multipart_parse(&body, &boundary,
                                               parts, 2, scratch,
                                               sizeof(scratch), &count,
                                               NULL),
                           CFW_ERROR_INVALID_ARGUMENT);
    }
    return 0;
}

static int test_multipart_parse_capacity(void)
{
    char raw[1024];
    cfw_span_t body;
    cfw_strview_t boundary;
    cfw_multipart_part_t parts[1];
    char scratch[2048];
    size_t count = 0;
    size_t dropped = 0;

    cfw_test_mp_body(raw, sizeof(raw));
    cfw_span_init(&body, raw, strlen(raw));
    boundary = cfw_test_view("XYZ");
    CFW_TEST_ASSERT_EQ(cfw_multipart_parse(&body, &boundary, parts, 1,
                                           scratch, sizeof(scratch),
                                           &count, &dropped),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 1);
    CFW_TEST_ASSERT_EQ(dropped, 1);
    return 0;
}

static int test_multipart_parse_arguments(void)
{
    char raw[256];
    cfw_span_t body;
    cfw_strview_t boundary;
    cfw_multipart_part_t parts[2];
    char scratch[64];
    size_t count = 0;

    cfw_test_mp_body(raw, sizeof(raw));
    cfw_span_init(&body, raw, strlen(raw));
    boundary = cfw_test_view("XYZ");

    CFW_TEST_ASSERT_EQ(cfw_multipart_parse(NULL, &boundary, parts, 2,
                                           scratch, sizeof(scratch),
                                           &count, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_multipart_parse(&body, &boundary, parts, 2,
                                           scratch, 8, &count, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_multipart_parse(&body, &boundary, NULL, 2,
                                           scratch, sizeof(scratch),
                                           &count, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_multipart_parse(&body, &boundary, parts, 2,
                                           scratch, sizeof(scratch),
                                           NULL, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

const cfw_test_case cfw_test_multipart_cases[] = {
    {"multipart_boundary", test_multipart_boundary},
    {"multipart_parse_basic", test_multipart_parse_basic},
    {"multipart_parse_preamble_epilogue_padding",
     test_multipart_parse_preamble_epilogue_padding},
    {"multipart_parse_quoted_escapes",
     test_multipart_parse_quoted_escapes},
    {"multipart_parse_no_headers_and_empty_body",
     test_multipart_parse_no_headers_and_empty_body},
    {"multipart_parse_boundary_prefix",
     test_multipart_parse_boundary_prefix},
    {"multipart_parse_malformed", test_multipart_parse_malformed},
    {"multipart_parse_capacity", test_multipart_parse_capacity},
    {"multipart_parse_arguments", test_multipart_parse_arguments},
    {NULL, NULL},
};
