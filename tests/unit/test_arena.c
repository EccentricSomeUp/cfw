/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CFW_TEST_ARENA_ALIGN _Alignof(max_align_t)

static int test_arena_create_destroy(void)
{
    cfw_arena_t *arena;

    arena = cfw_arena_create(0);
    CFW_TEST_ASSERT(arena != NULL);
    cfw_arena_destroy(arena);

    arena = cfw_arena_create(4096);
    CFW_TEST_ASSERT(arena != NULL);
    cfw_arena_destroy(arena);

    cfw_arena_destroy(NULL);
    cfw_arena_reset(NULL);
    return 0;
}

static int test_arena_alloc_basic_and_alignment(void)
{
    cfw_arena_t *arena;
    void *p1;
    void *p2;
    char *bytes;

    arena = cfw_arena_create(256);
    CFW_TEST_ASSERT(arena != NULL);

    CFW_TEST_ASSERT_EQ(cfw_arena_alloc(arena, 100, &p1), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_arena_alloc(arena, 1, &p2), CFW_OK);
    CFW_TEST_ASSERT(((uintptr_t)p1 % CFW_TEST_ARENA_ALIGN) == 0);
    CFW_TEST_ASSERT(((uintptr_t)p2 % CFW_TEST_ARENA_ALIGN) == 0);
    CFW_TEST_ASSERT(p2 != p1);

    bytes = (char *)p1;
    memset(bytes, 0xAB, 100);
    CFW_TEST_ASSERT_EQ(bytes[0], (char)0xAB);
    CFW_TEST_ASSERT_EQ(bytes[99], (char)0xAB);

    cfw_arena_destroy(arena);
    return 0;
}

static int test_arena_alloc_zeroed(void)
{
    cfw_arena_t *arena;
    unsigned char *data;
    size_t i;

    arena = cfw_arena_create(128);
    CFW_TEST_ASSERT(arena != NULL);

    CFW_TEST_ASSERT_EQ(cfw_arena_alloc_zeroed(arena, 64, (void **)&data),
                       CFW_OK);
    for (i = 0; i < 64; i++) {
        CFW_TEST_ASSERT_EQ(data[i], 0);
    }

    cfw_arena_destroy(arena);
    return 0;
}

static int test_arena_reset_reuses_storage(void)
{
    cfw_arena_t *arena;
    void *before;
    void *after;

    arena = cfw_arena_create(128);
    CFW_TEST_ASSERT(arena != NULL);

    CFW_TEST_ASSERT_EQ(cfw_arena_alloc(arena, 64, &before), CFW_OK);
    cfw_arena_reset(arena);
    CFW_TEST_ASSERT_EQ(cfw_arena_alloc(arena, 64, &after), CFW_OK);
    CFW_TEST_ASSERT(before == after);

    cfw_arena_destroy(arena);
    return 0;
}

static int test_arena_oversized_request_gets_dedicated_block(void)
{
    cfw_arena_t *arena;
    void *big;
    void *small;

    arena = cfw_arena_create(64);
    CFW_TEST_ASSERT(arena != NULL);

    CFW_TEST_ASSERT_EQ(cfw_arena_alloc(arena, 100000, &big), CFW_OK);
    CFW_TEST_ASSERT(((uintptr_t)big % CFW_TEST_ARENA_ALIGN) == 0);
    CFW_TEST_ASSERT_EQ(cfw_arena_alloc(arena, 8, &small), CFW_OK);

    cfw_arena_destroy(arena);
    return 0;
}

static int test_arena_many_allocations(void)
{
    cfw_arena_t *arena;
    void *ptrs[512];
    size_t i;

    arena = cfw_arena_create(1024);
    CFW_TEST_ASSERT(arena != NULL);

    for (i = 0; i < 512; i++) {
        CFW_TEST_ASSERT_EQ(cfw_arena_alloc(arena, 24, &ptrs[i]), CFW_OK);
        CFW_TEST_ASSERT(((uintptr_t)ptrs[i] % CFW_TEST_ARENA_ALIGN) == 0);
    }
    /* All allocations must be distinct. */
    for (i = 0; i < 512; i++) {
        size_t j;
        for (j = i + 1; j < 512; j++) {
            CFW_TEST_ASSERT(ptrs[i] != ptrs[j]);
        }
    }

    cfw_arena_destroy(arena);
    return 0;
}

static int test_arena_invalid_arguments(void)
{
    cfw_arena_t *arena;
    void *out;

    arena = cfw_arena_create(64);
    CFW_TEST_ASSERT(arena != NULL);

    CFW_TEST_ASSERT_EQ(cfw_arena_alloc(NULL, 1, &out),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_arena_alloc(arena, 1, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_arena_alloc_zeroed(arena, 1, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* Sizes that cannot be alignment-padded must fail closed. */
    CFW_TEST_ASSERT_EQ(cfw_arena_alloc(arena, SIZE_MAX, &out),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(cfw_arena_alloc_zeroed(arena, SIZE_MAX, &out),
                       CFW_ERROR_OVERFLOW);

    cfw_arena_destroy(arena);
    return 0;
}

static int test_arena_alloc_zero_size(void)
{
    cfw_arena_t *arena;
    void *out = NULL;

    arena = cfw_arena_create(64);
    CFW_TEST_ASSERT(arena != NULL);

    CFW_TEST_ASSERT_EQ(cfw_arena_alloc(arena, 0, &out), CFW_OK);
    CFW_TEST_ASSERT(out != NULL);

    cfw_arena_destroy(arena);
    return 0;
}

const cfw_test_case cfw_test_arena_cases[] = {
    {"arena_create_destroy", test_arena_create_destroy},
    {"arena_alloc_basic_and_alignment", test_arena_alloc_basic_and_alignment},
    {"arena_alloc_zeroed", test_arena_alloc_zeroed},
    {"arena_reset_reuses_storage", test_arena_reset_reuses_storage},
    {"arena_oversized_request_gets_dedicated_block",
     test_arena_oversized_request_gets_dedicated_block},
    {"arena_many_allocations", test_arena_many_allocations},
    {"arena_invalid_arguments", test_arena_invalid_arguments},
    {"arena_alloc_zero_size", test_arena_alloc_zero_size},
    {NULL, NULL},
};
