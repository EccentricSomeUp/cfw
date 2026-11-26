/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <string.h>

static int test_uri_decode_basic(void)
{
    char dst[64];
    size_t out_len = 999;

    CFW_TEST_ASSERT_EQ(cfw_uri_decode("hello", 5, dst, sizeof(dst),
                                      &out_len, 0),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(out_len, 5);
    CFW_TEST_ASSERT_TRUE(memcmp(dst, "hello", 6) == 0);

    CFW_TEST_ASSERT_EQ(cfw_uri_decode("a%20b%21c", 9, dst, sizeof(dst),
                                      &out_len, 0),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(out_len, 5);
    CFW_TEST_ASSERT_TRUE(memcmp(dst, "a b!c", 6) == 0);

    /* Lowercase hex accepted. */
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("%2f%2F", 6, dst, sizeof(dst),
                                      &out_len, 0),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(out_len, 2);
    CFW_TEST_ASSERT_TRUE(memcmp(dst, "//", 3) == 0);

    /* Empty input. */
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("", 0, dst, sizeof(dst),
                                      &out_len, 0),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(out_len, 0);
    CFW_TEST_ASSERT_EQ(dst[0], '\0');
    return 0;
}

static int test_uri_decode_plus_flag(void)
{
    char dst[64];
    size_t out_len = 0;

    /* Without the flag '+' stays literal. */
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("a+b", 3, dst, sizeof(dst),
                                      &out_len, 0),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(memcmp(dst, "a+b", 4) == 0);

    CFW_TEST_ASSERT_EQ(cfw_uri_decode("a+b", 3, dst, sizeof(dst),
                                      &out_len,
                                      CFW_URI_DECODE_PLUS_AS_SPACE),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(memcmp(dst, "a b", 4) == 0);
    return 0;
}

static int test_uri_decode_reject_flags(void)
{
    char dst[64];
    size_t out_len = 0;

    /* Decoded NUL rejected with the flag, allowed without. */
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("a%00b", 5, dst, sizeof(dst),
                                      &out_len,
                                      CFW_URI_DECODE_REJECT_NUL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("a%00b", 5, dst, sizeof(dst),
                                      &out_len, 0),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(out_len, 3);

    /* Backslash rejected wherever it appears — literal or decoded. */
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("a\\b", 3, dst, sizeof(dst),
                                      &out_len,
                                      CFW_URI_DECODE_REJECT_BACKSLASH),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("a%5Cb", 5, dst, sizeof(dst),
                                      &out_len,
                                      CFW_URI_DECODE_REJECT_BACKSLASH),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("a%5Cb", 5, dst, sizeof(dst),
                                      &out_len, 0),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(out_len, 3);

    /* Literal NUL in the source is rejected by REJECT_NUL. */
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("a\0b", 3, dst, sizeof(dst),
                                      &out_len,
                                      CFW_URI_DECODE_REJECT_NUL),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_uri_decode_malformed(void)
{
    char dst[64];
    size_t out_len = 0;

    CFW_TEST_ASSERT_EQ(cfw_uri_decode("%", 1, dst, sizeof(dst),
                                      &out_len, 0),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("%2", 2, dst, sizeof(dst),
                                      &out_len, 0),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("%zz", 3, dst, sizeof(dst),
                                      &out_len, 0),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("%2g", 3, dst, sizeof(dst),
                                      &out_len, 0),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("a%2", 3, dst, sizeof(dst),
                                      &out_len, 0),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_uri_decode_capacity(void)
{
    char dst[8];
    size_t out_len = 0;

    /* "abc" needs 4 bytes (3 + NUL). */
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("abc", 3, dst, 4, &out_len, 0),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("abc", 3, dst, 3, &out_len, 0),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* Encoded input decodes smaller; capacity is on output. */
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("%41%42%43", 9, dst, 4,
                                      &out_len, 0),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(memcmp(dst, "ABC", 4) == 0);

    /* cap == 0 rejected even for empty input. */
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("", 0, dst, 0, &out_len, 0),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_uri_decode_arguments(void)
{
    char dst[8];
    size_t out_len = 0;

    CFW_TEST_ASSERT_EQ(cfw_uri_decode(NULL, 5, dst, sizeof(dst),
                                      &out_len, 0),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("abc", 3, NULL, sizeof(dst),
                                      &out_len, 0),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("abc", 3, dst, sizeof(dst),
                                      NULL, 0),
                       CFW_ERROR_INVALID_ARGUMENT);
    /* Unknown flag bits rejected. */
    CFW_TEST_ASSERT_EQ(cfw_uri_decode("abc", 3, dst, sizeof(dst),
                                      &out_len, 0x80000000u),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_uri_decode_utf8_passthrough(void)
{
    /* Non-ASCII bytes pass through verbatim; the decoder is byte-
     * oriented and does not validate UTF-8. */
    char dst[8];
    size_t out_len = 0;
    const char src[] = {'\xC3', '\xA9'}; /* U+00E9 in UTF-8 */

    CFW_TEST_ASSERT_EQ(cfw_uri_decode(src, 2, dst, sizeof(dst),
                                      &out_len, 0),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(out_len, 2);
    CFW_TEST_ASSERT_TRUE(memcmp(dst, src, 2) == 0);
    return 0;
}

const cfw_test_case cfw_test_uri_cases[] = {
    {"uri_decode_basic", test_uri_decode_basic},
    {"uri_decode_plus_flag", test_uri_decode_plus_flag},
    {"uri_decode_reject_flags", test_uri_decode_reject_flags},
    {"uri_decode_malformed", test_uri_decode_malformed},
    {"uri_decode_capacity", test_uri_decode_capacity},
    {"uri_decode_arguments", test_uri_decode_arguments},
    {"uri_decode_utf8_passthrough", test_uri_decode_utf8_passthrough},
    {NULL, NULL},
};
