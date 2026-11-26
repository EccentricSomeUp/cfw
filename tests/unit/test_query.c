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

static int test_query_parse_basic(void)
{
    cfw_strview_t q = cfw_test_view("a=1&b=2&c=3");
    cfw_query_pair_t pairs[4];
    char scratch[64];
    size_t count = 0;
    size_t dropped = 999;
    static const char *const names[] = {"a", "b", "c"};
    static const char *const values[] = {"1", "2", "3"};
    size_t i;

    CFW_TEST_ASSERT_EQ(cfw_query_parse(&q, pairs, 4, scratch,
                                       sizeof(scratch), &count,
                                       &dropped),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 3);
    CFW_TEST_ASSERT_EQ(dropped, 0);
    for (i = 0; i < count; i++) {
        CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[i].name,
                                              names[i]));
        CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[i].value,
                                              values[i]));
    }
    return 0;
}

static int test_query_parse_forms(void)
{
    cfw_strview_t q = cfw_test_view("flag&empty=&noval&eq=a=b=c");
    cfw_query_pair_t pairs[8];
    char scratch[128];
    size_t count = 0;

    CFW_TEST_ASSERT_EQ(cfw_query_parse(&q, pairs, 8, scratch,
                                       sizeof(scratch), &count, NULL),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 4);
    /* Bare name → empty value. */
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[0].name, "flag"));
    CFW_TEST_ASSERT_EQ(pairs[0].value.size, 0);
    /* Explicit empty value. */
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[1].name, "empty"));
    CFW_TEST_ASSERT_EQ(pairs[1].value.size, 0);
    /* "noval" has no '='; "eq" keeps later '=' bytes literal. */
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[3].name, "eq"));
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[3].value, "a=b=c"));
    return 0;
}

static int test_query_parse_decoding(void)
{
    cfw_strview_t q = cfw_test_view("q=hello+world&pct=%41%42%43"
                                   "&slash=%2F&low=%af%Af");
    cfw_query_pair_t pairs[4];
    char scratch[128];
    size_t count = 0;

    CFW_TEST_ASSERT_EQ(cfw_query_parse(&q, pairs, 4, scratch,
                                       sizeof(scratch), &count, NULL),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 4);
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[0].value,
                                          "hello world"));
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[1].value, "ABC"));
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[2].value, "/"));
    /* Mixed-case hex decodes to the same bytes. */
    CFW_TEST_ASSERT_EQ(pairs[3].value.size, 2);
    CFW_TEST_ASSERT_EQ((unsigned char)pairs[3].value.data[0], 0xaf);
    CFW_TEST_ASSERT_EQ((unsigned char)pairs[3].value.data[1], 0xaf);
    return 0;
}

static int test_query_parse_empty_segments(void)
{
    cfw_strview_t q = cfw_test_view("&&a=1&&&b=2&");
    cfw_query_pair_t pairs[4];
    char scratch[32];
    size_t count = 0;

    CFW_TEST_ASSERT_EQ(cfw_query_parse(&q, pairs, 4, scratch,
                                       sizeof(scratch), &count, NULL),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 2);
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[0].name, "a"));
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[1].name, "b"));
    return 0;
}

static int test_query_parse_empty_query(void)
{
    cfw_strview_t q = cfw_test_view("");
    cfw_query_pair_t pairs[2];
    size_t count = 999;

    CFW_TEST_ASSERT_EQ(cfw_query_parse(&q, pairs, 2, NULL, 0, &count,
                                       NULL),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 0);
    return 0;
}

