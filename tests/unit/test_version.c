/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

static int test_version_fields(void)
{
    cfw_version_t version = cfw_get_version();

    CFW_TEST_ASSERT_EQ(version.major, CFW_VERSION_MAJOR);
    CFW_TEST_ASSERT_EQ(version.minor, CFW_VERSION_MINOR);
    CFW_TEST_ASSERT_EQ(version.patch, CFW_VERSION_PATCH);
    return 0;
}

static int test_version_string(void)
{
    const char *version = cfw_get_version_string();

    CFW_TEST_ASSERT(version != NULL);
    CFW_TEST_ASSERT_STR_EQ(version, CFW_VERSION_STRING);
    return 0;
}

static int test_abi_version(void)
{
    CFW_TEST_ASSERT(cfw_get_abi_version() >= 1);
    return 0;
}

const cfw_test_case cfw_test_version_cases[] = {
    {"version_fields", test_version_fields},
    {"version_string", test_version_string},
    {"abi_version", test_abi_version},
    {NULL, NULL},
};
