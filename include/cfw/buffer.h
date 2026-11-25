/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_BUFFER_H
#define CFW_BUFFER_H

#include <cfw/span.h>
#include <cfw/status.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cfw_buffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
} cfw_buffer_t;

CFW_API cfw_status_t cfw_buffer_init(cfw_buffer_t *buffer);
CFW_API void cfw_buffer_free(cfw_buffer_t *buffer);
CFW_API void cfw_buffer_clear(cfw_buffer_t *buffer);
CFW_API cfw_status_t cfw_buffer_reserve(cfw_buffer_t *buffer, size_t capacity);
CFW_API cfw_status_t cfw_buffer_append(cfw_buffer_t *buffer, const void *data,
                                       size_t size);
CFW_API cfw_status_t cfw_buffer_span(const cfw_buffer_t *buffer,
                                     cfw_span_t *span);

#ifdef __cplusplus
}
#endif

#endif /* CFW_BUFFER_H */
