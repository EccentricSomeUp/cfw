/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Coverage-guided fuzz target for the multipart parser (ADR 0077).
 * The input is exercised two ways: as a Content-Type header value for
 * cfw_multipart_boundary, and as a request body parsed against a fixed
 * boundary — seeds and mutations supply the delimiter bytes.
 */

#include <cfw/cfw.h>
#include <cfw/multipart.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const char boundary_str[] = "cfwfuzzboundary";
    cfw_strview_t boundary;
    cfw_strview_t content_type;
    cfw_span_t body;
    cfw_multipart_part_t parts[8];
    char *scratch;
    size_t count = 0;
    size_t dropped = 0;

    cfw_strview_from_cstr(&boundary, boundary_str);

    /* Boundary extraction over the input as a Content-Type value. */
    if (size <= 4096) {
        content_type.data = data;
        content_type.size = size;
        (void)cfw_multipart_boundary(&content_type, &boundary);
    }

    /* Body parse against the fixed boundary. Scratch must be at
     * least body->size per the API contract. */
    if (size <= (1u << 20)) {
        scratch = (char *)malloc(size + 1);
        if (scratch != NULL) {
            memset(parts, 0, sizeof(parts));
            cfw_span_init(&body, data, size);
            (void)cfw_multipart_parse(&body, &boundary, parts,
                                      sizeof(parts) / sizeof(parts[0]),
                                      scratch, size + 1, &count, &dropped);
            free(scratch);
        }
    }
    return 0;
}
