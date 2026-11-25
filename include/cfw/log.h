/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_LOG_H
#define CFW_LOG_H

#include <cfw/export.h>
#include <cfw/status.h>
#include <cfw/strview.h>

#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cfw_log_level {
    CFW_LOG_TRACE = 0,
    CFW_LOG_DEBUG,
    CFW_LOG_INFO,
    CFW_LOG_WARN,
    CFW_LOG_ERROR,
    CFW_LOG_FATAL
} cfw_log_level_t;

/*
 * Sink contract: called synchronously from cfw_log for each emitted message.
 * The message view is valid only for the duration of the call; sinks that
 * retain it must copy the bytes. Sinks must not call back into the same
 * logger. The sink must not block indefinitely; a slow sink stalls the
 * calling thread. Sinks are responsible for their own thread safety when
 * cfw_log may be invoked concurrently.
 *
 * Redaction rule: callers must never format secrets, credentials, tokens,
 * or key material into log messages. The facade performs no redaction;
 * structured logging with enforced redaction is a future, separately
 * reviewed feature.
 */
typedef void (*cfw_log_sink_fn)(cfw_log_level_t level,
                                const cfw_strview_t *message, void *user_data);

/*
 * cfw_logger_t is a public value type. Initialize with cfw_logger_init
 * before use; do not copy a logger while it may be in use. All fields are
 * managed by the API; callers must not modify them directly.
 */
typedef struct cfw_logger {
    cfw_log_sink_fn sink;
    void *user_data;
    cfw_log_level_t min_level;
    uint32_t rate_limit_per_second;
    atomic_int_fast64_t window_start_ns;
    atomic_uint_fast64_t window_count;
    atomic_uint_fast64_t dropped_count;
} cfw_logger_t;

CFW_API void cfw_logger_init(cfw_logger_t *logger);
CFW_API void cfw_logger_set_sink(cfw_logger_t *logger, cfw_log_sink_fn sink,
                                 void *user_data);
CFW_API void cfw_logger_set_min_level(cfw_logger_t *logger,
                                      cfw_log_level_t min_level);
CFW_API void cfw_logger_set_rate_limit(cfw_logger_t *logger,
                                       uint32_t messages_per_second);
CFW_API uint64_t cfw_logger_dropped_count(cfw_logger_t *logger);

CFW_API cfw_status_t cfw_log(cfw_logger_t *logger, cfw_log_level_t level,
                             const char *format, ...);
CFW_API cfw_status_t cfw_log_message(cfw_logger_t *logger,
                                     cfw_log_level_t level,
                                     const cfw_strview_t *message);

#ifdef __cplusplus
}
#endif

#endif /* CFW_LOG_H */
