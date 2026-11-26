/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#if !defined(_WIN32)
    #include <unistd.h>
#endif

static int test_fd_init_and_valid(void)
{
    cfw_fd_t fd;

    cfw_fd_init(&fd);
    CFW_TEST_ASSERT_FALSE(cfw_fd_valid(&fd));
    CFW_TEST_ASSERT_EQ(cfw_fd_get(&fd), (intptr_t)-1);

    CFW_TEST_ASSERT_FALSE(cfw_fd_valid(NULL));
    CFW_TEST_ASSERT_EQ(cfw_fd_get(NULL), (intptr_t)-1);
    CFW_TEST_ASSERT_EQ(cfw_fd_release(NULL), (intptr_t)-1);
    CFW_TEST_ASSERT_EQ(cfw_fd_close(NULL), CFW_ERROR_INVALID_ARGUMENT);
    cfw_fd_init(NULL); /* must be a safe no-op */
    return 0;
}

static int test_fd_close_empty_is_ok(void)
{
    cfw_fd_t fd;

    cfw_fd_init(&fd);
    CFW_TEST_ASSERT_EQ(cfw_fd_close(&fd), CFW_OK);
    CFW_TEST_ASSERT_FALSE(cfw_fd_valid(&fd));
    return 0;
}

#if !defined(_WIN32)
static int test_fd_close_real_descriptor(void)
{
    int fds[2];
    cfw_fd_t read_end;
    cfw_fd_t write_end;

    CFW_TEST_ASSERT_EQ(pipe(fds), 0);

    cfw_fd_init(&read_end);
    cfw_fd_init(&write_end);
    read_end.handle = fds[0];
    write_end.handle = fds[1];

    CFW_TEST_ASSERT_TRUE(cfw_fd_valid(&read_end));
    CFW_TEST_ASSERT_EQ(cfw_fd_get(&read_end), (intptr_t)fds[0]);

    CFW_TEST_ASSERT_EQ(cfw_fd_close(&read_end), CFW_OK);
    CFW_TEST_ASSERT_FALSE(cfw_fd_valid(&read_end));

    /* Closing again is a safe no-op. */
    CFW_TEST_ASSERT_EQ(cfw_fd_close(&read_end), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_fd_close(&write_end), CFW_OK);
    return 0;
}

static int test_fd_release_transfers_ownership(void)
{
    int fds[2];
    cfw_fd_t fd;
    intptr_t raw;

    CFW_TEST_ASSERT_EQ(pipe(fds), 0);

    cfw_fd_init(&fd);
    fd.handle = fds[0];

    raw = cfw_fd_release(&fd);
    CFW_TEST_ASSERT_EQ(raw, (intptr_t)fds[0]);
    CFW_TEST_ASSERT_FALSE(cfw_fd_valid(&fd));

    /* Caller now owns the raw descriptor. */
    CFW_TEST_ASSERT_EQ(close((int)raw), 0);
    CFW_TEST_ASSERT_EQ(close(fds[1]), 0);
    return 0;
}
#endif

const cfw_test_case cfw_test_fd_cases[] = {
    {"fd_init_and_valid", test_fd_init_and_valid},
    {"fd_close_empty_is_ok", test_fd_close_empty_is_ok},
#if !defined(_WIN32)
    {"fd_close_real_descriptor", test_fd_close_real_descriptor},
    {"fd_release_transfers_ownership", test_fd_release_transfers_ownership},
#endif
    {NULL, NULL},
};
