/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/log.h>

#include <cfw/time.h>

#include <stdio.h>
#include <string.h>

#define CFW_LOG_FORMATTED_CAPACITY 1024u
#define CFW_LOG_WINDOW_NS INT64_C(1000000000)

static int cfw_log_level_is_valid(cfw_log_level_t level)
{
    return level >= CFW_LOG_TRACE && level <= CFW_LOG_FATAL;
}

static int cfw_log_window_allows(cfw_logger_t *logger)
{
    cfw_timepoint_t now;
    int64_t start;
    uint64_t count;
    cfw_status_t status;

    if (logger->rate_limit_per_second == 0) {
        return 1;
    }

    status = cfw_time_now(&now);
    if (status != CFW_OK) {
        /* Without a clock the rate limiter cannot function; fail open for
         * logging availability but count the message as unthrottled. */
        return 1;
    }

    start = atomic_load_explicit(&logger->window_start_ns,
                                 memory_order_relaxed);
    if (start == 0 || now.nanoseconds - start >= CFW_LOG_WINDOW_NS) {
        int64_t expected = start;
        if (atomic_compare_exchange_weak_explicit(
                &logger->window_start_ns, &expected, now.nanoseconds,
                memory_order_relaxed, memory_order_relaxed)) {
            atomic_store_explicit(&logger->window_count, 0,
                                  memory_order_relaxed);
        } else {
            start = expected;
        }
    }

    count = atomic_fetch_add_explicit(&logger->window_count, 1,
                                      memory_order_relaxed) +
            1;
    if (count > logger->rate_limit_per_second) {
        atomic_fetch_add_explicit(&logger->dropped_count, 1,
                                  memory_order_relaxed);
        return 0;
    }
    return 1;
}

void cfw_logger_init(cfw_logger_t *logger)
{
    if (logger == NULL) {
        return;
    }

    logger->sink = NULL;
    logger->user_data = NULL;
    logger->min_level = CFW_LOG_INFO;
    logger->rate_limit_per_second = 0;
    atomic_init(&logger->window_start_ns, 0);
    atomic_init(&logger->window_count, 0);
    atomic_init(&logger->dropped_count, 0);
}

void cfw_logger_set_sink(cfw_logger_t *logger, cfw_log_sink_fn sink,
                         void *user_data)
{
    if (logger == NULL) {
        return;
    }

    logger->sink = sink;
    logger->user_data = user_data;
}

void cfw_logger_set_min_level(cfw_logger_t *logger, cfw_log_level_t min_level)
{
    if (logger == NULL || !cfw_log_level_is_valid(min_level)) {
        return;
    }

    logger->min_level = min_level;
}

void cfw_logger_set_rate_limit(cfw_logger_t *logger,
                               uint32_t messages_per_second)
{
    if (logger == NULL) {
        return;
    }

    logger->rate_limit_per_second = messages_per_second;
}

uint64_t cfw_logger_dropped_count(cfw_logger_t *logger)
{
    if (logger == NULL) {
        return 0;
    }

    return (uint64_t)atomic_load_explicit(&logger->dropped_count,
                                          memory_order_relaxed);
}

cfw_status_t cfw_log_message(cfw_logger_t *logger, cfw_log_level_t level,
                             const cfw_strview_t *message)
{
    if (logger == NULL || !cfw_log_level_is_valid(level) || message == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (level < logger->min_level || logger->sink == NULL) {
        return CFW_OK;
    }
    if (!cfw_log_window_allows(logger)) {
        return CFW_OK;
    }

    logger->sink(level, message, logger->user_data);
    return CFW_OK;
}

cfw_status_t cfw_log(cfw_logger_t *logger, cfw_log_level_t level,
                     const char *format, ...)
{
    char buffer[CFW_LOG_FORMATTED_CAPACITY];
    cfw_strview_t message;
    va_list args;
    int needed;
    cfw_status_t status;

    if (logger == NULL || !cfw_log_level_is_valid(level) || format == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (level < logger->min_level || logger->sink == NULL) {
        return CFW_OK;
    }
    if (!cfw_log_window_allows(logger)) {
        return CFW_OK;
    }

    va_start(args, format);
    /* The format string is caller-supplied by design; call sites are
     * checked via the CFW_PRINTF_FORMAT attribute on cfw_log. MSVC has
     * no -Wformat-nonliteral equivalent and does not understand GCC
     * diagnostic pragmas. */
#if !defined(_MSC_VER)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    needed = vsnprintf(buffer, sizeof(buffer), format, args);
#if !defined(_MSC_VER)
#pragma GCC diagnostic pop
#endif
    va_end(args);

    if (needed < 0) {
        return CFW_ERROR_INTERNAL;
    }

    if ((size_t)needed >= sizeof(buffer)) {
        memcpy(buffer + sizeof(buffer) - 4, "...", 4);
    }

    status = cfw_strview_init(&message, buffer, strlen(buffer));
    if (status != CFW_OK) {
        return status;
    }

    logger->sink(level, &message, logger->user_data);
    return CFW_OK;
}
