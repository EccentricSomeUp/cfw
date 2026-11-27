/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Coverage-guided fuzz target for the incremental HTTP head reader
 * (ADR 0077). The input is fed in content-dependent chunk sizes so
 * libFuzzer mutations also explore split-point variation across the
 * "\r\n\r\n" terminator search; completed heads go through the request
 * head parser and framing logic.
 */

#include <cfw/cfw.h>
#include <cfw/http.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CFW_FUZZ_HEAD_READER_MAX_INPUT 8192u
#define CFW_FUZZ_HEAD_READER_MAX_SIZE 8192u

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    cfw_http_head_reader_t reader;
    size_t pos = 0;

    if (size == 0 || size > CFW_FUZZ_HEAD_READER_MAX_INPUT) {
        return 0;
    }
    if (cfw_http_head_reader_init(&reader, CFW_FUZZ_HEAD_READER_MAX_SIZE) !=
        CFW_OK) {
        return 0;
    }

    /* Chunk sizes derive from the byte at the current position, so
     * mutations reshuffle the split points the terminator scan sees. */
    while (pos < size && !cfw_http_head_reader_is_complete(&reader)) {
        size_t step = 1u + (size_t)(data[pos] % 13u);
        cfw_span_t chunk;
        size_t consumed = 0;
        cfw_status_t st;

        if (step > size - pos) {
            step = size - pos;
        }
        cfw_span_init(&chunk, data + pos, step);
        st = cfw_http_head_reader_feed(&reader, &chunk, &consumed);
        if (st != CFW_OK) {
            break;
        }
        pos += step;
    }

    if (cfw_http_head_reader_is_complete(&reader)) {
        cfw_strview_t head;
        cfw_http_request_head_t parsed;
        cfw_http_message_framing_t framing;

        if (cfw_http_head_reader_head(&reader, &head) == CFW_OK &&
            cfw_http_request_head_parse(&head, &parsed) == CFW_OK) {
            (void)cfw_http_request_framing(&parsed, &framing);
        }
    }

    cfw_http_head_reader_free(&reader);
    return 0;
}
