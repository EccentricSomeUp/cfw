/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/buffer.h>

#include <cfw/checked_arithmetic.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int cfw_buffer_is_valid(const cfw_buffer_t *buffer)
{
    if (buffer == NULL || buffer->size > buffer->capacity) {
        return 0;
    }
    if (buffer->capacity == 0) {
        return buffer->data == NULL;
    }
    return buffer->data != NULL;
}

static size_t cfw_buffer_next_capacity(size_t current, size_t needed)
{
    size_t capacity = current == 0 ? needed : current;

    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            return needed;
        }
        capacity *= 2;
    }

    return capacity;
}

cfw_status_t cfw_buffer_init(cfw_buffer_t *buffer)
{
    if (buffer == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    buffer->data = NULL;
    buffer->size = 0;
    buffer->capacity = 0;
    return CFW_OK;
}

void cfw_buffer_free(cfw_buffer_t *buffer)
{
    if (buffer == NULL) {
        return;
    }

    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0;
    buffer->capacity = 0;
}

void cfw_buffer_clear(cfw_buffer_t *buffer)
{
    if (!cfw_buffer_is_valid(buffer)) {
        return;
    }

    buffer->size = 0;
}

cfw_status_t cfw_buffer_reserve(cfw_buffer_t *buffer, size_t capacity)
{
    size_t new_capacity;
    void *new_data;

    if (!cfw_buffer_is_valid(buffer)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (capacity <= buffer->capacity) {
        return CFW_OK;
    }

    new_capacity = cfw_buffer_next_capacity(buffer->capacity, capacity);
    new_data = realloc(buffer->data, new_capacity);
    if (new_data == NULL) {
        return CFW_ERROR_NOMEM;
    }

    buffer->data = (uint8_t *)new_data;
    buffer->capacity = new_capacity;
    return CFW_OK;
}

cfw_status_t cfw_buffer_append(cfw_buffer_t *buffer, const void *data,
                               size_t size)
{
    size_t needed;
    cfw_status_t status;

    if (!cfw_buffer_is_valid(buffer)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (size != 0 && data == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    status = cfw_checked_add_size(buffer->size, size, &needed);
    if (status != CFW_OK) {
        return status;
    }

    status = cfw_buffer_reserve(buffer, needed);
    if (status != CFW_OK) {
        return status;
    }

    if (size != 0) {
        memmove(buffer->data + buffer->size, data, size);
    }
    buffer->size = needed;
    return CFW_OK;
}

cfw_status_t cfw_buffer_span(const cfw_buffer_t *buffer, cfw_span_t *span)
{
    if (!cfw_buffer_is_valid(buffer)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    return cfw_span_init(span, buffer->data, buffer->size);
}
