/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int test_buffer_init_success(void)
{
    cfw_buffer_t buffer;

    CFW_TEST_ASSERT_EQ(cfw_buffer_init(&buffer), CFW_OK);
    CFW_TEST_ASSERT(buffer.data == NULL);
    CFW_TEST_ASSERT_EQ(buffer.size, 0);
    CFW_TEST_ASSERT_EQ(buffer.capacity, 0);
    return 0;
}

static int test_buffer_init_rejects_null(void)
{
    CFW_TEST_ASSERT_EQ(cfw_buffer_init(NULL), CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_buffer_append_success_and_growth(void)
{
    cfw_buffer_t buffer;

    CFW_TEST_ASSERT_EQ(cfw_buffer_init(&buffer), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_buffer_append(&buffer, "hello", 5), CFW_OK);
    CFW_TEST_ASSERT_EQ(buffer.size, 5);
    CFW_TEST_ASSERT(buffer.capacity >= 5);
    CFW_TEST_ASSERT(buffer.data != NULL);

    CFW_TEST_ASSERT_EQ(cfw_buffer_append(&buffer, "world", 5), CFW_OK);
    CFW_TEST_ASSERT_EQ(buffer.size, 10);
    CFW_TEST_ASSERT(buffer.capacity >= 10);
    CFW_TEST_ASSERT(memcmp(buffer.data, "helloworld", 10) == 0);

    cfw_buffer_free(&buffer);
    return 0;
}

static int test_buffer_append_zero_bytes(void)
{
    cfw_buffer_t buffer;

    CFW_TEST_ASSERT_EQ(cfw_buffer_init(&buffer), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_buffer_append(&buffer, NULL, 0), CFW_OK);
    CFW_TEST_ASSERT_EQ(buffer.size, 0);
    CFW_TEST_ASSERT_EQ(buffer.capacity, 0);

    CFW_TEST_ASSERT_EQ(cfw_buffer_append(&buffer, "x", 0), CFW_OK);
    CFW_TEST_ASSERT_EQ(buffer.size, 0);
    cfw_buffer_free(&buffer);
    return 0;
}

static int test_buffer_append_rejects_null_data_with_size(void)
{
    cfw_buffer_t buffer;

    CFW_TEST_ASSERT_EQ(cfw_buffer_init(&buffer), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_buffer_append(&buffer, NULL, 1),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(buffer.size, 0);
    cfw_buffer_free(&buffer);
    return 0;
}

static int test_buffer_reserve_success(void)
{
    cfw_buffer_t buffer;

    CFW_TEST_ASSERT_EQ(cfw_buffer_init(&buffer), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_buffer_reserve(&buffer, 16), CFW_OK);
    CFW_TEST_ASSERT(buffer.data != NULL);
    CFW_TEST_ASSERT(buffer.capacity >= 16);
    CFW_TEST_ASSERT_EQ(buffer.size, 0);

    CFW_TEST_ASSERT_EQ(cfw_buffer_reserve(&buffer, 8), CFW_OK);
    CFW_TEST_ASSERT(buffer.capacity >= 16);
    cfw_buffer_free(&buffer);
    return 0;
}

static int test_buffer_reserve_rejects_invalid_state(void)
{
    uint8_t bytes[] = {1, 2, 3};
    cfw_buffer_t null_data = {NULL, 1, 1};
    cfw_buffer_t oversized = {bytes, 2, 1};

    CFW_TEST_ASSERT_EQ(cfw_buffer_reserve(&null_data, 2),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_buffer_append(&oversized, "x", 1),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* NULL and invalid buffers are rejected or ignored safely. */
    cfw_buffer_free(NULL);
    cfw_buffer_clear(NULL);
    cfw_buffer_clear(&oversized);
    {
        cfw_span_t span;
        CFW_TEST_ASSERT_EQ(cfw_buffer_span(&oversized, &span),
                           CFW_ERROR_INVALID_ARGUMENT);
        CFW_TEST_ASSERT_EQ(cfw_buffer_span(NULL, &span),
                           CFW_ERROR_INVALID_ARGUMENT);
        CFW_TEST_ASSERT_EQ(cfw_buffer_append(NULL, "x", 1),
                           CFW_ERROR_INVALID_ARGUMENT);
    }
    return 0;
}

static int test_buffer_append_overflow(void)
{
    cfw_buffer_t buffer;

    CFW_TEST_ASSERT_EQ(cfw_buffer_init(&buffer), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_buffer_append(&buffer, "x", 1), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_buffer_append(&buffer, "y", SIZE_MAX),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(buffer.size, 1);
    CFW_TEST_ASSERT(buffer.data != NULL);
    cfw_buffer_free(&buffer);
    return 0;
}

static int test_buffer_clear_keeps_capacity(void)
{
    cfw_buffer_t buffer;
    size_t capacity;

    CFW_TEST_ASSERT_EQ(cfw_buffer_init(&buffer), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_buffer_append(&buffer, "hello", 5), CFW_OK);
    capacity = buffer.capacity;

    cfw_buffer_clear(&buffer);
    CFW_TEST_ASSERT_EQ(buffer.size, 0);
    CFW_TEST_ASSERT_EQ(buffer.capacity, capacity);
    CFW_TEST_ASSERT(buffer.data != NULL);

    CFW_TEST_ASSERT_EQ(cfw_buffer_append(&buffer, "abc", 3), CFW_OK);
    CFW_TEST_ASSERT_EQ(buffer.size, 3);
    CFW_TEST_ASSERT(memcmp(buffer.data, "abc", 3) == 0);
    cfw_buffer_free(&buffer);
    return 0;
}

static int test_buffer_free_resets_and_is_idempotent(void)
{
    cfw_buffer_t buffer;

    CFW_TEST_ASSERT_EQ(cfw_buffer_init(&buffer), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_buffer_append(&buffer, "hello", 5), CFW_OK);

    cfw_buffer_free(&buffer);
    CFW_TEST_ASSERT(buffer.data == NULL);
    CFW_TEST_ASSERT_EQ(buffer.size, 0);
    CFW_TEST_ASSERT_EQ(buffer.capacity, 0);

    cfw_buffer_free(&buffer);
    cfw_buffer_clear(&buffer);
    return 0;
}

static int test_buffer_span_success(void)
{
    cfw_buffer_t buffer;
    cfw_span_t span;

    CFW_TEST_ASSERT_EQ(cfw_buffer_init(&buffer), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_buffer_append(&buffer, "abc", 3), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_buffer_span(&buffer, &span), CFW_OK);
    CFW_TEST_ASSERT(span.data == buffer.data);
    CFW_TEST_ASSERT_EQ(span.size, 3);
    CFW_TEST_ASSERT_EQ(span.data[0], 'a');

    cfw_buffer_free(&buffer);
    return 0;
}

static int test_buffer_span_empty(void)
{
    cfw_buffer_t buffer;
    cfw_span_t span;

    CFW_TEST_ASSERT_EQ(cfw_buffer_init(&buffer), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_buffer_span(&buffer, &span), CFW_OK);
    CFW_TEST_ASSERT(span.data == NULL);
    CFW_TEST_ASSERT_EQ(span.size, 0);
    cfw_buffer_free(&buffer);
    return 0;
}

const cfw_test_case cfw_test_buffer_cases[] = {
    {"buffer_init_success", test_buffer_init_success},
    {"buffer_init_rejects_null", test_buffer_init_rejects_null},
    {"buffer_append_success_and_growth", test_buffer_append_success_and_growth},
    {"buffer_append_zero_bytes", test_buffer_append_zero_bytes},
    {"buffer_append_rejects_null_data_with_size",
     test_buffer_append_rejects_null_data_with_size},
    {"buffer_reserve_success", test_buffer_reserve_success},
    {"buffer_reserve_rejects_invalid_state",
     test_buffer_reserve_rejects_invalid_state},
    {"buffer_append_overflow", test_buffer_append_overflow},
    {"buffer_clear_keeps_capacity", test_buffer_clear_keeps_capacity},
    {"buffer_free_resets_and_is_idempotent",
     test_buffer_free_resets_and_is_idempotent},
    {"buffer_span_success", test_buffer_span_success},
    {"buffer_span_empty", test_buffer_span_empty},
    {NULL, NULL},
};
