/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/crypto.h>

#include <string.h>

/* FIPS 180-4 section 4.2.2 round constants. */
static const uint32_t cfw_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t cfw_rotr32(uint32_t x, unsigned int n)
{
    return (x >> n) | (x << (32u - n));
}

/* The SHA-256 compression function over one 64-byte block. */
static void cfw_sha256_compress(cfw_sha256_ctx_t *ctx,
                                const unsigned char *block)
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    unsigned int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4u] << 24) |
               ((uint32_t)block[i * 4u + 1u] << 16) |
               ((uint32_t)block[i * 4u + 2u] << 8) |
               (uint32_t)block[i * 4u + 3u];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = cfw_rotr32(w[i - 15u], 7) ^
                      cfw_rotr32(w[i - 15u], 18) ^ (w[i - 15u] >> 3);
        uint32_t s1 = cfw_rotr32(w[i - 2u], 17) ^
                      cfw_rotr32(w[i - 2u], 19) ^ (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t s1 = cfw_rotr32(e, 6) ^ cfw_rotr32(e, 11) ^
                      cfw_rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + cfw_sha256_k[i] + w[i];
        uint32_t s0 = cfw_rotr32(a, 2) ^ cfw_rotr32(a, 13) ^
                      cfw_rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void cfw_sha256_init(cfw_sha256_ctx_t *ctx)
{
    static const uint32_t initial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    if (ctx == NULL) {
        return;
    }
    memcpy(ctx->state, initial, sizeof(initial));
    ctx->total_len = 0;
    ctx->block_len = 0;
}

void cfw_sha256_update(cfw_sha256_ctx_t *ctx, const void *data,
                       size_t size)
{
    const unsigned char *p;
    size_t avail;

    if (ctx == NULL || (size != 0 && data == NULL)) {
        return;
    }
    p = (const unsigned char *)data;
    ctx->total_len += (uint64_t)size * 8u;

    if (ctx->block_len != 0) {
        avail = 64u - ctx->block_len;
        if (size < avail) {
            memcpy(ctx->block + ctx->block_len, p, size);
            ctx->block_len += size;
            return;
        }
        memcpy(ctx->block + ctx->block_len, p, avail);
        cfw_sha256_compress(ctx, ctx->block);
        p += avail;
        size -= avail;
        ctx->block_len = 0;
    }
    while (size >= 64u) {
        cfw_sha256_compress(ctx, p);
        p += 64u;
        size -= 64u;
    }
    if (size != 0) {
        memcpy(ctx->block, p, size);
        ctx->block_len = size;
    }
}

void cfw_sha256_final(cfw_sha256_ctx_t *ctx, unsigned char *out)
{
    uint64_t bit_len;
    unsigned char pad;
    unsigned int i;

    if (ctx == NULL || out == NULL) {
        return;
    }
    bit_len = ctx->total_len;

    /* Padding: 0x80, zeros until 56 mod 64, then 64-bit big-endian
     * bit length. */
    pad = 0x80u;
    cfw_sha256_update(ctx, &pad, 1);
    pad = 0x00u;
    while (ctx->block_len != 56u) {
        cfw_sha256_update(ctx, &pad, 1);
    }
    {
        unsigned char lenbuf[8];
        for (i = 0; i < 8; i++) {
            lenbuf[i] = (unsigned char)(bit_len >> (56u - i * 8u));
        }
        cfw_sha256_update(ctx, lenbuf, 8);
    }

    for (i = 0; i < 8; i++) {
        out[i * 4u] = (unsigned char)(ctx->state[i] >> 24);
        out[i * 4u + 1u] = (unsigned char)(ctx->state[i] >> 16);
        out[i * 4u + 2u] = (unsigned char)(ctx->state[i] >> 8);
        out[i * 4u + 3u] = (unsigned char)ctx->state[i];
    }
}

void cfw_sha256(const void *data, size_t size, unsigned char *out)
{
    cfw_sha256_ctx_t ctx;
    if (out == NULL) {
        return;
    }
    cfw_sha256_init(&ctx);
    cfw_sha256_update(&ctx, data, size);
    cfw_sha256_final(&ctx, out);
}

void cfw_hmac_sha256(const void *key, size_t key_size, const void *data,
                     size_t data_size, unsigned char *out)
{
    unsigned char kblock[64];
    unsigned char inner[CFW_SHA256_DIGEST_SIZE];
    cfw_sha256_ctx_t ctx;
    size_t i;

    if (out == NULL || (key_size != 0 && key == NULL) ||
        (data_size != 0 && data == NULL)) {
        return;
    }

    /* Keys longer than the block size are hashed first (RFC 2104). */
    if (key_size > 64u) {
        cfw_sha256(key, key_size, kblock);
        memset(kblock + CFW_SHA256_DIGEST_SIZE, 0,
               64u - CFW_SHA256_DIGEST_SIZE);
    } else {
        if (key_size != 0) {
            memcpy(kblock, key, key_size);
        }
        memset(kblock + key_size, 0, 64u - key_size);
    }

    /* Inner pad. */
    for (i = 0; i < 64u; i++) {
        kblock[i] ^= 0x36u;
    }
    cfw_sha256_init(&ctx);
    cfw_sha256_update(&ctx, kblock, 64u);
    cfw_sha256_update(&ctx, data, data_size);
    cfw_sha256_final(&ctx, inner);

    /* Outer pad: rebuild kblock with 0x5c (kblock currently holds the
     * inner-padded key; XOR difference 0x36^0x5c = 0x6a). */
    for (i = 0; i < 64u; i++) {
        kblock[i] ^= 0x6au;
    }
    cfw_sha256_init(&ctx);
    cfw_sha256_update(&ctx, kblock, 64u);
    cfw_sha256_update(&ctx, inner, sizeof(inner));
    cfw_sha256_final(&ctx, out);
}

int cfw_secure_equal(const void *a, const void *b, size_t size)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    unsigned char diff = 0;
    size_t i;

    for (i = 0; i < size; i++) {
        diff |= (unsigned char)(pa[i] ^ pb[i]);
    }
    return diff == 0;
}
