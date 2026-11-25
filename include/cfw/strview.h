/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_STRVIEW_H
#define CFW_STRVIEW_H

#include <cfw/span.h>
#include <cfw/status.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cfw_strview_t is a non-owning, length-delimited view over bytes that are
 * interpreted as text (typically ASCII or UTF-8). The view never validates
 * encoding; protocol layers decide what byte sequences are acceptable.
 * A NULL data pointer with size 0 is valid and denotes the empty view.
 */

typedef struct cfw_strview {
    const char *data;
    size_t size;
} cfw_strview_t;

CFW_API cfw_status_t cfw_strview_init(cfw_strview_t *view, const void *data,
                                      size_t size);
CFW_API cfw_status_t cfw_strview_from_cstr(cfw_strview_t *view,
                                           const char *cstr);
CFW_API cfw_status_t cfw_strview_from_span(const cfw_span_t *span,
                                           cfw_strview_t *view);
CFW_API cfw_status_t cfw_strview_to_span(const cfw_strview_t *view,
                                         cfw_span_t *span);

CFW_API int cfw_strview_is_empty(const cfw_strview_t *view);
CFW_API cfw_status_t cfw_strview_equal(const cfw_strview_t *a,
                                       const cfw_strview_t *b, int *equal);
CFW_API cfw_status_t cfw_strview_equal_nocase(const cfw_strview_t *a,
                                              const cfw_strview_t *b,
                                              int *equal);
CFW_API cfw_status_t cfw_strview_starts_with(const cfw_strview_t *view,
                                             const cfw_strview_t *prefix,
                                             int *matches);
CFW_API cfw_status_t cfw_strview_ends_with(const cfw_strview_t *view,
                                           const cfw_strview_t *suffix,
                                           int *matches);

#ifdef __cplusplus
}
#endif

#endif /* CFW_STRVIEW_H */
