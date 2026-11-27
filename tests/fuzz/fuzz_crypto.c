/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Coverage-guided fuzz target for the crypto/codec/cookie/signed-token
 * surface (ADR 0077): SHA-256/HMAC (one-shot and chunked streaming),
 * base64/base64url/hex codecs, constant-time compare, signed-token
 * decode, and Cookie header parsing. All in-memory, public API only.
 */

#include <cfw/cfw.h>
#include <cfw/codec.h>
#include <cfw/cookie.h>
#include <cfw/crypto.h>
#include <cfw/signed.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CFW_FUZZ_CRYPTO_MAX_INPUT (64u * 1024u)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const char key[] = "cfw-fuzz-key";
    unsigned char digest[32];
    char *buf;
    cfw_strview_t view;

    if (size == 0 || size > CFW_FUZZ_CRYPTO_MAX_INPUT) {
        return 0;
    }

    /* SHA-256 one-shot plus a chunked streaming pass whose split
     * points derive from input bytes. */
    cfw_sha256(data, size, digest);
    {
        cfw_sha256_ctx_t ctx;
        size_t pos = 0;

        cfw_sha256_init(&ctx);
        while (pos < size) {
            size_t step = 1u + (size_t)(data[pos] % 251u);
            if (step > size - pos) {
                step = size - pos;
            }
            cfw_sha256_update(&ctx, data + pos, step);
            pos += step;
        }
        cfw_sha256_final(&ctx, digest);
    }

    /* HMAC-SHA256 one-shot and streaming. */
    cfw_hmac_sha256(key, sizeof(key) - 1, data, size, digest);
    {
        cfw_hmac_sha256_ctx_t ctx;

        cfw_hmac_sha256_init(&ctx, key, sizeof(key) - 1);
        cfw_hmac_sha256_update(&ctx, data, size);
        cfw_hmac_sha256_final(&ctx, digest);
    }

    (void)cfw_secure_equal(digest, data, size < sizeof(digest)
                                          ? size
                                          : sizeof(digest));

    /* Codec round-trips: encode into a bounded buffer, decode the
     * input itself as if it were encoded text. */
    {
        size_t enc_cap = cfw_base64_encode_bound(size);
        if (enc_cap != SIZE_MAX && enc_cap <= (size * 2 + 64)) {
            buf = (char *)malloc(enc_cap);
            if (buf != NULL) {
                size_t written = 0;
                (void)cfw_base64_encode(data, size, buf, enc_cap,
                                        &written);
                free(buf);
            }
        }
    }
    buf = (char *)malloc(size + 1);
    if (buf == NULL) {
        return 0;
    }
    view.data = data;
    view.size = size;
    {
        size_t written = 0;
        (void)cfw_base64_decode(&view, (unsigned char *)buf, size + 1,
                                &written);
        (void)cfw_base64url_decode(&view, (unsigned char *)buf, size + 1,
                                   &written);
        (void)cfw_hex_decode(&view, (unsigned char *)buf, size + 1,
                             &written);
    }

    /* Signed-token decode with a fixed key (MAC mismatch is an
     * acceptable outcome; the parse path is the fuzz surface). */
    {
        unsigned char payload[256];
        size_t payload_size = 0;
        (void)cfw_signed_decode(key, sizeof(key) - 1, &view, payload,
                                sizeof(payload), &payload_size);
    }

    /* Cookie header parse. */
    {
        cfw_cookie_pair_t pairs[8];
        size_t count = 0;
        size_t dropped = 0;
        (void)cfw_cookie_parse(&view, pairs,
                               sizeof(pairs) / sizeof(pairs[0]), &count,
                               &dropped);
    }

    free(buf);
    return 0;
}
