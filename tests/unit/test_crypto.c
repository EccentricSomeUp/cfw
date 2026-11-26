/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <string.h>

static unsigned int cfw_test_hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return (unsigned int)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (unsigned int)(c - 'a' + 10u);
    }
    return (unsigned int)(c - 'A' + 10u);
}

/* Decodes a 64-char lowercase hex string into `out` (32 bytes). */
static void cfw_test_hex32(const char *hex, unsigned char *out)
{
    size_t i;

    for (i = 0; i < 32u; i++) {
        unsigned int hi = cfw_test_hex_value(hex[i * 2u]);
        unsigned int lo = cfw_test_hex_value(hex[i * 2u + 1u]);
        out[i] = (unsigned char)((hi << 4u) | lo);
    }
}

static int cfw_test_digest_is(const unsigned char *digest,
                              const char *expected_hex)
{
    unsigned char expected[CFW_SHA256_DIGEST_SIZE];

    cfw_test_hex32(expected_hex, expected);
    return memcmp(digest, expected, CFW_SHA256_DIGEST_SIZE) == 0;
}

static int test_sha256_empty(void)
{
    unsigned char out[CFW_SHA256_DIGEST_SIZE];

    cfw_sha256("", 0, out);
    CFW_TEST_ASSERT_TRUE(cfw_test_digest_is(
        out, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca49599"
             "1b7852b855"));
    return 0;
}

static int test_sha256_abc(void)
{
    unsigned char out[CFW_SHA256_DIGEST_SIZE];

    cfw_sha256("abc", 3, out);
    CFW_TEST_ASSERT_TRUE(cfw_test_digest_is(
        out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410f"
             "f61f20015ad"));
    return 0;
}

static int test_sha256_fips_vector(void)
{
    /* FIPS 180-4 example message: "abcdbcde...nopq" (56 bytes). */
    static const char msg[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    unsigned char out[CFW_SHA256_DIGEST_SIZE];

    cfw_sha256(msg, sizeof(msg) - 1u, out);
    CFW_TEST_ASSERT_TRUE(cfw_test_digest_is(
        out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ec"
             "edd419db06c1"));
    return 0;
}

static int test_sha256_million_a(void)
{
    /* FIPS 180-4 million-'a' vector; also exercises the streaming
     * path across many block boundaries. */
    cfw_sha256_ctx_t ctx;
    unsigned char out[CFW_SHA256_DIGEST_SIZE];
    unsigned char block[1000];
    size_t i;

    memset(block, 'a', sizeof(block));
    cfw_sha256_init(&ctx);
    for (i = 0; i < 1000u; i++) {
        cfw_sha256_update(&ctx, block, sizeof(block));
    }
    cfw_sha256_final(&ctx, out);
    CFW_TEST_ASSERT_TRUE(cfw_test_digest_is(
        out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d"
             "39ccc7112cd0"));
    return 0;
}

static int test_sha256_streaming_equivalence(void)
{
    /* Chunked updates of odd sizes must match the one-shot digest. */
    static const char msg[] =
        "The quick brown fox jumps over the lazy dog";
    cfw_sha256_ctx_t ctx;
    unsigned char one_shot[CFW_SHA256_DIGEST_SIZE];
    unsigned char streamed[CFW_SHA256_DIGEST_SIZE];
    size_t i;

    cfw_sha256(msg, sizeof(msg) - 1u, one_shot);
    cfw_sha256_init(&ctx);
    for (i = 0; i < sizeof(msg) - 1u; i += 7u) {
        size_t n = sizeof(msg) - 1u - i;
        if (n > 7u) {
            n = 7u;
        }
        cfw_sha256_update(&ctx, msg + i, n);
    }
    cfw_sha256_final(&ctx, streamed);
    CFW_TEST_ASSERT_TRUE(
        memcmp(one_shot, streamed, CFW_SHA256_DIGEST_SIZE) == 0);
    return 0;
}

static int test_sha256_block_boundary(void)
{
    /* Messages of exactly 55/56/64/65 bytes exercise every padding
     * branch of the final block. */
    static const char msg[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        "!@#$%^&*()_+-=[]{}|;':\",./<>?`~"
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    size_t sizes[] = {55u, 56u, 63u, 64u, 65u, 119u, 120u, 128u};
    size_t i;
    cfw_sha256_ctx_t ctx;
    unsigned char a[CFW_SHA256_DIGEST_SIZE];
    unsigned char b[CFW_SHA256_DIGEST_SIZE];

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        cfw_sha256(msg, sizes[i], a);
        cfw_sha256_init(&ctx);
        cfw_sha256_update(&ctx, msg, sizes[i]);
        cfw_sha256_final(&ctx, b);
        CFW_TEST_ASSERT_TRUE(
            memcmp(a, b, CFW_SHA256_DIGEST_SIZE) == 0);
    }
    return 0;
}

static int test_hmac_rfc4231_case1(void)
{
    unsigned char key[20];
    unsigned char out[CFW_HMAC_SHA256_DIGEST_SIZE];

    memset(key, 0x0bu, sizeof(key));
    cfw_hmac_sha256(key, sizeof(key), "Hi There", 8, out);
    CFW_TEST_ASSERT_TRUE(cfw_test_digest_is(
        out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e"
             "9376c2e32cff7"));
    return 0;
}

static int test_hmac_rfc4231_case2(void)
{
    unsigned char out[CFW_HMAC_SHA256_DIGEST_SIZE];

    cfw_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out);
    CFW_TEST_ASSERT_TRUE(cfw_test_digest_is(
        out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec"
             "58b964ec3843"));
    return 0;
}

