/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_TIME_H
#define CFW_TIME_H

#include <cfw/export.h>
#include <cfw/status.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Durations and timepoints are plain values; no allocation or ownership is
 * involved. All timepoints are measured on a monotonic clock (never wall
 * time) so they are safe for timeouts and elapsed measurements. Timepoints
 * are opaque: they are only meaningful relative to other timepoints from the
 * same clock and must not be persisted or compared across process restarts.
 */

typedef struct cfw_duration {
    int64_t nanoseconds;
} cfw_duration_t;

typedef struct cfw_timepoint {
    int64_t nanoseconds;
} cfw_timepoint_t;

CFW_API cfw_status_t cfw_duration_from_nanos(int64_t value,
                                             cfw_duration_t *duration);
CFW_API cfw_status_t cfw_duration_from_micros(int64_t value,
                                              cfw_duration_t *duration);
CFW_API cfw_status_t cfw_duration_from_millis(int64_t value,
                                              cfw_duration_t *duration);
CFW_API cfw_status_t cfw_duration_from_seconds(int64_t value,
                                               cfw_duration_t *duration);
CFW_API cfw_duration_t cfw_duration_zero(void);
CFW_API cfw_status_t cfw_duration_add(const cfw_duration_t *a,
                                      const cfw_duration_t *b,
                                      cfw_duration_t *result);
CFW_API cfw_status_t cfw_duration_sub(const cfw_duration_t *a,
                                      const cfw_duration_t *b,
                                      cfw_duration_t *result);
CFW_API cfw_status_t cfw_duration_compare(const cfw_duration_t *a,
                                          const cfw_duration_t *b, int *result);
CFW_API cfw_status_t cfw_duration_to_nanos(const cfw_duration_t *duration,
                                           int64_t *result);
CFW_API cfw_status_t cfw_duration_to_micros(const cfw_duration_t *duration,
                                            int64_t *result);
CFW_API cfw_status_t cfw_duration_to_millis(const cfw_duration_t *duration,
                                            int64_t *result);

CFW_API cfw_status_t cfw_time_now(cfw_timepoint_t *timepoint);
CFW_API cfw_status_t cfw_timepoint_add(const cfw_timepoint_t *timepoint,
                                       const cfw_duration_t *duration,
                                       cfw_timepoint_t *result);
CFW_API cfw_status_t cfw_timepoint_sub(const cfw_timepoint_t *timepoint,
                                       const cfw_duration_t *duration,
                                       cfw_timepoint_t *result);
CFW_API cfw_status_t cfw_timepoint_diff(const cfw_timepoint_t *later,
                                        const cfw_timepoint_t *earlier,
                                        cfw_duration_t *duration);
CFW_API cfw_status_t cfw_timepoint_compare(const cfw_timepoint_t *a,
                                           const cfw_timepoint_t *b, int *result);

CFW_API cfw_status_t cfw_deadline_from_now(cfw_timepoint_t *deadline,
                                           cfw_duration_t timeout);
CFW_API cfw_status_t cfw_deadline_expired(const cfw_timepoint_t *deadline,
                                          int *expired);

#ifdef __cplusplus
}
#endif

#endif /* CFW_TIME_H */
