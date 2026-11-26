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

static int test_cookie_parse_basic(void)
{
    cfw_strview_t h = cfw_test_view("session=abc123; theme=dark; cart=");
    cfw_cookie_pair_t pairs[4];
    size_t count = 0;
    size_t dropped = 999;

    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 4, &count,
                                        &dropped),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 3);
    CFW_TEST_ASSERT_EQ(dropped, 0);
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[0].name, "session"));
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[0].value, "abc123"));
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[1].name, "theme"));
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[1].value, "dark"));
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[2].name, "cart"));
    CFW_TEST_ASSERT_EQ(pairs[2].value.size, 0);
    return 0;
}

static int test_cookie_parse_whitespace_and_quotes(void)
{
    cfw_strview_t h = cfw_test_view("  a=1 ; b=2 ;\tc=3  ");
    cfw_cookie_pair_t pairs[4];
    size_t count = 0;

    /* OWS around segments and values is tolerated. */
    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 4, &count, NULL),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 3);
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[0].name, "a"));
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[0].value, "1"));
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[1].name, "b"));
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[2].name, "c"));
    return 0;
}

static int test_cookie_parse_quoted_value(void)
{
    cfw_strview_t h = cfw_test_view("a=\"x=y\";b=2");
    cfw_cookie_pair_t pairs[4];
    size_t count = 0;

    /* '=' inside a quoted value: the first '=' after the name still
     * splits, and the remaining '=' is literal value data. The
     * surrounding DQUOTEs are stripped from the returned view. */
    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 4, &count, NULL),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 2);
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[0].name, "a"));
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[0].value, "x=y"));
    return 0;
}

static int test_cookie_parse_real_quoted(void)
{
    cfw_strview_t h = cfw_test_view("pref=\"dark-mode\"; n=1");
    cfw_cookie_pair_t pairs[4];
    size_t count = 0;

    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 4, &count, NULL),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 2);
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[0].name, "pref"));
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[0].value,
                                          "dark-mode"));
    return 0;
}

static int test_cookie_parse_empty_and_lenient(void)
{
    cfw_strview_t h;
    cfw_cookie_pair_t pairs[4];
    size_t count = 999;

    /* Empty header → zero pairs. */
    h = cfw_test_view("");
    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 4, &count, NULL),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 0);

    /* Empty segments skipped. */
    h = cfw_test_view("a=1;;  ;b=2;");
    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 4, &count, NULL),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 2);

    /* '=' inside the value stays literal (first '=' splits). */
    h = cfw_test_view("tok=a=b=c");
    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 4, &count, NULL),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 1);
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[0].value, "a=b=c"));
    return 0;
}

static int test_cookie_parse_capacity(void)
{
    cfw_strview_t h = cfw_test_view("a=1;b=2;c=3");
    cfw_cookie_pair_t pairs[2];
    size_t count = 0;
    size_t dropped = 0;

    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 2, &count,
                                        &dropped),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 2);
    CFW_TEST_ASSERT_EQ(dropped, 1);
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[1].name, "b"));
    return 0;
}

static int test_cookie_parse_malformed(void)
{
    cfw_strview_t h;
    cfw_cookie_pair_t pairs[4];
    size_t count = 0;

    /* Segment without '='. */
    h = cfw_test_view("a=1;novalue");
    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 4, &count, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* Empty name. */
    h = cfw_test_view("=1");
    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 4, &count, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* Name with space (not a token char). */
    h = cfw_test_view("a b=1");
    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 4, &count, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* OWS before '=' is not a token char → rejected. */
    h = cfw_test_view("a =1");
    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 4, &count, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* Space inside unquoted value is not a cookie-octet. */
    h = cfw_test_view("a=b c");
    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 4, &count, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* Comma/semicolon/backslash rejected in values. */
    h = cfw_test_view("a=b,c");
    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 4, &count, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    h = cfw_test_view("a=b\\c");
    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 4, &count, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* Unterminated quote → DQUOTE is not a cookie-octet. */
    h = cfw_test_view("a=\"bc");
    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 4, &count, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* Space inside a quoted value is still not a cookie-octet. */
    h = cfw_test_view("a=\"b c\"");
    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 4, &count, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* Non-ASCII byte rejected. */
    {
        char raw[] = {'a', '=', 'b', (char)0xC3, (char)0xA9};
        cfw_strview_t hv;
        cfw_strview_init(&hv, raw, sizeof(raw));
        CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&hv, pairs, 4, &count,
                                            NULL),
                           CFW_ERROR_INVALID_ARGUMENT);
    }
    return 0;
}

