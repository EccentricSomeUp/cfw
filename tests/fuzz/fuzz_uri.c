/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Coverage-guided fuzz target for the URI percent-decoder, the
 * request-target path/query splitters, and the query-pair parser
 * (ADR 0077). Inputs are treated both as raw %-encoded byte strings
 * and as request targets / query strings.
 */

#include <cfw/cfw.h>
#include <cfw/http.h>
#include <cfw/query.h>
#include <cfw/uri.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* cfw_uri_decode: output never exceeds input, so size+1 suffices. */
    char *dst = (char *)malloc(size + 1);
    if (dst != NULL) {
        static const uint32_t flag_sets[] = {
            0u,
            CFW_URI_DECODE_PLUS_AS_SPACE,
            CFW_URI_DECODE_REJECT_NUL,
            CFW_URI_DECODE_REJECT_BACKSLASH,
            CFW_URI_DECODE_PLUS_AS_SPACE | CFW_URI_DECODE_REJECT_NUL |
                CFW_URI_DECODE_REJECT_BACKSLASH,
        };
        size_t out_len = 0;
        unsigned f;

        for (f = 0; f < sizeof(flag_sets) / sizeof(flag_sets[0]); f++) {
            (void)cfw_uri_decode((const char *)data, size, dst, size + 1,
                                 &out_len, flag_sets[f]);
        }
        free(dst);
    }

    /* Request-target splitters over the input as a target strview. */
    {
        cfw_strview_t target;
        cfw_strview_t path;
        cfw_strview_t query;
        cfw_query_pair_t pairs[8];
        char *scratch;
        size_t count = 0;
        size_t dropped = 0;

        if (size <= 8192) {
            target.data = data;
            target.size = size;
            (void)cfw_http_target_path(&target, &path);
            if (cfw_http_target_query(&target, &query) == CFW_OK &&
                query.size > 0) {
                scratch = (char *)malloc(query.size + 1);
                if (scratch != NULL) {
                    (void)cfw_query_parse(&query, pairs,
                                          sizeof(pairs) /
                                              sizeof(pairs[0]),
                                          scratch, query.size + 1, &count,
                                          &dropped);
                    free(scratch);
                }
            }
        }
    }
    return 0;
}
