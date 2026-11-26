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

/*
 * cfw_buffer_t is an owning, resizable byte buffer with checked doubling
 * growth. cfw_buffer_init must succeed before any other call;
 * cfw_buffer_free releases all storage, is idempotent, and must be called
 * exactly once per initialized buffer. cfw_buffer_clear keeps the
 * allocation but resets size to 0. cfw_buffer_span exposes a non-owning
 * cfw_span_t view that stays valid until the next call that may reallocate
 * or mutate the buffer.
 *
 * The buffer is not thread-safe; concurrent use requires external
 * synchronization. Growth and append are checked: requests that cannot be
 * satisfied return CFW_ERROR_OVERFLOW or CFW_ERROR_NOMEM rather than
 * wrapping.
 */
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