static int test_cookie_parse_arguments(void)
{
    cfw_strview_t h = cfw_test_view("a=1");
    cfw_cookie_pair_t pairs[2];
    size_t count = 0;

    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(NULL, pairs, 2, &count,
                                        NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, NULL, 2, &count, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 2, NULL, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 0, &count, NULL),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 0);
    return 0;
}

static cfw_cookie_t cfw_test_cookie(const char *name,
                                    const char *value)
{
    cfw_cookie_t c;
    memset(&c, 0, sizeof(c));
    cfw_strview_from_cstr(&c.name, name);
    cfw_strview_from_cstr(&c.value, value);
    c.max_age_seconds = -1;
    c.expires_unix = -1;
    c.samesite = CFW_COOKIE_SAMESITE_UNSET;
    return c;
}

static int test_cookie_serialize_basic(void)
{
    cfw_cookie_t c = cfw_test_cookie("session", "abc123");
    char dst[256];
    size_t written = 0;

    CFW_TEST_ASSERT_EQ(cfw_cookie_serialize(&c, dst, sizeof(dst),
                                            &written),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(strcmp(dst, "session=abc123") == 0);
    CFW_TEST_ASSERT_EQ(written, strlen("session=abc123"));

    /* Empty value is legal. */
    c = cfw_test_cookie("empty", "");
    CFW_TEST_ASSERT_EQ(cfw_cookie_serialize(&c, dst, sizeof(dst),
                                            &written),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(strcmp(dst, "empty=") == 0);
    return 0;
}

static int test_cookie_serialize_attributes(void)
{
    cfw_cookie_t c = cfw_test_cookie("s", "v");
    char dst[512];
    size_t written = 0;

    cfw_strview_from_cstr(&c.path, "/app");
    cfw_strview_from_cstr(&c.domain, "example.com");
    c.max_age_seconds = 3600;
    c.expires_unix = 1700000000; /* 2023-11-14T22:13:20Z */
    c.secure = 1;
    c.httponly = 1;
    c.partitioned = 1;
    c.samesite = CFW_COOKIE_SAMESITE_LAX;

    CFW_TEST_ASSERT_EQ(cfw_cookie_serialize(&c, dst, sizeof(dst),
                                            &written),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(strcmp(
                             dst,
                             "s=v; Path=/app; Domain=example.com; "
                             "Max-Age=3600; Expires=Tue, 14 Nov 2023 "
                             "22:13:20 GMT; Secure; HttpOnly; "
                             "Partitioned; SameSite=Lax") == 0);

    /* SameSite variants. */
    c.partitioned = 0;
    c.domain = cfw_test_view("");
    c.path = cfw_test_view("");
    c.max_age_seconds = -1;
    c.expires_unix = -1;
    c.samesite = CFW_COOKIE_SAMESITE_STRICT;
    CFW_TEST_ASSERT_EQ(cfw_cookie_serialize(&c, dst, sizeof(dst),
                                            &written),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(strcmp(dst, "s=v; Secure; HttpOnly; "
                                     "SameSite=Strict") == 0);

    c.samesite = CFW_COOKIE_SAMESITE_NONE;
    CFW_TEST_ASSERT_EQ(cfw_cookie_serialize(&c, dst, sizeof(dst),
                                            &written),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(strstr(dst, "SameSite=None") != NULL);
    return 0;
}

static int test_cookie_serialize_validation(void)
{
    cfw_cookie_t c = cfw_test_cookie("ok", "v");
    char dst[256];
    size_t written = 0;

    /* Empty name rejected. */
    c.name = cfw_test_view("");
    CFW_TEST_ASSERT_EQ(cfw_cookie_serialize(&c, dst, sizeof(dst),
                                            &written),
                       CFW_ERROR_INVALID_ARGUMENT);
    c = cfw_test_cookie("ok", "v");

    /* Space in name is not a tchar. */
    cfw_strview_from_cstr(&c.name, "a b");
    CFW_TEST_ASSERT_EQ(cfw_cookie_serialize(&c, dst, sizeof(dst),
                                            &written),
                       CFW_ERROR_INVALID_ARGUMENT);
    c = cfw_test_cookie("ok", "v");

    /* Semicolon in value rejected (would break header syntax). */
    cfw_strview_from_cstr(&c.value, "a;b");
    CFW_TEST_ASSERT_EQ(cfw_cookie_serialize(&c, dst, sizeof(dst),
                                            &written),
                       CFW_ERROR_INVALID_ARGUMENT);
    c = cfw_test_cookie("ok", "v");

    /* DQUOTE in value rejected. */
    cfw_strview_from_cstr(&c.value, "a\"b");
    CFW_TEST_ASSERT_EQ(cfw_cookie_serialize(&c, dst, sizeof(dst),
                                            &written),
                       CFW_ERROR_INVALID_ARGUMENT);
    c = cfw_test_cookie("ok", "v");

    /* Path must start with '/'. */
    cfw_strview_from_cstr(&c.path, "app");
    CFW_TEST_ASSERT_EQ(cfw_cookie_serialize(&c, dst, sizeof(dst),
                                            &written),
                       CFW_ERROR_INVALID_ARGUMENT);
    c = cfw_test_cookie("ok", "v");

    /* Invalid samesite enum rejected. */
    c.samesite = (cfw_cookie_samesite_t)99;
    CFW_TEST_ASSERT_EQ(cfw_cookie_serialize(&c, dst, sizeof(dst),
                                            &written),
                       CFW_ERROR_INVALID_ARGUMENT);
    c = cfw_test_cookie("ok", "v");

    /* Undersized dst → error. */
    CFW_TEST_ASSERT_EQ(cfw_cookie_serialize(&c, dst, 4, &written),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* NULL args. */
    CFW_TEST_ASSERT_EQ(cfw_cookie_serialize(NULL, dst, sizeof(dst),
                                            &written),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_cookie_serialize(&c, NULL, sizeof(dst),
                                            &written),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_cookie_serialize(&c, dst, sizeof(dst),
                                            NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_cookie_roundtrip(void)
{
    /* Serialize then parse the name=value portion. */
    cfw_cookie_t c = cfw_test_cookie("uid", "u-123_x.y");
    char dst[128];
    size_t written = 0;
    cfw_strview_t h;
    cfw_cookie_pair_t pairs[2];
    size_t count = 0;

    CFW_TEST_ASSERT_EQ(cfw_cookie_serialize(&c, dst, sizeof(dst),
                                            &written),
                       CFW_OK);
    /* Feed just the pair portion (before any ';'). */
    h = cfw_test_view(dst);
    {
        const char *semi = strchr(dst, ';');
        if (semi != NULL) {
            h.size = (size_t)(semi - dst);
        }
    }
    CFW_TEST_ASSERT_EQ(cfw_cookie_parse(&h, pairs, 2, &count,
                                        NULL),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 1);
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[0].name, "uid"));
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[0].value,
                                          "u-123_x.y"));
    return 0;
}

const cfw_test_case cfw_test_cookie_cases[] = {
    {"cookie_parse_basic", test_cookie_parse_basic},
    {"cookie_parse_whitespace_and_quotes", test_cookie_parse_whitespace_and_quotes},
    {"cookie_parse_quoted_value", test_cookie_parse_quoted_value},
    {"cookie_parse_real_quoted", test_cookie_parse_real_quoted},
    {"cookie_parse_empty_and_lenient", test_cookie_parse_empty_and_lenient},
    {"cookie_parse_capacity", test_cookie_parse_capacity},
    {"cookie_parse_malformed", test_cookie_parse_malformed},
    {"cookie_parse_arguments", test_cookie_parse_arguments},
    {"cookie_serialize_basic", test_cookie_serialize_basic},
    {"cookie_serialize_attributes", test_cookie_serialize_attributes},
    {"cookie_serialize_validation", test_cookie_serialize_validation},
    {"cookie_roundtrip", test_cookie_roundtrip},
    {NULL, NULL},
};
