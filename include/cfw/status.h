/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_STATUS_H
#define CFW_STATUS_H

#include <cfw/export.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cfw_status_t is the single error channel for the public API. CFW_OK is
 * zero; every other value denotes a failure class. Enumerator values are
 * append-only and never change meaning.
 *
 * cfw_status_string returns a static, never-NULL string; unrecognized
 * values map to "unknown".
 */
typedef enum cfw_status {
    CFW_OK = 0,
    CFW_ERROR_INVALID_ARGUMENT,
    CFW_ERROR_NOMEM,
    CFW_ERROR_IO,
    CFW_ERROR_TIMEOUT,
    CFW_ERROR_UNAVAILABLE,
    CFW_ERROR_INTERNAL,
    CFW_ERROR_OVERFLOW,
    CFW_ERROR_WOULDBLOCK
} cfw_status_t;

CFW_API const char *cfw_status_string(cfw_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* CFW_STATUS_H */
