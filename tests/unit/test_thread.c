/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <string.h>

static int cfw_test_return_value(void *arg)
{
    return (int)(intptr_t)arg;
}

static int test_thread_create_join_exit_code(void)
{
    cfw_thread_t thread;
    int exit_code = -1;

    CFW_TEST_ASSERT_EQ(cfw_thread_create(NULL, cfw_test_return_value,
                                         (void *)(intptr_t)42),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_thread_create(&thread, NULL, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_thread_join(NULL, &exit_code),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_thread_detach(NULL),
                       CFW_ERROR_INVALID_ARGUMENT);

    CFW_TEST_ASSERT_EQ(cfw_thread_create(&thread, cfw_test_return_value,
                                         (void *)(intptr_t)42),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_thread_join(&thread, &exit_code), CFW_OK);
    CFW_TEST_ASSERT_EQ(exit_code, 42);

    /* Negative exit codes round-trip too. */
    CFW_TEST_ASSERT_EQ(cfw_thread_create(&thread, cfw_test_return_value,
                                         (void *)(intptr_t)-7),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_thread_join(&thread, &exit_code), CFW_OK);
    CFW_TEST_ASSERT_EQ(exit_code, -7);

    /* join with NULL exit_code is allowed. */
    CFW_TEST_ASSERT_EQ(cfw_thread_create(&thread, cfw_test_return_value,
                                         (void *)(intptr_t)1),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_thread_join(&thread, NULL), CFW_OK);
    return 0;
}

typedef struct cfw_test_detach_ctx {
    cfw_mutex_t mutex;
    cfw_cond_t cond;
    int done;
} cfw_test_detach_ctx_t;

static int cfw_test_detach_worker(void *arg)
{
    cfw_test_detach_ctx_t *ctx = (cfw_test_detach_ctx_t *)arg;

    if (cfw_mutex_lock(&ctx->mutex) != CFW_OK) {
        return -1;
    }
    ctx->done = 1;
    if (cfw_cond_signal(&ctx->cond) != CFW_OK) {
        cfw_mutex_unlock(&ctx->mutex);
        return -1;
    }
    if (cfw_mutex_unlock(&ctx->mutex) != CFW_OK) {
        return -1;
    }
    return 0;
}

static int test_thread_detach_runs_to_completion(void)
{
    cfw_test_detach_ctx_t ctx;
    cfw_thread_t thread;
    cfw_timepoint_t deadline;
    int expired = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.done = 0;
    CFW_TEST_ASSERT_EQ(cfw_mutex_init(&ctx.mutex), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_cond_init(&ctx.cond), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_thread_create(&thread, cfw_test_detach_worker,
                                         &ctx),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_thread_detach(&thread), CFW_OK);

    /* The detached thread must still run and signal. */
    CFW_TEST_ASSERT_EQ(cfw_deadline_from_now(&deadline,
                                              (cfw_duration_t){
                                                  .nanoseconds =
                                                      5000000000LL}),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_mutex_lock(&ctx.mutex), CFW_OK);
    while (!ctx.done) {
        cfw_status_t wait_status =
            cfw_cond_wait(&ctx.cond, &ctx.mutex, &deadline);
        CFW_TEST_ASSERT_TRUE(wait_status == CFW_OK ||
                             wait_status == CFW_ERROR_TIMEOUT);
        CFW_TEST_ASSERT_EQ(cfw_deadline_expired(&deadline, &expired),
                           CFW_OK);
        if (expired || wait_status == CFW_ERROR_TIMEOUT) {
            cfw_mutex_unlock(&ctx.mutex);
            return 1;
        }
    }
    CFW_TEST_ASSERT_EQ(ctx.done, 1);
    CFW_TEST_ASSERT_EQ(cfw_mutex_unlock(&ctx.mutex), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_cond_destroy(&ctx.cond), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_mutex_destroy(&ctx.mutex), CFW_OK);
    return 0;
}

typedef struct cfw_test_pool_ctx {
    cfw_mutex_t mutex;
    int value;
} cfw_test_pool_ctx_t;

static int cfw_test_pool_worker(void *arg)
{
    cfw_test_pool_ctx_t *ctx = (cfw_test_pool_ctx_t *)arg;
    int i;

    for (i = 0; i < 500; i++) {
        if (cfw_mutex_lock(&ctx->mutex) != CFW_OK) {
            return -1;
        }
        ctx->value++;
        if (cfw_mutex_unlock(&ctx->mutex) != CFW_OK) {
            return -1;
        }
    }
    return 0;
}

static int test_thread_joined_workers(void)
{
    cfw_test_pool_ctx_t ctx;
    cfw_thread_t threads[4];
    int exit_code;
    int i;

    memset(&ctx, 0, sizeof(ctx));
    ctx.value = 0;
    CFW_TEST_ASSERT_EQ(cfw_mutex_init(&ctx.mutex), CFW_OK);

    for (i = 0; i < 4; i++) {
        CFW_TEST_ASSERT_EQ(cfw_thread_create(&threads[i], cfw_test_pool_worker,
                                             &ctx),
                           CFW_OK);
    }
    for (i = 0; i < 4; i++) {
        CFW_TEST_ASSERT_EQ(cfw_thread_join(&threads[i], &exit_code), CFW_OK);
        CFW_TEST_ASSERT_EQ(exit_code, 0);
    }
    CFW_TEST_ASSERT_EQ(ctx.value, 2000);
    CFW_TEST_ASSERT_EQ(cfw_mutex_destroy(&ctx.mutex), CFW_OK);
    return 0;
}

const cfw_test_case cfw_test_thread_cases[] = {
    {"thread_create_join_exit_code", test_thread_create_join_exit_code},
    {"thread_detach_runs_to_completion", test_thread_detach_runs_to_completion},
    {"thread_joined_workers", test_thread_joined_workers},
    {NULL, NULL},
};
