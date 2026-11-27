/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/pollset.h>

#include <cfw/cfw.h>
#include <cfw/net.h>

#include "test_harness.h"
#include "test_loopback.h"

#include <string.h>

static int test_pollset_create_destroy(void)
{
    cfw_pollset_t *pollset = NULL;

    CFW_TEST_ASSERT_EQ(cfw_pollset_create(NULL), CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_pollset_create(&pollset), CFW_OK);
    CFW_TEST_ASSERT(pollset != NULL);
    cfw_pollset_destroy(pollset);
    cfw_pollset_destroy(NULL);
    return 0;
}

static int test_pollset_wait_reports_readable(void)
{
    cfw_pollset_t *pollset;
    cfw_poll_event_t events[4];
    cfw_timepoint_t deadline;
    cfw_duration_t wait_time;
    size_t count = 0;
    int token = 42;
    cfw_fd_t pair[2];
    intptr_t read_fd;
    intptr_t write_fd;
    char byte = 'x';
    size_t sent = 0;

    CFW_TEST_ASSERT_EQ(cfw_test_socketpair(pair), CFW_OK);
    read_fd = cfw_fd_get(&pair[0]);
    write_fd = cfw_fd_get(&pair[1]);
    CFW_TEST_ASSERT(read_fd >= 0 && write_fd >= 0);

    CFW_TEST_ASSERT_EQ(cfw_pollset_create(&pollset), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_pollset_add(pollset, read_fd, CFW_POLL_IN,
                                       &token),
                       CFW_OK);

    /* Duplicate add must fail. */
    CFW_TEST_ASSERT_EQ(cfw_pollset_add(pollset, read_fd, CFW_POLL_IN,
                                       NULL),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* Nothing to read yet: deadline should produce a timeout. */
    CFW_TEST_ASSERT_EQ(cfw_duration_from_millis(50, &wait_time), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_deadline_from_now(&deadline, wait_time), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_pollset_wait(pollset, &deadline, events, 4,
                                        &count),
                       CFW_ERROR_TIMEOUT);
    CFW_TEST_ASSERT_EQ(count, 0);

    /* Make the read end readable. */
    CFW_TEST_ASSERT_EQ(cfw_net_send(&pair[1], &byte, 1, &sent), CFW_OK);
    CFW_TEST_ASSERT_EQ(sent, 1);

    CFW_TEST_ASSERT_EQ(cfw_duration_from_seconds(2, &wait_time), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_deadline_from_now(&deadline, wait_time), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_pollset_wait(pollset, &deadline, events, 4,
                                        &count),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 1);
    CFW_TEST_ASSERT_EQ(events[0].handle, read_fd);
    CFW_TEST_ASSERT_TRUE((events[0].events & CFW_POLL_IN) != 0);
    CFW_TEST_ASSERT_EQ(events[0].token, &token);

    /* Remove and confirm the set is empty for a subsequent wait. */
    CFW_TEST_ASSERT_EQ(cfw_pollset_remove(pollset, read_fd), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_pollset_remove(pollset, read_fd),
                       CFW_ERROR_INVALID_ARGUMENT);

    cfw_pollset_destroy(pollset);
    CFW_TEST_ASSERT_EQ(cfw_fd_close(&pair[0]), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_fd_close(&pair[1]), CFW_OK);
    return 0;
}

static int test_pollset_modify_and_remove(void)
{
    cfw_pollset_t *pollset;
    cfw_poll_event_t events[4];
    cfw_timepoint_t deadline;
    cfw_duration_t wait_time;
    size_t count = 0;
    int token_a = 1;
    int token_b = 2;
    cfw_fd_t pair[2];
    intptr_t read_fd;
    intptr_t write_fd;
    char byte = 'x';
    size_t sent = 0;

    CFW_TEST_ASSERT_EQ(cfw_test_socketpair(pair), CFW_OK);
    read_fd = cfw_fd_get(&pair[0]);
    write_fd = cfw_fd_get(&pair[1]);
    CFW_TEST_ASSERT(read_fd >= 0 && write_fd >= 0);

    CFW_TEST_ASSERT_EQ(cfw_pollset_create(&pollset), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_pollset_add(pollset, read_fd, CFW_POLL_IN,
                                       &token_a),
                       CFW_OK);

    /* modify on missing handle fails */
    CFW_TEST_ASSERT_EQ(cfw_pollset_modify(pollset, write_fd, CFW_POLL_IN,
                                          &token_b),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* Disable all interest while data is pending: the wait must time
     * out instead of reporting the stale IN registration. */
    CFW_TEST_ASSERT_EQ(cfw_net_send(&pair[1], &byte, 1, &sent), CFW_OK);
    CFW_TEST_ASSERT_EQ(sent, 1);
    CFW_TEST_ASSERT_EQ(cfw_pollset_modify(pollset, read_fd, 0,
                                          &token_b),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_duration_from_millis(50, &wait_time), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_deadline_from_now(&deadline, wait_time), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_pollset_wait(pollset, &deadline, events, 4,
                                        &count),
                       CFW_ERROR_TIMEOUT);
    CFW_TEST_ASSERT_EQ(count, 0);

    /* Re-enable IN with the new token and confirm the pending byte is
     * reported exactly once under the updated registration. */
    CFW_TEST_ASSERT_EQ(cfw_pollset_modify(pollset, read_fd, CFW_POLL_IN,
                                          &token_b),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_duration_from_seconds(2, &wait_time), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_deadline_from_now(&deadline, wait_time), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_pollset_wait(pollset, &deadline, events, 4,
                                        &count),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(count, 1);
    CFW_TEST_ASSERT_EQ(events[0].handle, read_fd);
    CFW_TEST_ASSERT_TRUE((events[0].events & CFW_POLL_IN) != 0);
    CFW_TEST_ASSERT_EQ(events[0].token, &token_b);

    /* Removal must also suppress the still-pending readable state. */
    CFW_TEST_ASSERT_EQ(cfw_pollset_remove(pollset, read_fd), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_pollset_remove(pollset, read_fd),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_duration_from_millis(50, &wait_time), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_deadline_from_now(&deadline, wait_time), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_pollset_wait(pollset, &deadline, events, 4,
                                        &count),
                       CFW_ERROR_TIMEOUT);

    cfw_pollset_destroy(pollset);
    CFW_TEST_ASSERT_EQ(cfw_fd_close(&pair[0]), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_fd_close(&pair[1]), CFW_OK);
    return 0;
}

static int test_pollset_invalid_arguments(void)
{
    cfw_pollset_t *pollset;
    cfw_poll_event_t events[1];
    cfw_timepoint_t deadline;
    cfw_duration_t wait_time;
    size_t count;

    CFW_TEST_ASSERT_EQ(cfw_pollset_create(&pollset), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_duration_from_seconds(1, &wait_time), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_deadline_from_now(&deadline, wait_time), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_pollset_add(NULL, 0, CFW_POLL_IN, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_pollset_add(pollset, -1, CFW_POLL_IN, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_pollset_add(pollset, 0, 0xFFFFFFFFu, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);

    CFW_TEST_ASSERT_EQ(cfw_pollset_wait(NULL, &deadline, events, 1,
                                        &count),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_pollset_wait(pollset, &deadline, NULL, 1,
                                        &count),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_pollset_wait(pollset, &deadline, events, 0,
                                        &count),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_pollset_wait(pollset, &deadline, events, 1,
                                        NULL),
                       CFW_ERROR_INVALID_ARGUMENT);

    cfw_pollset_destroy(pollset);
    return 0;
}

const cfw_test_case cfw_test_pollset_cases[] = {
    {"pollset_create_destroy", test_pollset_create_destroy},
    {"pollset_wait_reports_readable", test_pollset_wait_reports_readable},
    {"pollset_modify_and_remove", test_pollset_modify_and_remove},
    {"pollset_invalid_arguments", test_pollset_invalid_arguments},
    {NULL, NULL},
};
