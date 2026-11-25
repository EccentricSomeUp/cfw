/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/span.h>

cfw_status_t cfw_span_init(cfw_span_t *span, const void *data, size_t size)
{
    if (span == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (data == NULL && size != 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    span->data = (const uint8_t *)data;
    span->size = size;
    return CFW_OK;
}

cfw_status_t cfw_span_subspan(const cfw_span_t *span, size_t offset,
                              size_t length, cfw_span_t *result)
{
    size_t remaining;

    if (span == NULL || result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (span->size != 0 && span->data == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (offset > span->size) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    remaining = span->size - offset;
    if (length > remaining) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    if (span->data == NULL) {
        result->data = NULL;
    } else {
        result->data = span->data + offset;
    }
    result->size = length;
    return CFW_OK;
}