static int test_query_parse_capacity_and_drop(void)
{
    cfw_strview_t q = cfw_test_view("a=1&b=2&c=3&d=4");
    cfw_query_pair_t pairs[2];
    char scratch[32];
    size_t count = 0;
    size_t dropped = 0;

    CFW_TEST_ASSERT_EQ(cfw_query_parse(&q, pairs, 2, scratch,
                                       sizeof(scratch), &count,
                                       &dropped),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 2);
    CFW_TEST_ASSERT_EQ(dropped, 2);
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&pairs[1].name, "b"));

    /* Zero capacity still counts drops. */
    count = 0;
    dropped = 0;
    CFW_TEST_ASSERT_EQ(cfw_query_parse(&q, NULL, 0, scratch,
                                       sizeof(scratch), &count,
                                       &dropped),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 0);
    CFW_TEST_ASSERT_EQ(dropped, 4);
    return 0;
}

static int test_query_parse_malformed(void)
{
    static const char *const bad[] = {
        "a=%zz", "a=%1", "a=%", "a=100%", "a=%2", "a=%zz&b=1",
    };
    cfw_query_pair_t pairs[4];
    char scratch[32];
    size_t count = 0;
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        cfw_strview_t q = cfw_test_view(bad[i]);
        CFW_TEST_ASSERT_EQ(cfw_query_parse(&q, pairs, 4, scratch,
                                           sizeof(scratch), &count,
                                           NULL),
                           CFW_ERROR_INVALID_ARGUMENT);
    }
    return 0;
}

static int test_query_parse_arguments(void)
{
    cfw_strview_t q = cfw_test_view("a=1");
    cfw_query_pair_t pairs[2];
    char scratch[8];
    size_t count = 0;

    CFW_TEST_ASSERT_EQ(cfw_query_parse(NULL, pairs, 2, scratch,
                                       sizeof(scratch), &count, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_query_parse(&q, pairs, 2, scratch,
                                       sizeof(scratch), NULL, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    /* Scratch smaller than the query can never suffice. */
    CFW_TEST_ASSERT_EQ(cfw_query_parse(&q, pairs, 2, scratch, 2,
                                       &count, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    /* pairs required when capacity > 0. */
    CFW_TEST_ASSERT_EQ(cfw_query_parse(&q, NULL, 2, scratch,
                                       sizeof(scratch), &count, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_http_target_query(void)
{
    cfw_strview_t target;
    cfw_strview_t query;

    target = cfw_test_view("/path/to?x=1&y=2");
    CFW_TEST_ASSERT_EQ(cfw_http_target_query(&target, &query), CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&query, "x=1&y=2"));

    /* First '?' wins; later '?' bytes stay literal in the query. */
    target = cfw_test_view("/p?a=?b");
    CFW_TEST_ASSERT_EQ(cfw_http_target_query(&target, &query), CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&query, "a=?b"));

    /* Absolute-form works the same. */
    target = cfw_test_view("http://example.com/p?q=1");
    CFW_TEST_ASSERT_EQ(cfw_http_target_query(&target, &query), CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_test_view_is(&query, "q=1"));

    /* No query → empty view, still CFW_OK. */
    target = cfw_test_view("/plain");
    CFW_TEST_ASSERT_EQ(cfw_http_target_query(&target, &query), CFW_OK);
    CFW_TEST_ASSERT_EQ(query.size, 0);

    /* Trailing '?' yields an empty query. */
    target = cfw_test_view("/p?");
    CFW_TEST_ASSERT_EQ(cfw_http_target_query(&target, &query), CFW_OK);
    CFW_TEST_ASSERT_EQ(query.size, 0);

    CFW_TEST_ASSERT_EQ(cfw_http_target_query(NULL, &query),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

const cfw_test_case cfw_test_query_cases[] = {
    {"query_parse_basic", test_query_parse_basic},
    {"query_parse_forms", test_query_parse_forms},
    {"query_parse_decoding", test_query_parse_decoding},
    {"query_parse_empty_segments", test_query_parse_empty_segments},
    {"query_parse_empty_query", test_query_parse_empty_query},
    {"query_parse_capacity_and_drop", test_query_parse_capacity_and_drop},
    {"query_parse_malformed", test_query_parse_malformed},
    {"query_parse_arguments", test_query_parse_arguments},
    {"http_target_query", test_http_target_query},
    {NULL, NULL},
};
