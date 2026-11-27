/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "ws_proto.h"

#include <string.h>

static uint32_t cfw_ws_rotr(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static void cfw_ws_sha1_block(cfw_ws_sha1_t *ctx, const uint8_t *p)
{
    uint32_t w[80];
    uint32_t a, b, c, d, e;
    size_t i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) |
               ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (i = 16; i < 80; i++) {
        /* ROTL1(x) == ROTR31(x). */
        w[i] = cfw_ws_rotr(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16],
                           31);
    }

    a = ctx->h[0];
    b = ctx->h[1];
    c = ctx->h[2];
    d = ctx->h[3];
    e = ctx->h[4];

    for (i = 0; i < 80; i++) {
        uint32_t f, k, tmp;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
        }
        /* ROTL5(a) == ROTR27(a); ROTL30(b) == ROTR2(b). */
        tmp = cfw_ws_rotr(a, 27) + f + e + k + w[i];
        e = d;
        d = c;
        c = cfw_ws_rotr(b, 2);
        b = a;
        a = tmp;
    }

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
}

void cfw_ws_sha1_init(cfw_ws_sha1_t *ctx)
{
    ctx->h[0] = 0x67452301u;
    ctx->h[1] = 0xefcdab89u;
    ctx->h[2] = 0x98badcfeu;
    ctx->h[3] = 0x10325476u;
    ctx->h[4] = 0xc3d2e1f0u;
    ctx->total = 0;
    ctx->block_len = 0;
}

void cfw_ws_sha1_update(cfw_ws_sha1_t *ctx, const uint8_t *data,
                        size_t size)
{
    ctx->total += (uint64_t)size * 8u;
    while (size != 0) {
        size_t n = 64u - ctx->block_len;
        if (n > size) {
            n = size;
        }
        memcpy(ctx->block + ctx->block_len, data, n);
        ctx->block_len += n;
        data += n;
        size -= n;
        if (ctx->block_len == 64u) {
            cfw_ws_sha1_block(ctx, ctx->block);
            ctx->block_len = 0;
        }
    }
}

void cfw_ws_sha1_final(cfw_ws_sha1_t *ctx, uint8_t out[20])
{
    uint64_t bits = ctx->total;
    uint8_t pad = 0x80;
    uint8_t len[8];
    size_t i;

    for (i = 0; i < 8; i++) {
        len[i] = (uint8_t)(bits >> (56u - i * 8u));
    }
    cfw_ws_sha1_update(ctx, &pad, 1);
    pad = 0;
    while (ctx->block_len != 56u) {
        cfw_ws_sha1_update(ctx, &pad, 1);
    }
    cfw_ws_sha1_update(ctx, len, 8);
    for (i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)ctx->h[i];
    }
}

int cfw_ws_utf8_valid(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        uint32_t cp;
        size_t need;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            cp = c & 0x1f;
            need = 1;
        } else if (c >= 0xe0 && c <= 0xef) {
            cp = c & 0x0f;
            need = 2;
        } else if (c >= 0xf0 && c <= 0xf4) {
            cp = c & 0x07;
            need = 3;
        } else {
            return 0;
        }
        if (i + need >= n) {
            return 0;
        }
        for (size_t k = 1; k <= need; k++) {
            uint8_t cc = s[i + k];
            if ((cc & 0xc0) != 0x80) {
                return 0;
            }
            cp = (cp << 6) | (cc & 0x3f);
        }
        /* Overlong, surrogate, and range checks. */
        if ((need == 2 && cp < 0x800) ||
            (need == 3 && cp < 0x10000) ||
            (need == 4 && cp < 0x10000) ||
            cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            return 0;
        }
        i += need + 1;
    }
    return 1;
}

int cfw_ws_close_status_ok(uint16_t status)
{
    if (status < 1000 || status > 4999) {
        return 0;
    }
    if (status == 1004 || status == 1005 || status == 1006 ||
        status == 1015 || status == 1016) {
        return 0;
    }
    if (status >= 1016 && status <= 2999) {
        return 0; /* reserved for future protocol use */
    }
    return 1;
}
