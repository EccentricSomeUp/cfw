/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

static int test_known_status_strings(void)
{
    CFW_TEST_ASSERT_STR_EQ(cfw_status_string(CFW_OK), "ok");
    CFW_TEST_ASSERT_STR_EQ(cfw_status_string(CFW_ERROR_INVALID_ARGUMENT),
                           "invalid_argument");
    CFW_TEST_ASSERT_STR_EQ(cfw_status_string(CFW_ERROR_NOMEM), "nomem");
    CFW_TEST_ASSERT_STR_EQ(cfw_status_string(CFW_ERROR_IO), "io");
    CFW_TEST_ASSERT_STR_EQ(cfw_status_string(CFW_ERROR_TIMEOUT), "timeout");
    CFW_TEST_ASSERT_STR_EQ(cfw_status_string(CFW_ERROR_UNAVAILABLE),
                           "unavailable");
    CFW_TEST_ASSERT_STR_EQ(cfw_status_string(CFW_ERROR_INTERNAL), "internal");
    CFW_TEST_ASSERT_STR_EQ(cfw_status_string(CFW_ERROR_OVERFLOW), "overflow");
    return 0;
}

static int test_unknown_status_string(void)
{
    CFW_TEST_ASSERT_STR_EQ(cfw_status_string((cfw_status_t)999), "unknown");
    return 0;
}

const cfw_test_case cfw_test_status_cases[] = {
    {"known_status_strings", test_known_status_strings},
    {"unknown_status_string", test_unknown_status_string},
    {NULL, NULL},
};
