/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/uri.h>

/* Returns the hex value of c, or -1 when c is not an ASCII hex digit. */
static int cfw_uri_hex(unsigned char c)
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

cfw_status_t cfw_uri_decode(const char *src, size_t len, char *dst,
                           size_t cap, size_t *out_len, uint32_t flags)
{
    size_t i = 0;
    size_t o = 0;

    if ((len != 0 && src == NULL) || dst == NULL || cap == 0 ||
        out_len == NULL || (flags & ~(CFW_URI_DECODE_PLUS_AS_SPACE |
                                      CFW_URI_DECODE_REJECT_NUL |
                                      CFW_URI_DECODE_REJECT_BACKSLASH)) !=
                               0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    while (i < len) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%' &&
            (i + 2 >= len || cfw_uri_hex((unsigned char)src[i + 1]) < 0 ||
             cfw_uri_hex((unsigned char)src[i + 2]) < 0)) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        if (c == '%') {
            int hi = cfw_uri_hex((unsigned char)src[i + 1]);
            int lo = cfw_uri_hex((unsigned char)src[i + 2]);
            c = (unsigned char)((hi << 4) | lo);
            i += 3;
        } else {
            i += 1;
        }
        if (c == '\0' && (flags & CFW_URI_DECODE_REJECT_NUL) != 0) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        if (c == '\\' && (flags & CFW_URI_DECODE_REJECT_BACKSLASH) != 0) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        if ((flags & CFW_URI_DECODE_PLUS_AS_SPACE) != 0 && c == '+') {
            c = ' ';
        }
        if (o + 1 >= cap) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        dst[o++] = (char)c;
    }
    dst[o] = '\0';
    *out_len = o;
    return CFW_OK;
}
