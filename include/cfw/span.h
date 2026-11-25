/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_SPAN_H
#define CFW_SPAN_H

#include <cfw/status.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
