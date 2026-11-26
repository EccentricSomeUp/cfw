/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/ws.h>

#include <cfw/http.h>
#include <cfw/strview.h>

#include <string.h>

/* ------------------------------------------------------------------ */
/* SHA-1 (FIPS 180-4) — used only for the RFC 6455 accept key, which   */
/* is a protocol transform, not a security boundary.                   */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t h[5];
    uint64_t total;
    uint8_t block[64];
    size_t block_len;
} cfw_ws_sha1_t;

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

static void cfw_ws_sha1_init(cfw_ws_sha1_t *ctx)
{
    ctx->h[0] = 0x67452301u;
    ctx->h[1] = 0xefcdab89u;
    ctx->h[2] = 0x98badcfeu;
    ctx->h[3] = 0x10325476u;
    ctx->h[4] = 0xc3d2e1f0u;
    ctx->total = 0;
    ctx->block_len = 0;
}

static void cfw_ws_sha1_update(cfw_ws_sha1_t *ctx, const uint8_t *data,
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

static void cfw_ws_sha1_final(cfw_ws_sha1_t *ctx, uint8_t out[20])
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

/* ------------------------------------------------------------------ */
/* Base64 (RFC 4648) — decode for key validation, encode for accept.   */
/* ------------------------------------------------------------------ */

static int cfw_ws_b64_value(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

/* Decodes canonical base64 (with optional '=' padding) into out.
 * Returns decoded size or (size_t)-1 on malformed input. */
static size_t cfw_ws_b64_decode(const char *in, size_t in_size,
                                uint8_t *out, size_t out_cap)
{
    size_t i = 0;
    size_t o = 0;
    int pad = 0;

    while (i + 4 <= in_size) {
        int v[4];
        size_t j;
        uint32_t triple;
        int pads = 0;
        for (j = 0; j < 4; j++) {
            uint8_t c = (uint8_t)in[i + j];
            if (c == '=') {
                v[j] = -2;
                pads++;
            } else {
                v[j] = cfw_ws_b64_value(c);
                if (v[j] < 0 || pads != 0) {
                    return (size_t)-1;
                }
            }
        }
        if (pads > 2 || (pads != 0 && i + 4 != in_size)) {
            return (size_t)-1;
        }
        triple = ((uint32_t)(v[0] < 0 ? 0 : v[0]) << 18) |
                 ((uint32_t)(v[1] < 0 ? 0 : v[1]) << 12) |
                 ((uint32_t)(v[2] < 0 ? 0 : v[2]) << 6) |
                 (uint32_t)(v[3] < 0 ? 0 : v[3]);
        /* A padded group writes 3 - pads bytes, not always 3. */
        if (o + (3u - (size_t)pads) > out_cap) {
            return (size_t)-1;
        }
        out[o++] = (uint8_t)(triple >> 16);
        if (pads < 2) {
            out[o++] = (uint8_t)(triple >> 8);
        }
        if (pads < 1) {
            out[o++] = (uint8_t)triple;
        }
        pad += pads;
        i += 4;
        if (pads != 0) {
            break;
        }
    }
    if (i != in_size || pad > 2) {
        return (size_t)-1;
    }
    return o;
}

static const char cfw_ws_b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void cfw_ws_b64_encode(const uint8_t *in, size_t in_size, char *out)
{
    size_t i = 0;
    size_t o = 0;
    while (i + 3 <= in_size) {
        uint32_t t = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) |
                     (uint32_t)in[i + 2];
        out[o++] = cfw_ws_b64_alphabet[(t >> 18) & 63];
        out[o++] = cfw_ws_b64_alphabet[(t >> 12) & 63];
        out[o++] = cfw_ws_b64_alphabet[(t >> 6) & 63];
        out[o++] = cfw_ws_b64_alphabet[t & 63];
        i += 3;
    }
    if (i < in_size) {
        uint32_t t = (uint32_t)in[i] << 16;
        int rem = (int)(in_size - i);
        if (rem == 2) {
            t |= (uint32_t)in[i + 1] << 8;
        }
        out[o++] = cfw_ws_b64_alphabet[(t >> 18) & 63];
        out[o++] = cfw_ws_b64_alphabet[(t >> 12) & 63];
        out[o++] = rem == 2 ? cfw_ws_b64_alphabet[(t >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* Header helpers                                                      */
/* ------------------------------------------------------------------ */

static int cfw_ws_header_is(const cfw_http_header_t *header,
                            const char *name)
{
    cfw_strview_t hn;
    int eq = 0;
    cfw_strview_from_cstr(&hn, name);
    if (cfw_strview_equal_nocase(&header->name, &hn, &eq) != CFW_OK ||
        !eq) {
        return 0;
    }
    return 1;
}

/* Returns the header value span for `name` (first match), or NULL. */
static const cfw_strview_t *cfw_ws_header_find(const cfw_request_t *req,
                                               const char *name)
{
    size_t i;
    for (i = 0; i < req->header_count; i++) {
        if (cfw_ws_header_is(&req->headers[i], name)) {
            return &req->headers[i].value;
        }
    }
    return NULL;
}

/* Case-insensitive token membership in a comma-separated header value
 * (OWS trimmed), e.g. Connection: keep-alive, Upgrade. */
static int cfw_ws_value_has_token(const cfw_strview_t *value,
                                  const char *token)
{
    cfw_strview_t wanted;
    size_t pos = 0;
    cfw_strview_from_cstr(&wanted, token);

    while (pos <= value->size) {
        size_t ts = pos;
        size_t te;
        cfw_strview_t element;
        int eq = 0;
        while (pos < value->size && value->data[pos] != ',') {
            pos++;
        }
        te = pos;
        if (pos < value->size) {
            pos++;
        }
        while (ts < te &&
               (value->data[ts] == ' ' || value->data[ts] == '\t')) {
            ts++;
        }
        while (te > ts &&
               (value->data[te - 1] == ' ' || value->data[te - 1] == '\t')) {
            te--;
        }
        cfw_strview_init(&element, value->data + ts, te - ts);
        if (cfw_strview_equal_nocase(&element, &wanted, &eq) == CFW_OK &&
            eq) {
            return 1;
        }
        if (pos >= value->size) {
            break;
        }
    }
    return 0;
}

static int cfw_ws_strview_is(const cfw_strview_t *view, const char *text)
{
    cfw_strview_t wanted;
    int eq = 0;
    cfw_strview_from_cstr(&wanted, text);
    return cfw_strview_equal_nocase(view, &wanted, &eq) == CFW_OK && eq;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

cfw_status_t cfw_ws_handshake_validate(const cfw_request_t *req)
{
    const cfw_strview_t *upgrade;
    const cfw_strview_t *connection;
    const cfw_strview_t *version;
    const cfw_strview_t *key;
    uint8_t decoded[16];

    if (req == NULL || req->method != CFW_HTTP_METHOD_GET) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    upgrade = cfw_ws_header_find(req, "upgrade");
    if (upgrade == NULL || !cfw_ws_strview_is(upgrade, "websocket")) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    connection = cfw_ws_header_find(req, "connection");
    if (connection == NULL ||
        !cfw_ws_value_has_token(connection, "upgrade")) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    version = cfw_ws_header_find(req, "sec-websocket-version");
    if (version == NULL || version->size != 2 ||
        !(version->data[0] == '1' && version->data[1] == '3')) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    key = cfw_ws_header_find(req, "sec-websocket-key");
    if (key == NULL || key->size == 0 || key->size > 256) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (cfw_ws_b64_decode(key->data, key->size, decoded,
                          sizeof(decoded)) != 16) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    return CFW_OK;
}

cfw_status_t cfw_ws_accept_key(const char *key, size_t key_size, char *out,
                               size_t out_size)
{
    static const char guid[] =
        "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    cfw_ws_sha1_t sha;
    uint8_t digest[20];
    char material[256 + sizeof(guid) - 1];
    size_t mlen;

    if (key == NULL || key_size == 0 || key_size > 256 || out == NULL ||
        out_size < CFW_WS_ACCEPT_KEY_CAPACITY) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    mlen = key_size;
    memcpy(material, key, key_size);
    memcpy(material + key_size, guid, sizeof(guid) - 1);
    mlen += sizeof(guid) - 1;

    cfw_ws_sha1_init(&sha);
    cfw_ws_sha1_update(&sha, (const uint8_t *)material, mlen);
    cfw_ws_sha1_final(&sha, digest);
    cfw_ws_b64_encode(digest, sizeof(digest), out);
    return CFW_OK;
}

cfw_status_t cfw_ws_handshake_accept(const cfw_request_t *req, char *out,
                                     size_t out_size)
{
    const cfw_strview_t *key;

    if (req == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    key = cfw_ws_header_find(req, "sec-websocket-key");
    if (key == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    return cfw_ws_accept_key(key->data, key->size, out, out_size);
}
