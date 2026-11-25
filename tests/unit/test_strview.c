/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

static int test_strview_init_and_cstr(void)
{
    cfw_strview_t view;
    int equal;
    cfw_strview_t expected;

    CFW_TEST_ASSERT_EQ(cfw_strview_init(&view, "hello", 5), CFW_OK);
    CFW_TEST_ASSERT_EQ(view.size, 5);
    CFW_TEST_ASSERT_EQ(view.data[0], 'h');

    CFW_TEST_ASSERT_EQ(cfw_strview_init(&view, NULL, 0), CFW_OK);
    CFW_TEST_ASSERT(view.data == NULL);
    CFW_TEST_ASSERT_EQ(view.size, 0);
    CFW_TEST_ASSERT_TRUE(cfw_strview_is_empty(&view));

    CFW_TEST_ASSERT_EQ(cfw_strview_init(&view, NULL, 1),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_strview_init(NULL, "x", 1),
                       CFW_ERROR_INVALID_ARGUMENT);

    CFW_TEST_ASSERT_EQ(cfw_strview_from_cstr(&view, "world"), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_strview_from_cstr(&expected, "world"), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_strview_equal(&view, &expected, &equal), CFW_OK);
    CFW_TEST_ASSERT_TRUE(equal);

    CFW_TEST_ASSERT_EQ(cfw_strview_from_cstr(&view, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_strview_span_conversion(void)
{
    cfw_strview_t view;
    cfw_span_t span;
    cfw_strview_t round_trip;

    CFW_TEST_ASSERT_EQ(cfw_strview_from_cstr(&view, "bytes"), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_strview_to_span(&view, &span), CFW_OK);
    CFW_TEST_ASSERT_EQ(span.size, 5);

    CFW_TEST_ASSERT_EQ(cfw_strview_from_span(&span, &round_trip), CFW_OK);
    CFW_TEST_ASSERT_EQ(round_trip.size, 5);
    CFW_TEST_ASSERT(round_trip.data == view.data);

    CFW_TEST_ASSERT_EQ(cfw_strview_to_span(NULL, &span),
                       CFW_ERROR_INVALID_ARGUMENT);
    {
        cfw_span_t bad_span = {NULL, 1};
        CFW_TEST_ASSERT_EQ(cfw_strview_from_span(&bad_span, &round_trip),
                           CFW_ERROR_INVALID_ARGUMENT);
    }
    return 0;
}

static int test_strview_equal(void)
{
    cfw_strview_t a;
    cfw_strview_t b;
    int equal;

    CFW_TEST_ASSERT_EQ(cfw_strview_from_cstr(&a, "abc"), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_strview_from_cstr(&b, "abc"), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_strview_equal(&a, &b, &equal), CFW_OK);
    CFW_TEST_ASSERT_TRUE(equal);

    CFW_TEST_ASSERT_EQ(cfw_strview_from_cstr(&b, "abd"), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_strview_equal(&a, &b, &equal), CFW_OK);
    CFW_TEST_ASSERT_FALSE(equal);

    CFW_TEST_ASSERT_EQ(cfw_strview_from_cstr(&b, "ab"), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_strview_equal(&a, &b, &equal), CFW_OK);
    CFW_TEST_ASSERT_FALSE(equal);

    CFW_TEST_ASSERT_EQ(cfw_strview_equal(&a, NULL, &equal),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_strview_equal(&a, &b, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_strview_equal_nocase(void)
{
    cfw_strview_t a;
    cfw_strview_t b;
    int equal;

    CFW_TEST_ASSERT_EQ(cfw_strview_from_cstr(&a, "Content-Type"), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_strview_from_cstr(&b, "content-type"), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_strview_equal_nocase(&a, &b, &equal), CFW_OK);
    CFW_TEST_ASSERT_TRUE(equal);

    CFW_TEST_ASSERT_EQ(cfw_strview_from_cstr(&b, "Content-Length"), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_strview_equal_nocase(&a, &b, &equal), CFW_OK);
    CFW_TEST_ASSERT_FALSE(equal);

    /* Non-ASCII bytes must not fold: only A-Z map to a-z. */
    CFW_TEST_ASSERT_EQ(cfw_strview_init(&a, "\xC1", 1), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_strview_init(&b, "\xE1", 1), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_strview_equal_nocase(&a, &b, &equal), CFW_OK);
    CFW_TEST_ASSERT_FALSE(equal);
    return 0;
}

static int test_strview_prefix_suffix(void)
{
    cfw_strview_t view;
    cfw_strview_t affix;
    int matches;

    CFW_TEST_ASSERT_EQ(cfw_strview_from_cstr(&view, "hello world"), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_strview_from_cstr(&affix, "hello"), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_strview_starts_with(&view, &affix, &matches),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(matches);

    CFW_TEST_ASSERT_EQ(cfw_strview_from_cstr(&affix, "world"), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_strview_ends_with(&view, &affix, &matches), CFW_OK);
    CFW_TEST_ASSERT_TRUE(matches);
    CFW_TEST_ASSERT_EQ(cfw_strview_starts_with(&view, &affix, &matches),
                       CFW_OK);
    CFW_TEST_ASSERT_FALSE(matches);

    /* Empty affixes match everything, including empty views. */
    CFW_TEST_ASSERT_EQ(cfw_strview_init(&affix, NULL, 0), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_strview_starts_with(&view, &affix, &matches),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(matches);
    CFW_TEST_ASSERT_EQ(cfw_strview_ends_with(&view, &affix, &matches), CFW_OK);
    CFW_TEST_ASSERT_TRUE(matches);

    CFW_TEST_ASSERT_EQ(cfw_strview_from_cstr(&affix, "hello world!"), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_strview_starts_with(&view, &affix, &matches),
                       CFW_OK);
    CFW_TEST_ASSERT_FALSE(matches);

    CFW_TEST_ASSERT_EQ(cfw_strview_starts_with(&view, NULL, &matches),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_strview_ends_with(&view, &affix, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

const cfw_test_case cfw_test_strview_cases[] = {
    {"strview_init_and_cstr", test_strview_init_and_cstr},
    {"strview_span_conversion", test_strview_span_conversion},
    {"strview_equal", test_strview_equal},
    {"strview_equal_nocase", test_strview_equal_nocase},
    {"strview_prefix_suffix", test_strview_prefix_suffix},
    {NULL, NULL},
};
