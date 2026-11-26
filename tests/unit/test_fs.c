/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <stdio.h>
#include <string.h>

static int test_fs_file_size_success(void)
{
    const char *path = "cfw_test_fs_tmp.bin";
    FILE *file;
    uint64_t size;
    const char payload[] = "cfw-fs-test-payload";

    file = fopen(path, "wb");
    CFW_TEST_ASSERT(file != NULL);
    CFW_TEST_ASSERT_EQ(fwrite(payload, 1, sizeof(payload) - 1, file),
                       sizeof(payload) - 1);
    CFW_TEST_ASSERT_EQ(fclose(file), 0);

    CFW_TEST_ASSERT_EQ(cfw_file_size(path, &size), CFW_OK);
    CFW_TEST_ASSERT_EQ(size, sizeof(payload) - 1);

    CFW_TEST_ASSERT_EQ(remove(path), 0);
    return 0;
}

static int test_fs_file_size_errors(void)
{
    uint64_t size;

    CFW_TEST_ASSERT_EQ(cfw_file_size(NULL, &size),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_size("definitely/not/here", &size),
                       CFW_ERROR_IO);

    /* A directory is not a regular file. */
    CFW_TEST_ASSERT_EQ(cfw_file_size(".", &size),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_fs_is_directory(void)
{
    int is_dir;

    CFW_TEST_ASSERT_EQ(cfw_file_is_directory(NULL, &is_dir),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_is_directory(".", NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_is_directory("definitely/not/here", &is_dir),
                       CFW_ERROR_IO);

    CFW_TEST_ASSERT_EQ(cfw_file_is_directory(".", &is_dir), CFW_OK);
    CFW_TEST_ASSERT_TRUE(is_dir);

    {
        const char *path = "cfw_test_fs_tmp.txt";
        FILE *file = fopen(path, "wb");
        CFW_TEST_ASSERT(file != NULL);
        CFW_TEST_ASSERT_EQ(fclose(file), 0);

        CFW_TEST_ASSERT_EQ(cfw_file_is_directory(path, &is_dir), CFW_OK);
        CFW_TEST_ASSERT_FALSE(is_dir);
        CFW_TEST_ASSERT_EQ(remove(path), 0);
    }
    return 0;
}

const cfw_test_case cfw_test_fs_cases[] = {
    {"fs_file_size_success", test_fs_file_size_success},
    {"fs_file_size_errors", test_fs_file_size_errors},
    {"fs_is_directory", test_fs_is_directory},
    {NULL, NULL},
};