static int test_hmac_rfc4231_case3(void)
{
    unsigned char key[20];
    unsigned char data[50];
    unsigned char out[CFW_HMAC_SHA256_DIGEST_SIZE];

    memset(key, 0xaau, sizeof(key));
    memset(data, 0xddu, sizeof(data));
    cfw_hmac_sha256(key, sizeof(key), data, sizeof(data), out);
    CFW_TEST_ASSERT_TRUE(cfw_test_digest_is(
        out, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d963"
             "5514ced565fe"));
    return 0;
}

static int test_hmac_long_key(void)
{
    /* Keys longer than 64 bytes are hashed first per RFC 2104. */
    unsigned char key[200];
    unsigned char out[CFW_HMAC_SHA256_DIGEST_SIZE];
    unsigned char ref[CFW_SHA256_DIGEST_SIZE];

    memset(key, 0x5au, sizeof(key));
    cfw_hmac_sha256(key, sizeof(key), "keyed message", 13, out);

    /* Reference: HMAC with the pre-hashed (then zero-padded) key. */
    cfw_sha256(key, sizeof(key), ref);
    {
        unsigned char kblock[64];
        cfw_sha256_ctx_t ctx;
        unsigned char inner[CFW_SHA256_DIGEST_SIZE];
        size_t i;

        memset(kblock, 0, sizeof(kblock));
        memcpy(kblock, ref, CFW_SHA256_DIGEST_SIZE);
        for (i = 0; i < 64u; i++) {
            kblock[i] ^= 0x36u;
        }
        cfw_sha256_init(&ctx);
        cfw_sha256_update(&ctx, kblock, 64u);
        cfw_sha256_update(&ctx, "keyed message", 13);
        cfw_sha256_final(&ctx, inner);
        for (i = 0; i < 64u; i++) {
            kblock[i] ^= 0x6au;
        }
        cfw_sha256_init(&ctx);
        cfw_sha256_update(&ctx, kblock, 64u);
        cfw_sha256_update(&ctx, inner, sizeof(inner));
        cfw_sha256_final(&ctx, ref);
    }
    CFW_TEST_ASSERT_TRUE(
        memcmp(out, ref, CFW_HMAC_SHA256_DIGEST_SIZE) == 0);
    return 0;
}

static int test_hmac_empty_key_and_data(void)
{
    unsigned char out[CFW_HMAC_SHA256_DIGEST_SIZE];

    /* Empty key and empty data are valid per RFC 2104. */
    cfw_hmac_sha256(NULL, 0, "", 0, out);
    CFW_TEST_ASSERT_TRUE(cfw_test_digest_is(
        out, "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c7"
             "12144292c5ad"));
    return 0;
}

static int test_secure_equal(void)
{
    static const unsigned char a[32] = {0};
    static const unsigned char b[32] = {0};
    unsigned char c[32];

    memset(c, 0, sizeof(c));
    c[31] = 1;
    CFW_TEST_ASSERT_TRUE(cfw_secure_equal(a, b, sizeof(a)));
    CFW_TEST_ASSERT_FALSE(cfw_secure_equal(a, c, sizeof(a)));
    CFW_TEST_ASSERT_TRUE(cfw_secure_equal(a, b, 0));
    CFW_TEST_ASSERT_TRUE(cfw_secure_equal(NULL, NULL, 0));
    return 0;
}

const cfw_test_case cfw_test_crypto_cases[] = {
    {"sha256_empty", test_sha256_empty},
    {"sha256_abc", test_sha256_abc},
    {"sha256_fips_vector", test_sha256_fips_vector},
    {"sha256_million_a", test_sha256_million_a},
    {"sha256_streaming_equivalence", test_sha256_streaming_equivalence},
    {"sha256_block_boundary", test_sha256_block_boundary},
    {"hmac_rfc4231_case1", test_hmac_rfc4231_case1},
    {"hmac_rfc4231_case2", test_hmac_rfc4231_case2},
    {"hmac_rfc4231_case3", test_hmac_rfc4231_case3},
    {"hmac_long_key", test_hmac_long_key},
    {"hmac_empty_key_and_data", test_hmac_empty_key_and_data},
    {"secure_equal", test_secure_equal},
    {NULL, NULL},
};
