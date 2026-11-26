/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <stdint.h>

static int test_platform_detect_consistent(void)
{
    cfw_platform_info_t info = cfw_platform_detect();

    CFW_TEST_ASSERT(info.pointer_bits == 32 || info.pointer_bits == 64);
    CFW_TEST_ASSERT(info.endianness != CFW_ENDIAN_UNKNOWN);

    /* Compile-time environment cross-checks. */
#if defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
    CFW_TEST_ASSERT_EQ(info.arch, CFW_ARCH_X86_64);
#elif defined(__aarch64__) || defined(_M_ARM64)
    CFW_TEST_ASSERT_EQ(info.arch, CFW_ARCH_AARCH64);
#endif
#if CFW_PLATFORM_WINDOWS
    CFW_TEST_ASSERT_EQ(info.os, CFW_OS_WINDOWS);
#elif CFW_PLATFORM_MACOS
    CFW_TEST_ASSERT_EQ(info.os, CFW_OS_MACOS);
#elif CFW_PLATFORM_LINUX
    CFW_TEST_ASSERT_EQ(info.os, CFW_OS_LINUX);
#endif
#if defined(SIZE_MAX) && SIZE_MAX > UINT32_MAX
    CFW_TEST_ASSERT_EQ(info.pointer_bits, 64);
#else
    CFW_TEST_ASSERT_EQ(info.pointer_bits, 32);
#endif
    return 0;
}

static int test_platform_names(void)
{
    CFW_TEST_ASSERT_STR_EQ(cfw_os_name(CFW_OS_LINUX), "linux");
    CFW_TEST_ASSERT_STR_EQ(cfw_os_name(CFW_OS_WINDOWS), "windows");
    CFW_TEST_ASSERT_STR_EQ(cfw_os_name(CFW_OS_UNKNOWN), "unknown");
    CFW_TEST_ASSERT_STR_EQ(cfw_os_name((cfw_os_t)999), "unknown");

    CFW_TEST_ASSERT_STR_EQ(cfw_arch_name(CFW_ARCH_X86_64), "x86_64");
    CFW_TEST_ASSERT_STR_EQ(cfw_arch_name(CFW_ARCH_AARCH64), "aarch64");
    CFW_TEST_ASSERT_STR_EQ(cfw_arch_name((cfw_arch_t)999), "unknown");

    CFW_TEST_ASSERT_STR_EQ(cfw_endian_name(CFW_ENDIAN_LITTLE), "little");
    CFW_TEST_ASSERT_STR_EQ(cfw_endian_name(CFW_ENDIAN_BIG), "big");
    CFW_TEST_ASSERT_STR_EQ(cfw_endian_name((cfw_endian_t)999), "unknown");
    return 0;
}

static int test_platform_page_size(void)
{
    size_t page_size = 0;

    CFW_TEST_ASSERT_EQ(cfw_platform_page_size(NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_platform_page_size(&page_size), CFW_OK);
    CFW_TEST_ASSERT(page_size >= 512);
    CFW_TEST_ASSERT((page_size & (page_size - 1)) == 0);
    return 0;
}

const cfw_test_case cfw_test_platform_cases[] = {
    {"platform_detect_consistent", test_platform_detect_consistent},
    {"platform_names", test_platform_names},
    {"platform_page_size", test_platform_page_size},
    {NULL, NULL},
};
