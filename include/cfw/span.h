/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_SPAN_H
#define CFW_SPAN_H

#include <cfw/status.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cfw_span_t is a non-owning view over a byte range. The pointed-to bytes
 * must outlive every use of the span; the span never frees or mutates
 * them. A NULL data pointer is valid only when size is 0.
 *
 * These are pure functions with no side effects and no allocation; they
 * are safe to call from any thread. On failure the result parameter is
 * left untouched.
 */
typedef struct cfw_span {
    const uint8_t *data;
    size_t size;
} cfw_span_t;

CFW_API cfw_status_t cfw_span_init(cfw_span_t *span, const void *data,
                                   size_t size);
CFW_API cfw_status_t cfw_span_subspan(const cfw_span_t *span, size_t offset,
                                      size_t length, cfw_span_t *result);

#ifdef __cplusplus
}
#endif

#endif /* CFW_SPAN_H */
