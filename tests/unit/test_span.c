/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <stddef.h>
#include <stdint.h>

static int test_span_init_success(void)
{
    uint8_t bytes[] = {1, 2, 3, 4};
    cfw_span_t span;

    CFW_TEST_ASSERT_EQ(cfw_span_init(&span, bytes, sizeof(bytes)), CFW_OK);
    CFW_TEST_ASSERT(span.data == bytes);
    CFW_TEST_ASSERT_EQ(span.size, sizeof(bytes));
    return 0;
}

static int test_span_init_empty_allows_null_data(void)
{
    cfw_span_t span;

    CFW_TEST_ASSERT_EQ(cfw_span_init(&span, NULL, 0), CFW_OK);
    CFW_TEST_ASSERT(span.data == NULL);
    CFW_TEST_ASSERT_EQ(span.size, 0);
    return 0;
}

static int test_span_init_rejects_null_span(void)
{
    uint8_t bytes[] = {1};

    CFW_TEST_ASSERT_EQ(cfw_span_init(NULL, bytes, sizeof(bytes)),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_span_init_rejects_null_data_with_size(void)
{
    cfw_span_t span;
    span.data = NULL;
    span.size = 99;

    CFW_TEST_ASSERT_EQ(cfw_span_init(&span, NULL, 1),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(span.size, 99);
    return 0;
}

static int test_span_subspan_success(void)
{
    uint8_t bytes[] = {1, 2, 3, 4, 5, 6};
    cfw_span_t span;
    cfw_span_t subspan;

    CFW_TEST_ASSERT_EQ(cfw_span_init(&span, bytes, sizeof(bytes)), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_span_subspan(&span, 2, 3, &subspan), CFW_OK);
    CFW_TEST_ASSERT(subspan.data == bytes + 2);
    CFW_TEST_ASSERT_EQ(subspan.size, 3);
    CFW_TEST_ASSERT_EQ(subspan.data[0], 3);
    CFW_TEST_ASSERT_EQ(subspan.data[2], 5);
    return 0;
}

static int test_span_subspan_boundaries(void)
{
    uint8_t bytes[] = {1, 2, 3};
    cfw_span_t span;
    cfw_span_t subspan;

    CFW_TEST_ASSERT_EQ(cfw_span_init(&span, bytes, sizeof(bytes)), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_span_subspan(&span, 0, sizeof(bytes), &subspan),
                       CFW_OK);
    CFW_TEST_ASSERT(subspan.data == bytes);
    CFW_TEST_ASSERT_EQ(subspan.size, sizeof(bytes));

    CFW_TEST_ASSERT_EQ(cfw_span_subspan(&span, sizeof(bytes), 0, &subspan),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(subspan.size, 0);

    CFW_TEST_ASSERT_EQ(cfw_span_subspan(&span, sizeof(bytes) - 1, 1, &subspan),
                       CFW_OK);
    CFW_TEST_ASSERT(subspan.data == bytes + sizeof(bytes) - 1);
    CFW_TEST_ASSERT_EQ(subspan.size, 1);
    return 0;
}

static int test_span_subspan_rejects_out_of_range(void)
{
    uint8_t bytes[] = {1, 2, 3};
    cfw_span_t span;
    cfw_span_t subspan;

    CFW_TEST_ASSERT_EQ(cfw_span_init(&span, bytes, sizeof(bytes)), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_span_subspan(&span, sizeof(bytes) + 1, 0, &subspan),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_span_subspan(&span, 2, 2, &subspan),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_span_subspan(&span, sizeof(bytes), 1, &subspan),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_span_subspan_rejects_null_arguments(void)
{
    uint8_t bytes[] = {1, 2, 3};
    cfw_span_t span;

    CFW_TEST_ASSERT_EQ(cfw_span_init(&span, bytes, sizeof(bytes)), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_span_subspan(NULL, 0, 1, &span),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_span_subspan(&span, 0, 1, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_span_subspan_rejects_invalid_span(void)
{
    cfw_span_t invalid = {NULL, 1};
    cfw_span_t result;

    CFW_TEST_ASSERT_EQ(cfw_span_subspan(&invalid, 0, 0, &result),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_span_subspan_preserves_result_on_error(void)
{
    uint8_t bytes[] = {1, 2, 3};
    cfw_span_t span;
    cfw_span_t result;

    CFW_TEST_ASSERT_EQ(cfw_span_init(&span, bytes, sizeof(bytes)), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_span_init(&result, bytes, sizeof(bytes)), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_span_subspan(&span, 0, sizeof(bytes) + 1, &result),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT(result.data == bytes);
    CFW_TEST_ASSERT_EQ(result.size, sizeof(bytes));
    return 0;
}

const cfw_test_case cfw_test_span_cases[] = {
    {"span_init_success", test_span_init_success},
    {"span_init_empty_allows_null_data", test_span_init_empty_allows_null_data},
    {"span_init_rejects_null_span", test_span_init_rejects_null_span},
    {"span_init_rejects_null_data_with_size",
     test_span_init_rejects_null_data_with_size},
    {"span_subspan_success", test_span_subspan_success},
    {"span_subspan_boundaries", test_span_subspan_boundaries},
    {"span_subspan_rejects_out_of_range", test_span_subspan_rejects_out_of_range},
    {"span_subspan_rejects_null_arguments",
     test_span_subspan_rejects_null_arguments},
    {"span_subspan_rejects_invalid_span", test_span_subspan_rejects_invalid_span},
    {"span_subspan_preserves_result_on_error",
     test_span_subspan_preserves_result_on_error},
    {NULL, NULL},
};
