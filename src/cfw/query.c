/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/query.h>

/* Returns the hex value of c, or -1 when c is not an ASCII hex digit. */
static int cfw_query_hex(unsigned char c)
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

/* Decodes one raw name or value component [src, end) into *out,
 * advancing *out past the written bytes. '+' becomes SP and %XX
 * becomes the byte; anything else copies verbatim. Returns
 * CFW_ERROR_INVALID_ARGUMENT on a malformed percent escape. The
 * caller guarantees *out has room for at least (end - src) bytes,
 * which always suffices since decoding never expands. */
static cfw_status_t cfw_query_decode(const unsigned char *src,
                                     const unsigned char *end,
                                     char **out)
{
    char *w = *out;

    while (src < end) {
        unsigned char c = *src++;
        if (c == '+') {
            *w++ = ' ';
        } else if (c == '%') {
            int hi;
            int lo;
            if (end - src < 2) {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
            hi = cfw_query_hex(src[0]);
            lo = cfw_query_hex(src[1]);
            if (hi < 0 || lo < 0) {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
            *w++ = (char)((hi << 4) | lo);
            src += 2;
        } else {
            *w++ = (char)c;
        }
    }
    *out = w;
    return CFW_OK;
}

cfw_status_t cfw_query_parse(const cfw_strview_t *query,
                             cfw_query_pair_t *pairs,
                             size_t pair_capacity,
                             char *scratch, size_t scratch_size,
                             size_t *count_out,
                             size_t *dropped_out)
{
    size_t pos = 0;
    size_t stored = 0;
    size_t dropped = 0;
    char *out;

    if (query == NULL || count_out == NULL ||
        (query->size != 0 && query->data == NULL) ||
        (pair_capacity != 0 && pairs == NULL) ||
        (scratch_size != 0 && scratch == NULL) ||
        scratch_size < query->size) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    *count_out = 0;
    if (dropped_out != NULL) {
        *dropped_out = 0;
    }
    out = scratch;

    for (;;) {
        /* One segment runs to the next '&' or the end of input. */
        size_t seg_end = pos;
        size_t eq;
        size_t name_len;
        size_t value_len;
        char *name_at;
        char *value_at;
        cfw_status_t st;

        while (seg_end < query->size && query->data[seg_end] != '&') {
            seg_end++;
        }
        if (seg_end > pos) {
            /* Split at the first '='; a later '=' stays literal in
             * the value. */
            eq = pos;
            while (eq < seg_end && query->data[eq] != '=') {
                eq++;
            }
            name_at = out;
            st = cfw_query_decode(
                (const unsigned char *)query->data + pos,
                (const unsigned char *)query->data + eq, &out);
            if (st != CFW_OK) {
                return st;
            }
            name_len = (size_t)(out - name_at);
            value_at = out;
            if (eq < seg_end) {
                st = cfw_query_decode(
                    (const unsigned char *)query->data + eq + 1,
                    (const unsigned char *)query->data + seg_end,
                    &out);
            } else {
                st = CFW_OK;
            }
            if (st != CFW_OK) {
                return st;
            }
            value_len = (size_t)(out - value_at);

            if (stored < pair_capacity) {
                cfw_strview_init(&pairs[stored].name, name_at,
                                 name_len);
                cfw_strview_init(&pairs[stored].value, value_at,
                                 value_len);
                stored++;
            } else {
                dropped++;
            }
        }
        if (seg_end >= query->size) {
            break;
        }
        pos = seg_end + 1;
    }

    *count_out = stored;
    if (dropped_out != NULL) {
        *dropped_out = dropped;
    }
    return CFW_OK;
}
