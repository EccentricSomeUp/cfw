/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Coverage-guided fuzz target for the query-pair parser (ADR 0077).
 * The input is treated as a raw query string; percent-escapes, bare
 * '&' separators, and missing/empty values are all in scope.
 */

#include <cfw/cfw.h>
#include <cfw/query.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    cfw_query_pair_t pairs[16];
    cfw_strview_t query;
    char *scratch;
    size_t count = 0;
    size_t dropped = 0;

    if (size > (1u << 20)) {
        return 0;
    }

    scratch = (char *)malloc(size + 1);
    if (scratch == NULL) {
        return 0;
    }
    memset(pairs, 0, sizeof(pairs));
    query.data = data;
    query.size = size;
    (void)cfw_query_parse(&query, pairs,
                          sizeof(pairs) / sizeof(pairs[0]), scratch,
                          size + 1, &count, &dropped);
    free(scratch);
    return 0;
}
