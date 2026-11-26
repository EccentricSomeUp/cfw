/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/query.h>
#include <cfw/uri.h>

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
    char *scratch_end;

    /* Decoded components are packed back-to-back; each decode may
     * transiently write a terminator that the next decode overwrites.
     * scratch_size >= query->size + 1 therefore always suffices. */
    if (query == NULL || count_out == NULL ||
        (query->size != 0 && query->data == NULL) ||
        (pair_capacity != 0 && pairs == NULL) ||
        (scratch_size != 0 && scratch == NULL) ||
        query->size == (size_t)-1 ||
        (query->size != 0 && scratch_size < query->size + 1)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    *count_out = 0;
    if (dropped_out != NULL) {
        *dropped_out = 0;
    }
    out = scratch;
    scratch_end = scratch != NULL ? scratch + scratch_size : NULL;

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
            st = cfw_uri_decode(query->data + pos, eq - pos, out,
                                (size_t)(scratch_end - out), &name_len,
                                CFW_URI_DECODE_PLUS_AS_SPACE);
            if (st != CFW_OK) {
                return st;
            }
            out += name_len;
            value_at = out;
            if (eq < seg_end) {
                st = cfw_uri_decode(query->data + eq + 1,
                                    seg_end - eq - 1, out,
                                    (size_t)(scratch_end - out),
                                    &value_len,
                                    CFW_URI_DECODE_PLUS_AS_SPACE);
            } else {
                st = CFW_OK;
                value_len = 0;
            }
            if (st != CFW_OK) {
                return st;
            }
            out += value_len;

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
