/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <string.h>

static cfw_strview_t cfw_test_view_n(const char *text, size_t size)
{
    cfw_strview_t view;
    cfw_strview_init(&view, text, size);
    return view;
}

static cfw_strview_t cfw_test_view(const char *text)
{
    return cfw_test_view_n(text, strlen(text));
}

static int test_signed_roundtrip(void)
{
    static const char key[] = "test-secret-key";
    static const char payload[] = "session-id-12345|uid=42|exp=9999999999";
    char token[256];
    unsigned char dec[128];
    size_t written = 0;
    size_t dec_len = 0;
    cfw_strview_t tv;

    CFW_TEST_ASSERT_EQ(cfw_signed_encode(key, sizeof(key) - 1, payload,
                                         sizeof(payload) - 1, token,
                                         sizeof(token), &written),
                       CFW_OK);
    /* Format: "v1." + b64 + "." + 44-char MAC. */
    CFW_TEST_ASSERT_TRUE(strncmp(token, "v1.", 3) == 0);
    CFW_TEST_ASSERT_EQ(strlen(token), written);

    tv = cfw_test_view_n(token, written);
    CFW_TEST_ASSERT_EQ(cfw_signed_decode(key, sizeof(key) - 1, &tv, dec,
                                         sizeof(dec), &dec_len),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(dec_len, sizeof(payload) - 1);
    CFW_TEST_ASSERT_TRUE(memcmp(dec, payload, dec_len) == 0);
    return 0;
}

static int test_signed_empty_and_binary_payload(void)
{
    static const char key[] = "k";
    static const unsigned char bin[] = {0x00, 0xff, 0x10, 0x80, 'a', 0x00};
    char token[256];
    unsigned char dec[64];
    size_t written = 0;
    size_t dec_len = 999;
    cfw_strview_t tv;

    /* Empty payload. */
    CFW_TEST_ASSERT_EQ(cfw_signed_encode(key, 1, "", 0, token,
                                         sizeof(token), &written),
                       CFW_OK);
    tv = cfw_test_view_n(token, written);
    CFW_TEST_ASSERT_EQ(cfw_signed_decode(key, 1, &tv, NULL, 0, &dec_len),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(dec_len, 0);

    /* Binary payload including NUL bytes. */
    CFW_TEST_ASSERT_EQ(cfw_signed_encode(key, 1, bin, sizeof(bin), token,
                                         sizeof(token), &written),
                       CFW_OK);
    tv = cfw_test_view_n(token, written);
    CFW_TEST_ASSERT_EQ(cfw_signed_decode(key, 1, &tv, dec, sizeof(dec),
                                         &dec_len),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(dec_len, sizeof(bin));
    CFW_TEST_ASSERT_TRUE(memcmp(dec, bin, sizeof(bin)) == 0);
    return 0;
}

static int test_signed_tamper_rejection(void)
{
    static const char key[] = "secret";
    static const char payload[] = "value";
    char token[256];
    char tampered[256];
    unsigned char dec[64];
    size_t written = 0;
    size_t i;
    cfw_strview_t tv;

    CFW_TEST_ASSERT_EQ(cfw_signed_encode(key, sizeof(key) - 1, payload,
                                         sizeof(payload) - 1, token,
                                         sizeof(token), &written),
                       CFW_OK);

    /* Every single-character mutation of the token must fail. */
    for (i = 0; i < written; i++) {
        cfw_status_t st;
        memcpy(tampered, token, written + 1u);
        tampered[i] = tampered[i] == 'A' ? 'B' : 'A';
        tv = cfw_test_view_n(tampered, written);
        st = cfw_signed_decode(key, sizeof(key) - 1, &tv, dec,
                               sizeof(dec), NULL);
        CFW_TEST_ASSERT_EQ(st, CFW_ERROR_INVALID_ARGUMENT);
    }
    return 0;
}

static int test_signed_wrong_key_and_garbage(void)
{
    static const char key[] = "secret";
    static const char other[] = "other";
    static const char payload[] = "x";
    char token[256];
    unsigned char dec[64];
    size_t written = 0;
    cfw_strview_t tv;

    CFW_TEST_ASSERT_EQ(cfw_signed_encode(key, sizeof(key) - 1, payload, 1,
                                         token, sizeof(token), &written),
                       CFW_OK);
    tv = cfw_test_view_n(token, written);

    /* Wrong key. */
    CFW_TEST_ASSERT_EQ(cfw_signed_decode(other, sizeof(other) - 1, &tv,
                                         dec, sizeof(dec), NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    /* Truncated. */
    tv = cfw_test_view_n(token, written - 1u);
    CFW_TEST_ASSERT_EQ(cfw_signed_decode(key, sizeof(key) - 1, &tv, dec,
                                         sizeof(dec), NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    /* Empty token. */
    tv = cfw_test_view("");
    CFW_TEST_ASSERT_EQ(cfw_signed_decode(key, sizeof(key) - 1, &tv, dec,
                                         sizeof(dec), NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    /* Bad version prefix. */
    {
        char bad[256];
        memcpy(bad, token, written + 1u);
        bad[1] = '2'; /* "v2." */
        tv = cfw_test_view_n(bad, written);
        CFW_TEST_ASSERT_EQ(cfw_signed_decode(key, sizeof(key) - 1, &tv,
                                             dec, sizeof(dec), NULL),
                           CFW_ERROR_INVALID_ARGUMENT);
    }
    return 0;
}

static int test_signed_capacity(void)
{
    static const char key[] = "k";
    static const char payload[] = "0123456789";
    char token[128];
    size_t written = 0;

    CFW_TEST_ASSERT_EQ(cfw_signed_encode_bound(sizeof(payload) - 1),
                       cfw_signed_encode_bound(10));
    {
        size_t bound = cfw_signed_encode_bound(sizeof(payload) - 1);
        CFW_TEST_ASSERT_TRUE(bound <= sizeof(token));
        CFW_TEST_ASSERT_EQ(cfw_signed_encode(key, 1, payload,
                                             sizeof(payload) - 1, token,
                                             bound - 1u, &written),
                           CFW_ERROR_OVERFLOW);
        CFW_TEST_ASSERT_EQ(cfw_signed_encode(key, 1, payload,
                                             sizeof(payload) - 1, token,
                                             bound, &written),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(written, bound - 1u);
    }
    /* Decode-side payload capacity. */
    {
        unsigned char dec[16];
        size_t dec_len = 0;
        cfw_strview_t tv = cfw_test_view_n(token, written);
        CFW_TEST_ASSERT_EQ(cfw_signed_decode(key, 1, &tv, dec, 4,
                                             &dec_len),
                           CFW_ERROR_OVERFLOW);
        CFW_TEST_ASSERT_EQ(cfw_signed_decode(key, 1, &tv, dec, 10,
                                             &dec_len),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(dec_len, 10u);
    }
    /* Bound overflow sentinel. */
    CFW_TEST_ASSERT_EQ(cfw_signed_encode_bound(SIZE_MAX), SIZE_MAX);
    return 0;
}

static int test_signed_arguments(void)
{
    static const char key[] = "k";
    char token[64];
    unsigned char dec[8];
    cfw_strview_t tv = cfw_test_view("");

    CFW_TEST_ASSERT_EQ(cfw_signed_encode(NULL, 1, "x", 1, token,
                                         sizeof(token), NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_signed_encode(key, 1, NULL, 1, token,
                                         sizeof(token), NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_signed_encode(key, 1, "x", 1, NULL,
                                         sizeof(token), NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_signed_decode(NULL, 1, &tv, dec, sizeof(dec),
                                         NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_signed_decode(key, 1, NULL, dec, sizeof(dec),
                                         NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    /* Empty key is valid (HMAC with empty key). */
    CFW_TEST_ASSERT_EQ(cfw_signed_encode(NULL, 0, "x", 1, token,
                                         sizeof(token), NULL),
                       CFW_OK);
    return 0;
}

static int test_signed_cookie_safe_charset(void)
{
    /* The token must be usable as a cookie-octet value without
     * quoting: v, digits, '.', base64 alphabet incl '+' '/' '='. */
    static const char key[] = "k";
    static const unsigned char payload[] = {0xfb, 0xff, 0xfe, 0x00, 0x7f};
    char token[128];
    size_t written = 0;
    size_t i;

    CFW_TEST_ASSERT_EQ(cfw_signed_encode(key, 1, payload, sizeof(payload),
                                         token, sizeof(token), &written),
                       CFW_OK);
    for (i = 0; i < written; i++) {
        unsigned char c = (unsigned char)token[i];
        /* cookie-octet: %x21 / %x23-2B / %x2D-3A / %x3C-5B / %x5D-7E */
        int ok = c == 0x21 || (c >= 0x23 && c <= 0x2b) ||
                 (c >= 0x2d && c <= 0x3a) || (c >= 0x3c && c <= 0x5b) ||
                 (c >= 0x5d && c <= 0x7e);
        CFW_TEST_ASSERT_TRUE(ok);
    }
    return 0;
}

const cfw_test_case cfw_test_signed_cases[] = {
    {"signed_roundtrip", test_signed_roundtrip},
    {"signed_empty_and_binary_payload", test_signed_empty_and_binary_payload},
    {"signed_tamper_rejection", test_signed_tamper_rejection},
    {"signed_wrong_key_and_garbage", test_signed_wrong_key_and_garbage},
    {"signed_capacity", test_signed_capacity},
    {"signed_arguments", test_signed_arguments},
    {"signed_cookie_safe_charset", test_signed_cookie_safe_charset},
    {NULL, NULL},
};
