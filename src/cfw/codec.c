/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/codec.h>

#include <string.h>

/* ------------------------------------------------------------------ */
/* Base64 (RFC 4648 section 4, with padding)                           */
/* ------------------------------------------------------------------ */

static const char cfw_b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Returns the sextet value of c, or -1 when c is not in the alphabet. */
static int cfw_b64_value(unsigned char c)
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

size_t cfw_base64_encode_bound(size_t data_size)
{
    /* 4*ceil(n/3) encoded chars + NUL. The conservative bound keeps
     * both the +2 grouping and the *4 scaling far from SIZE_MAX. */
    if (data_size > ((SIZE_MAX - 1u) / 4u) * 3u) {
        return SIZE_MAX;
    }
    return ((data_size + 2u) / 3u) * 4u + 1u;
}

cfw_status_t cfw_base64_encode(const void *data, size_t data_size,
                               char *out, size_t cap, size_t *written)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t need;
    size_t i = 0;
    size_t o = 0;

    if (out == NULL || (data_size != 0 && data == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    need = cfw_base64_encode_bound(data_size);
    if (need == SIZE_MAX || cap < need) {
        return CFW_ERROR_OVERFLOW;
    }
    while (i + 3u <= data_size) {
        uint32_t t = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) |
                     (uint32_t)p[i + 2];
        out[o++] = cfw_b64_alphabet[(t >> 18) & 63u];
        out[o++] = cfw_b64_alphabet[(t >> 12) & 63u];
        out[o++] = cfw_b64_alphabet[(t >> 6) & 63u];
        out[o++] = cfw_b64_alphabet[t & 63u];
        i += 3u;
    }
    if (i < data_size) {
        uint32_t t = (uint32_t)p[i] << 16;
        size_t rem = data_size - i;
        if (rem == 2u) {
            t |= (uint32_t)p[i + 1] << 8;
        }
        out[o++] = cfw_b64_alphabet[(t >> 18) & 63u];
        out[o++] = cfw_b64_alphabet[(t >> 12) & 63u];
        out[o++] = rem == 2u ? cfw_b64_alphabet[(t >> 6) & 63u] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    if (written != NULL) {
        *written = o;
    }
    return CFW_OK;
}

cfw_status_t cfw_base64_decode(const cfw_strview_t *in,
                               unsigned char *out, size_t cap,
                               size_t *written)
{
    size_t i;
    size_t o = 0;

    if (in == NULL || (in->size != 0 && in->data == NULL) ||
        (out == NULL && in->size != 0)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    /* Canonical base64 length is a multiple of 4 (padding included). */
    if (in->size % 4u != 0u) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    for (i = 0; i < in->size; i += 4u) {
        int v[4] = {0, 0, 0, 0};
        unsigned int pads = 0;
        size_t j;
        uint32_t triple;
        size_t group_bytes;
        int last_group = (i + 4u == in->size);

        for (j = 0; j < 4u; j++) {
            unsigned char c = (unsigned char)in->data[i + j];
            if (c == '=') {
                /* '=' only allowed as the trailing 1-2 chars of the
                 * final group. */
                if (!last_group || j < 2u) {
                    return CFW_ERROR_INVALID_ARGUMENT;
                }
                pads++;
            } else {
                if (pads != 0u) {
                    return CFW_ERROR_INVALID_ARGUMENT;
                }
                v[j] = cfw_b64_value(c);
                if (v[j] < 0) {
                    return CFW_ERROR_INVALID_ARGUMENT;
                }
            }
        }
        if (pads > 2u) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        /* Canonical pad bits must be zero: with "xx==" the low 4 bits
         * of sextet 1 are unused; with "xxx=" the low 2 bits of
         * sextet 2 are unused. */
        if (pads == 2u && (v[1] & 0x0f) != 0) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        if (pads == 1u && (v[2] & 0x03) != 0) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }

        triple = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) |
                 ((uint32_t)v[2] << 6) | (uint32_t)v[3];
        group_bytes = 3u - pads;
        if (o + group_bytes > cap) {
            return CFW_ERROR_OVERFLOW;
        }
        out[o++] = (unsigned char)(triple >> 16);
        if (group_bytes > 1u) {
            out[o++] = (unsigned char)(triple >> 8);
        }
        if (group_bytes > 2u) {
            out[o++] = (unsigned char)triple;
        }
    }
    if (written != NULL) {
        *written = o;
    }
    return CFW_OK;
}

/* ------------------------------------------------------------------ */
/* Hex                                                                  */
/* ------------------------------------------------------------------ */

static const char cfw_hex_digits[] = "0123456789abcdef";

/* Returns the nibble value of c (either case), or -1. */
static int cfw_hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

size_t cfw_hex_encode_bound(size_t data_size)
{
    if (data_size > (SIZE_MAX - 1u) / 2u) {
        return SIZE_MAX;
    }
    return data_size * 2u + 1u;
}

cfw_status_t cfw_hex_encode(const void *data, size_t data_size,
                            char *out, size_t cap, size_t *written)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t need;
    size_t i;
    size_t o = 0;

    if (out == NULL || (data_size != 0 && data == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    need = cfw_hex_encode_bound(data_size);
    if (need == SIZE_MAX || cap < need) {
        return CFW_ERROR_OVERFLOW;
    }
    for (i = 0; i < data_size; i++) {
        out[o++] = cfw_hex_digits[p[i] >> 4];
        out[o++] = cfw_hex_digits[p[i] & 0x0fu];
    }
    out[o] = '\0';
    if (written != NULL) {
        *written = o;
    }
    return CFW_OK;
}

cfw_status_t cfw_hex_decode(const cfw_strview_t *in,
                            unsigned char *out, size_t cap,
                            size_t *written)
{
    size_t i;
    size_t o = 0;

    if (in == NULL || (in->size != 0 && in->data == NULL) ||
        (out == NULL && in->size != 0)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (in->size % 2u != 0u) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    /* Validate every digit before writing so malformed input never
     * leaves partial output. */
    for (i = 0; i < in->size; i++) {
        if (cfw_hex_value((unsigned char)in->data[i]) < 0) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
    }
    if (in->size / 2u > cap) {
        return CFW_ERROR_OVERFLOW;
    }
    for (i = 0; i < in->size; i += 2u) {
        out[o++] = (unsigned char)((cfw_hex_value((unsigned char)in->data[i])
                                    << 4) |
                                   cfw_hex_value((unsigned char)in->data[i + 1]));
    }
    if (written != NULL) {
        *written = o;
    }
    return CFW_OK;
}
