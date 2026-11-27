/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <string.h>

static cfw_strview_t cfw_test_view(const char *text)
{
    cfw_strview_t view;
    cfw_strview_init(&view, text, strlen(text));
    return view;
}

static int cfw_test_str_is(const char *s, const char *expected)
{
    return strcmp(s, expected) == 0;
}

/* RFC 4648 test vectors. */
static int test_base64_rfc4648_vectors(void)
{
    static const struct {
        const char *raw;
        const char *encoded;
    } vectors[] = {
        {"", ""},
        {"f", "Zg=="},
        {"fo", "Zm8="},
        {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="},
        {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"},
    };
    size_t i;

    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        char out[16];
        unsigned char dec[16];
        size_t written = 0;
        cfw_strview_t in;

        CFW_TEST_ASSERT_EQ(cfw_base64_encode(vectors[i].raw,
                                             strlen(vectors[i].raw),
                                             out, sizeof(out), &written),
                           CFW_OK);
        CFW_TEST_ASSERT_TRUE(cfw_test_str_is(out, vectors[i].encoded));
        CFW_TEST_ASSERT_EQ(written, strlen(vectors[i].encoded));

        in = cfw_test_view(vectors[i].encoded);
        CFW_TEST_ASSERT_EQ(cfw_base64_decode(&in, dec, sizeof(dec),
                                             &written),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(written, strlen(vectors[i].raw));
        CFW_TEST_ASSERT_TRUE(memcmp(dec, vectors[i].raw, written) == 0);
    }
    return 0;
}

static int test_base64_encode_bound(void)
{
    CFW_TEST_ASSERT_EQ(cfw_base64_encode_bound(0), 1u);
    CFW_TEST_ASSERT_EQ(cfw_base64_encode_bound(1), 5u);
    CFW_TEST_ASSERT_EQ(cfw_base64_encode_bound(2), 5u);
    CFW_TEST_ASSERT_EQ(cfw_base64_encode_bound(3), 5u);
    CFW_TEST_ASSERT_EQ(cfw_base64_encode_bound(4), 9u);
    CFW_TEST_ASSERT_EQ(cfw_base64_encode_bound(1024),
                       ((1024u + 2u) / 3u) * 4u + 1u);
    CFW_TEST_ASSERT_EQ(cfw_base64_encode_bound(SIZE_MAX), SIZE_MAX);
    return 0;
}

static int test_base64_encode_capacity(void)
{
    char out[16];
    size_t written = 999;

    /* "foobar" needs 9 bytes including NUL. */
    CFW_TEST_ASSERT_EQ(cfw_base64_encode("foobar", 6, out, 8, &written),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(cfw_base64_encode("foobar", 6, out, 9, &written),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_test_str_is(out, "Zm9vYmFy"));
    CFW_TEST_ASSERT_EQ(written, 8u);
    CFW_TEST_ASSERT_EQ(cfw_base64_encode("x", 1, NULL, 8, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_base64_encode(NULL, 1, out, sizeof(out), NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    /* NULL data with zero size is valid (encodes empty string). */
    CFW_TEST_ASSERT_EQ(cfw_base64_encode(NULL, 0, out, sizeof(out), NULL),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_test_str_is(out, ""));
    return 0;
}

static int test_base64_decode_malformed(void)
{
    static const char *bad[] = {
        "Zg",      /* length not a multiple of 4 */
        "Z",       /* ditto */
        "Z===",    /* too much padding / '=' in wrong place */
        "=g==",    /* leading '=' */
        "Z=g=",    /* '=' before end */
        "Zg=A",    /* '=' mid-group */
        "Zg==Zg==",/* padding not at end */
        "Zh==",    /* nonzero pad bits (canonical violation) */
        "Zm9=",    /* nonzero pad bits */
        "Zm 9",    /* whitespace is not alphabet */
        "Zm9v\n",  /* newline rejected */
        "Zm9v+",   /* truncated tail char? len%4==1 rejected above */
    };
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        unsigned char out[16];
        size_t written = 0;
        cfw_strview_t in = cfw_test_view(bad[i]);
        CFW_TEST_ASSERT_EQ(cfw_base64_decode(&in, out, sizeof(out),
                                             &written),
                           CFW_ERROR_INVALID_ARGUMENT);
    }
    return 0;
}

static int test_base64_decode_capacity(void)
{
    unsigned char out[8];
    size_t written = 0;
    cfw_strview_t in = cfw_test_view("Zm9vYmFy"); /* 6 bytes */

    CFW_TEST_ASSERT_EQ(cfw_base64_decode(&in, out, 5, &written),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(cfw_base64_decode(&in, out, 6, &written),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(written, 6u);
    CFW_TEST_ASSERT_TRUE(memcmp(out, "foobar", 6) == 0);

    /* Empty input decodes to zero bytes; NULL out is allowed. */
    in = cfw_test_view("");
    CFW_TEST_ASSERT_EQ(cfw_base64_decode(&in, NULL, 0, &written),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(written, 0u);
    return 0;
}

static int test_base64_decode_null_args(void)
{
    unsigned char out[8];
    size_t written = 0;
    cfw_strview_t in = cfw_test_view("Zm9v");

    CFW_TEST_ASSERT_EQ(cfw_base64_decode(NULL, out, sizeof(out),
                                         &written),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_base64_decode(&in, NULL, sizeof(out),
                                         &written),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_hex_roundtrip(void)
{
    static const unsigned char raw[] = {0x00, 0x01, 0x23, 0x45, 0x67,
                                        0x89, 0xab, 0xcd, 0xef, 0xff};
    char out[32];
    unsigned char dec[16];
    size_t written = 0;
    cfw_strview_t in;

    CFW_TEST_ASSERT_EQ(cfw_hex_encode(raw, sizeof(raw), out, sizeof(out),
                                      &written),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_test_str_is(out, "000123456789abcdefff"));
    CFW_TEST_ASSERT_EQ(written, 20u);

    in = cfw_test_view(out);
    CFW_TEST_ASSERT_EQ(cfw_hex_decode(&in, dec, sizeof(dec), &written),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(written, sizeof(raw));
    CFW_TEST_ASSERT_TRUE(memcmp(dec, raw, sizeof(raw)) == 0);
    return 0;
}

static int test_hex_decode_mixed_case(void)
{
    unsigned char out[4];
    size_t written = 0;
    cfw_strview_t in = cfw_test_view("DeAdBeEf");

    CFW_TEST_ASSERT_EQ(cfw_hex_decode(&in, out, sizeof(out), &written),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(written, 4u);
    CFW_TEST_ASSERT_EQ(out[0], 0xde);
    CFW_TEST_ASSERT_EQ(out[3], 0xef);
    return 0;
}

static int test_hex_malformed(void)
{
    static const char *bad[] = {"0", "abc", "zz", "12 34", "0x12", "gg"};
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        unsigned char out[8];
        size_t written = 0;
        cfw_strview_t in = cfw_test_view(bad[i]);
        CFW_TEST_ASSERT_EQ(cfw_hex_decode(&in, out, sizeof(out),
                                          &written),
                           CFW_ERROR_INVALID_ARGUMENT);
    }
    return 0;
}

/* RFC 4648 section 5 vectors, unpadded canonical form (ADR 0059). */
static int test_base64url_vectors(void)
{
    static const struct {
        const char *raw;
        size_t raw_len;
        const char *encoded;
    } vectors[] = {
        {"", 0, ""},
        {"f", 1, "Zg"},
        {"fo", 2, "Zm8"},
        {"foo", 3, "Zm9v"},
        {"foob", 4, "Zm9vYg"},
        {"fooba", 5, "Zm9vYmE"},
        {"foobar", 6, "Zm9vYmFy"},
        {"\xff\xff", 2, "__8"},
        {"\xfb\xff\xfe", 3, "-__-"},
    };
    size_t i;

    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        char out[16];
        unsigned char dec[16];
        size_t written = 0;
        cfw_strview_t in;

        CFW_TEST_ASSERT_EQ(cfw_base64url_encode(vectors[i].raw,
                                                vectors[i].raw_len,
                                                out, sizeof(out),
                                                &written),
                           CFW_OK);
        CFW_TEST_ASSERT_TRUE(cfw_test_str_is(out, vectors[i].encoded));
        CFW_TEST_ASSERT_EQ(written, strlen(vectors[i].encoded));

        in = cfw_test_view(vectors[i].encoded);
        CFW_TEST_ASSERT_EQ(cfw_base64url_decode(&in, dec, sizeof(dec),
                                                &written),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(written, vectors[i].raw_len);
        CFW_TEST_ASSERT_TRUE(
            memcmp(dec, vectors[i].raw, vectors[i].raw_len) == 0);
    }
    return 0;
}

/* The decoder also accepts canonical '='-padded input. */
static int test_base64url_decode_padded(void)
{
    static const struct {
        const char *encoded;
        const char *raw;
    } vectors[] = {
        {"Zg==", "f"},
        {"Zm8=", "fo"},
        {"Zm9v", "foo"},
        {"Zm9vYg==", "foob"},
        {"__8=", "\xff\xff"},
        {"-__-", "\xfb\xff\xfe"}, /* 3-byte group needs no padding */
    };
    size_t i;

    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        unsigned char dec[16];
        size_t written = 0;
        cfw_strview_t in = cfw_test_view(vectors[i].encoded);

        CFW_TEST_ASSERT_EQ(cfw_base64url_decode(&in, dec, sizeof(dec),
                                                &written),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(written, strlen(vectors[i].raw));
        CFW_TEST_ASSERT_TRUE(
            memcmp(dec, vectors[i].raw, written) == 0);
    }
    return 0;
}

static int test_base64url_decode_malformed(void)
{
    static const char *bad[] = {
        "A",        /* single sextet: impossible length */
        "AAAAA",    /* 5 sextets: rem == 1 */
        "Zh==",     /* nonzero pad bits in final sextet */
        "Zm9=",     /* one pad on a 3-char tail is non-canonical */
        "Zg=",      /* padded form not a multiple of 4 */
        "Zg===",    /* more than two pads */
        "Zg==Zg==", /* '=' in the middle */
        "Z g==",    /* whitespace is not alphabet */
        "Zg++",     /* standard alphabet rejected */
        "Zg//",     /* standard alphabet rejected */
    };
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        unsigned char out[16];
        size_t written = 0;
        cfw_strview_t in = cfw_test_view(bad[i]);
        CFW_TEST_ASSERT_EQ(cfw_base64url_decode(&in, out, sizeof(out),
                                                &written),
                           CFW_ERROR_INVALID_ARGUMENT);
    }
    return 0;
}

static int test_base64url_capacity(void)
{
    char out[16];
    unsigned char dec[8];
    size_t written = 0;
    cfw_strview_t in;

    CFW_TEST_ASSERT_EQ(cfw_base64url_encode_bound(0), 1u);
    CFW_TEST_ASSERT_EQ(cfw_base64url_encode_bound(1), 3u);
    CFW_TEST_ASSERT_EQ(cfw_base64url_encode_bound(2), 4u);
    CFW_TEST_ASSERT_EQ(cfw_base64url_encode_bound(3), 5u);
    CFW_TEST_ASSERT_EQ(cfw_base64url_encode_bound(1024),
                       ((1024u / 3u) * 4u + 2u) + 1u);
    CFW_TEST_ASSERT_EQ(cfw_base64url_encode_bound(SIZE_MAX), SIZE_MAX);

    /* "foobar" -> "Zm9vYmFy" (8 chars + NUL = 9). */
    CFW_TEST_ASSERT_EQ(cfw_base64url_encode("foobar", 6, out, 8,
                                            &written),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(cfw_base64url_encode("foobar", 6, out, 9,
                                            &written),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_test_str_is(out, "Zm9vYmFy"));

    in = cfw_test_view("Zm9vYmFy");
    CFW_TEST_ASSERT_EQ(cfw_base64url_decode(&in, dec, 5, &written),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(cfw_base64url_decode(&in, dec, 6, &written),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(written, 6u);

    CFW_TEST_ASSERT_EQ(cfw_base64url_encode(NULL, 1, out, sizeof(out),
                                            NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_base64url_decode(NULL, dec, sizeof(dec),
                                            &written),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_hex_capacity(void)
{
    char out[16];
    unsigned char dec[4];
    size_t written = 0;
    cfw_strview_t in = cfw_test_view("01020304"); /* 4 bytes */

    CFW_TEST_ASSERT_EQ(cfw_hex_encode("\x01\x02\x03\x04", 4, out, 8,
                                      &written),
                       CFW_ERROR_OVERFLOW); /* needs 9 */
    CFW_TEST_ASSERT_EQ(cfw_hex_encode("\x01\x02\x03\x04", 4, out, 9,
                                      &written),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_test_str_is(out, "01020304"));

    CFW_TEST_ASSERT_EQ(cfw_hex_decode(&in, dec, 3, &written),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(cfw_hex_decode(&in, dec, 4, &written), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_hex_encode_bound(4), 9u);
    CFW_TEST_ASSERT_EQ(cfw_hex_encode_bound(SIZE_MAX), SIZE_MAX);
    return 0;
}

const cfw_test_case cfw_test_codec_cases[] = {
    {"base64_rfc4648_vectors", test_base64_rfc4648_vectors},
    {"base64_encode_bound", test_base64_encode_bound},
    {"base64_encode_capacity", test_base64_encode_capacity},
    {"base64_decode_malformed", test_base64_decode_malformed},
    {"base64_decode_capacity", test_base64_decode_capacity},
    {"base64_decode_null_args", test_base64_decode_null_args},
    {"base64url_vectors", test_base64url_vectors},
    {"base64url_decode_padded", test_base64url_decode_padded},
    {"base64url_decode_malformed", test_base64url_decode_malformed},
    {"base64url_capacity", test_base64url_capacity},
    {"hex_roundtrip", test_hex_roundtrip},
    {"hex_decode_mixed_case", test_hex_decode_mixed_case},
    {"hex_malformed", test_hex_malformed},
    {"hex_capacity", test_hex_capacity},
    {NULL, NULL},
};
