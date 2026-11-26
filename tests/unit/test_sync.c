/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <stdlib.h>

#if CFW_PLATFORM_WINDOWS
    #include <windows.h>
#else
    #include <pthread.h>
    #include <time.h>
#endif

/* Portable one-shot thread helper for exercising the sync primitives. */

typedef void *(*cfw_test_thread_fn)(void *arg);

static void cfw_test_sleep_ms(int milliseconds)
{
#if CFW_PLATFORM_WINDOWS
    Sleep((DWORD)milliseconds);
#else
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

#if CFW_PLATFORM_WINDOWS
typedef HANDLE cfw_test_thread_t;

typedef struct cfw_test_thread_start {
    cfw_test_thread_fn fn;
    void *arg;
} cfw_test_thread_start_t;

static DWORD WINAPI cfw_test_thread_trampoline(LPVOID parameter)
{
    cfw_test_thread_start_t *start = (cfw_test_thread_start_t *)parameter;
    cfw_test_thread_fn fn = start->fn;
    void *arg = start->arg;

    fn(arg);
    free(start);
    return 0;
}

static int cfw_test_thread_create(cfw_test_thread_t *thread,
                                  cfw_test_thread_fn fn, void *arg)
{
    cfw_test_thread_start_t *start =
        (cfw_test_thread_start_t *)malloc(sizeof(*start));
    if (start == NULL) {
        return -1;
    }
    start->fn = fn;
    start->arg = arg;
    *thread = CreateThread(NULL, 0, cfw_test_thread_trampoline, start, 0, NULL);
    if (*thread == NULL) {
        free(start);
        return -1;
    }
    return 0;
}

static int cfw_test_thread_join(cfw_test_thread_t thread)
{
    return WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0 &&
                   CloseHandle(thread)
               ? 0
               : -1;
}
#else
typedef pthread_t cfw_test_thread_t;

static int cfw_test_thread_create(cfw_test_thread_t *thread,
                                  cfw_test_thread_fn fn, void *arg)
{
    return pthread_create(thread, NULL, fn, arg);
}

static int cfw_test_thread_join(cfw_test_thread_t thread)
{
    return pthread_join(thread, NULL);
}
#endif

/* --- mutex ------------------------------------------------------------ */

static int test_mutex_init_and_lock_cycle(void)
{
    cfw_mutex_t mutex;

    CFW_TEST_ASSERT_EQ(cfw_mutex_init(NULL), CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_mutex_lock(NULL), CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_mutex_try_lock(NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_mutex_unlock(NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_mutex_destroy(NULL), CFW_ERROR_INVALID_ARGUMENT);

    CFW_TEST_ASSERT_EQ(cfw_mutex_init(&mutex), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_mutex_lock(&mutex), CFW_OK);
    /* Non-recursive: a second lock attempt by the holder must not
     * succeed. try_lock reports contention instead of blocking. */
    CFW_TEST_ASSERT_EQ(cfw_mutex_try_lock(&mutex), CFW_ERROR_WOULDBLOCK);
    CFW_TEST_ASSERT_EQ(cfw_mutex_unlock(&mutex), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_mutex_try_lock(&mutex), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_mutex_unlock(&mutex), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_mutex_destroy(&mutex), CFW_OK);
    return 0;
}

typedef struct cfw_test_counter_ctx {
    cfw_mutex_t mutex;
    int value;
} cfw_test_counter_ctx_t;

static void *cfw_test_counter_worker(void *arg)
{
    cfw_test_counter_ctx_t *ctx = (cfw_test_counter_ctx_t *)arg;
    int i;

    for (i = 0; i < 500; i++) {
        if (cfw_mutex_lock(&ctx->mutex) != CFW_OK) {
            return (void *)(intptr_t)-1;
        }
        ctx->value++;
        if (cfw_mutex_unlock(&ctx->mutex) != CFW_OK) {
            return (void *)(intptr_t)-1;
        }
    }
    return NULL;
}

static int test_mutex_mutual_exclusion(void)
{
    cfw_test_counter_ctx_t ctx;
    cfw_test_thread_t threads[2];
    int i;

    memset(&ctx, 0, sizeof(ctx));
    ctx.value = 0;
    CFW_TEST_ASSERT_EQ(cfw_mutex_init(&ctx.mutex), CFW_OK);

    for (i = 0; i < 2; i++) {
        CFW_TEST_ASSERT_EQ(cfw_test_thread_create(&threads[i],
                                                  cfw_test_counter_worker,
                                                  &ctx),
                           0);
    }
    for (i = 0; i < 2; i++) {
        CFW_TEST_ASSERT_EQ(cfw_test_thread_join(threads[i]), 0);
    }
    CFW_TEST_ASSERT_EQ(ctx.value, 1000);
    CFW_TEST_ASSERT_EQ(cfw_mutex_destroy(&ctx.mutex), CFW_OK);
    return 0;
}

/* --- condition variable ---------------------------------------------- */

typedef struct cfw_test_cond_ctx {
    cfw_mutex_t mutex;
    cfw_cond_t cond;
    int ready;
    int observed;
    cfw_status_t wait_status;
} cfw_test_cond_ctx_t;

static void *cfw_test_cond_waiter(void *arg)
{
    cfw_test_cond_ctx_t *ctx = (cfw_test_cond_ctx_t *)arg;

    if (cfw_mutex_lock(&ctx->mutex) != CFW_OK) {
        return (void *)(intptr_t)-1;
    }
    /* Predicate loop: required because wakeups may be spurious. */
    while (!ctx->ready) {
        ctx->wait_status = cfw_cond_wait(&ctx->cond, &ctx->mutex, NULL);
        if (ctx->wait_status != CFW_OK) {
            break;
        }
    }
    ctx->observed = ctx->ready;
    if (cfw_mutex_unlock(&ctx->mutex) != CFW_OK) {
        return (void *)(intptr_t)-1;
    }
    return NULL;
}

static int test_cond_signal_wakeup(void)
{
    cfw_test_cond_ctx_t ctx;
    cfw_test_thread_t waiter;

    memset(&ctx, 0, sizeof(ctx));
    ctx.wait_status = CFW_ERROR_INTERNAL;
    CFW_TEST_ASSERT_EQ(cfw_mutex_init(&ctx.mutex), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_cond_init(&ctx.cond), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_test_thread_create(&waiter, cfw_test_cond_waiter,
                                              &ctx),
                       0);

    /* Give the waiter time to block inside cfw_cond_wait, then publish
     * and signal. */
    cfw_test_sleep_ms(100);

    CFW_TEST_ASSERT_EQ(cfw_mutex_lock(&ctx.mutex), CFW_OK);
    ctx.ready = 1;
    CFW_TEST_ASSERT_EQ(cfw_cond_signal(&ctx.cond), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_mutex_unlock(&ctx.mutex), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_test_thread_join(waiter), 0);
    CFW_TEST_ASSERT_EQ(ctx.wait_status, CFW_OK);
    CFW_TEST_ASSERT_EQ(ctx.observed, 1);

    CFW_TEST_ASSERT_EQ(cfw_cond_destroy(&ctx.cond), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_mutex_destroy(&ctx.mutex), CFW_OK);
    return 0;
}

typedef struct cfw_test_broadcast_ctx {
    cfw_mutex_t mutex;
    cfw_cond_t cond;
    int ready;
    int arrived;
} cfw_test_broadcast_ctx_t;

static void *cfw_test_broadcast_waiter(void *arg)
{
    cfw_test_broadcast_ctx_t *ctx = (cfw_test_broadcast_ctx_t *)arg;

    if (cfw_mutex_lock(&ctx->mutex) != CFW_OK) {
        return (void *)(intptr_t)-1;
    }
    ctx->arrived++;
    while (!ctx->ready) {
        if (cfw_cond_wait(&ctx->cond, &ctx->mutex, NULL) != CFW_OK) {
            break;
        }
    }
    if (cfw_mutex_unlock(&ctx->mutex) != CFW_OK) {
        return (void *)(intptr_t)-1;
    }
    return NULL;
}

static int test_cond_broadcast(void)
{
    cfw_test_broadcast_ctx_t ctx;
    cfw_test_thread_t waiters[2];
    int i;
    int arrived;

    memset(&ctx, 0, sizeof(ctx));
    CFW_TEST_ASSERT_EQ(cfw_mutex_init(&ctx.mutex), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_cond_init(&ctx.cond), CFW_OK);

    for (i = 0; i < 2; i++) {
        CFW_TEST_ASSERT_EQ(cfw_test_thread_create(
                               &waiters[i], cfw_test_broadcast_waiter, &ctx),
                           0);
    }

    /* Wait until both workers are inside the predicate loop. `arrived`
     * is mutex-protected, so read it under the lock. */
    for (;;) {
        CFW_TEST_ASSERT_EQ(cfw_mutex_lock(&ctx.mutex), CFW_OK);
        arrived = ctx.arrived;
        CFW_TEST_ASSERT_EQ(cfw_mutex_unlock(&ctx.mutex), CFW_OK);
        if (arrived >= 2) {
            break;
        }
        cfw_test_sleep_ms(1);
    }

    CFW_TEST_ASSERT_EQ(cfw_mutex_lock(&ctx.mutex), CFW_OK);
    ctx.ready = 1;
    CFW_TEST_ASSERT_EQ(cfw_cond_broadcast(&ctx.cond), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_mutex_unlock(&ctx.mutex), CFW_OK);

    for (i = 0; i < 2; i++) {
        CFW_TEST_ASSERT_EQ(cfw_test_thread_join(waiters[i]), 0);
    }

    CFW_TEST_ASSERT_EQ(cfw_mutex_lock(&ctx.mutex), CFW_OK);
    CFW_TEST_ASSERT_EQ(ctx.arrived, 2);
    CFW_TEST_ASSERT_EQ(ctx.ready, 1);
    CFW_TEST_ASSERT_EQ(cfw_mutex_unlock(&ctx.mutex), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_cond_destroy(&ctx.cond), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_mutex_destroy(&ctx.mutex), CFW_OK);
    return 0;
}

static void *cfw_test_cond_timeout_waiter(void *arg)
{
    cfw_test_cond_ctx_t *ctx = (cfw_test_cond_ctx_t *)arg;
    cfw_timepoint_t deadline;

    if (cfw_mutex_lock(&ctx->mutex) != CFW_OK) {
        return (void *)(intptr_t)-1;
    }
    /* Nobody ever signals; the wait must expire. */
    if (cfw_deadline_from_now(&deadline,
                              (cfw_duration_t){.nanoseconds = 50000000}) !=
        CFW_OK) {
        ctx->wait_status = CFW_ERROR_INTERNAL;
    } else {
        ctx->wait_status = cfw_cond_wait(&ctx->cond, &ctx->mutex, &deadline);
    }
    if (cfw_mutex_unlock(&ctx->mutex) != CFW_OK) {
        return (void *)(intptr_t)-1;
    }
    return NULL;
}

static int test_cond_wait_timeout(void)
{
    cfw_test_cond_ctx_t ctx;
    cfw_test_thread_t waiter;
    cfw_timepoint_t start;
    cfw_timepoint_t end;
    cfw_duration_t elapsed;
    int64_t elapsed_ms;

    memset(&ctx, 0, sizeof(ctx));
    ctx.wait_status = CFW_ERROR_INTERNAL;
    CFW_TEST_ASSERT_EQ(cfw_mutex_init(&ctx.mutex), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_cond_init(&ctx.cond), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_time_now(&start), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_thread_create(&waiter, cfw_test_cond_timeout_waiter,
                                              &ctx),
                       0);
    CFW_TEST_ASSERT_EQ(cfw_test_thread_join(waiter), 0);
    CFW_TEST_ASSERT_EQ(cfw_time_now(&end), CFW_OK);

    CFW_TEST_ASSERT_EQ(ctx.wait_status, CFW_ERROR_TIMEOUT);

    /* The 50 ms deadline must not fire meaningfully early. */
    CFW_TEST_ASSERT_EQ(cfw_timepoint_diff(&end, &start, &elapsed), CFW_OK);
    elapsed_ms = elapsed.nanoseconds / 1000000;
    CFW_TEST_ASSERT_TRUE(elapsed_ms >= 30);
    CFW_TEST_ASSERT_TRUE(elapsed_ms < 10000);

    CFW_TEST_ASSERT_EQ(cfw_cond_destroy(&ctx.cond), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_mutex_destroy(&ctx.mutex), CFW_OK);
    return 0;
}

static int test_cond_wait_expired_deadline(void)
{
    cfw_mutex_t mutex;
    cfw_cond_t cond;
    cfw_timepoint_t deadline;

    CFW_TEST_ASSERT_EQ(cfw_mutex_init(&mutex), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_cond_init(&cond), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_deadline_from_now(
                           &deadline,
                           (cfw_duration_t){.nanoseconds = -1000000}),
                       CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_mutex_lock(&mutex), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_cond_wait(&cond, &mutex, &deadline),
                       CFW_ERROR_TIMEOUT);
    /* Mutex must be held again on the timeout path. */
    CFW_TEST_ASSERT_EQ(cfw_mutex_unlock(&mutex), CFW_OK);

    /* NULL arguments are rejected. */
    CFW_TEST_ASSERT_EQ(cfw_cond_wait(NULL, &mutex, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_cond_wait(&cond, NULL, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_cond_signal(NULL), CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_cond_broadcast(NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_cond_init(NULL), CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_cond_destroy(NULL), CFW_ERROR_INVALID_ARGUMENT);

    CFW_TEST_ASSERT_EQ(cfw_cond_destroy(&cond), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_mutex_destroy(&mutex), CFW_OK);
    return 0;
}

const cfw_test_case cfw_test_sync_cases[] = {
    {"mutex_init_and_lock_cycle", test_mutex_init_and_lock_cycle},
    {"mutex_mutual_exclusion", test_mutex_mutual_exclusion},
    {"cond_signal_wakeup", test_cond_signal_wakeup},
    {"cond_broadcast", test_cond_broadcast},
    {"cond_wait_timeout", test_cond_wait_timeout},
    {"cond_wait_expired_deadline", test_cond_wait_expired_deadline},
    {NULL, NULL},
};
