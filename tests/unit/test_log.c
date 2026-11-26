/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <stdio.h>
#include <string.h>

#define CFW_TEST_MAX_MESSAGES 16

typedef struct cfw_test_sink {
    cfw_log_level_t levels[CFW_TEST_MAX_MESSAGES];
    size_t sizes[CFW_TEST_MAX_MESSAGES];
    char messages[CFW_TEST_MAX_MESSAGES][1100];
    size_t count;
} cfw_test_sink_t;

static void cfw_test_sink_fn(cfw_log_level_t level,
                             const cfw_strview_t *message, void *user_data)
{
    cfw_test_sink_t *sink = (cfw_test_sink_t *)user_data;

    if (sink->count >= CFW_TEST_MAX_MESSAGES) {
        return;
    }
    sink->levels[sink->count] = level;
    sink->sizes[sink->count] = message->size;
    if (message->size >= sizeof(sink->messages[0])) {
        memcpy(sink->messages[sink->count], message->data,
               sizeof(sink->messages[0]) - 1);
        sink->messages[sink->count][sizeof(sink->messages[0]) - 1] = '\0';
    } else {
        memcpy(sink->messages[sink->count], message->data, message->size);
        sink->messages[sink->count][message->size] = '\0';
    }
    sink->count++;
}

static void cfw_test_logger_setup(cfw_logger_t *logger, cfw_test_sink_t *sink)
{
    memset(sink, 0, sizeof(*sink));
    cfw_logger_init(logger);
    cfw_logger_set_sink(logger, cfw_test_sink_fn, sink);
}

static int test_log_init_defaults(void)
{
    cfw_logger_t logger;
    cfw_strview_t msg;

    cfw_logger_init(&logger);
    CFW_TEST_ASSERT(logger.sink == NULL);
    CFW_TEST_ASSERT_EQ(logger.min_level, CFW_LOG_INFO);
    CFW_TEST_ASSERT_EQ(logger.rate_limit_per_second, 0);
    CFW_TEST_ASSERT_EQ(cfw_logger_dropped_count(&logger), 0);

    /* Without a sink, messages are accepted but dropped silently. */
    CFW_TEST_ASSERT_EQ(cfw_strview_from_cstr(&msg, "x"), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_log_message(&logger, CFW_LOG_ERROR, &msg), CFW_OK);

    cfw_logger_init(NULL);
    cfw_logger_set_sink(NULL, cfw_test_sink_fn, NULL);
    cfw_logger_set_min_level(NULL, CFW_LOG_DEBUG);
    cfw_logger_set_rate_limit(NULL, 1);
    CFW_TEST_ASSERT_EQ(cfw_logger_dropped_count(NULL), 0);
    return 0;
}

static int test_log_level_filtering(void)
{
    cfw_logger_t logger;
    cfw_test_sink_t sink;

    cfw_test_logger_setup(&logger, &sink);
    logger.min_level = CFW_LOG_WARN;

    CFW_TEST_ASSERT_EQ(cfw_log(&logger, CFW_LOG_INFO, "info"), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_log(&logger, CFW_LOG_WARN, "warn"), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_log(&logger, CFW_LOG_ERROR, "error"), CFW_OK);
    CFW_TEST_ASSERT_EQ(sink.count, 2);
    CFW_TEST_ASSERT_EQ(sink.levels[0], CFW_LOG_WARN);
    CFW_TEST_ASSERT_EQ(sink.levels[1], CFW_LOG_ERROR);
    return 0;
}

static int test_log_formatted_message(void)
{
    cfw_logger_t logger;
    cfw_test_sink_t sink;

    cfw_test_logger_setup(&logger, &sink);

    CFW_TEST_ASSERT_EQ(cfw_log(&logger, CFW_LOG_INFO, "value=%d name=%s", 42,
                                "cfw"),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(sink.count, 1);
    CFW_TEST_ASSERT_STR_EQ(sink.messages[0], "value=42 name=cfw");
    return 0;
}

static int test_log_message_passthrough(void)
{
    cfw_logger_t logger;
    cfw_test_sink_t sink;
    cfw_strview_t msg;

    cfw_test_logger_setup(&logger, &sink);

    CFW_TEST_ASSERT_EQ(cfw_strview_init(&msg, "raw\x01bytes", 9), CFW_OK);
    /* DEBUG is below the default INFO threshold: filtered, still CFW_OK. */
    CFW_TEST_ASSERT_EQ(cfw_log_message(&logger, CFW_LOG_DEBUG, &msg), CFW_OK);
    CFW_TEST_ASSERT_EQ(sink.count, 0);

    CFW_TEST_ASSERT_EQ(cfw_log_message(&logger, CFW_LOG_WARN, &msg), CFW_OK);
    CFW_TEST_ASSERT_EQ(sink.count, 1);
    CFW_TEST_ASSERT_EQ(sink.messages[0][0], 'r');
    return 0;
}

static int test_log_invalid_arguments(void)
{
    cfw_logger_t logger;
    cfw_test_sink_t sink;
    cfw_strview_t msg;

    cfw_test_logger_setup(&logger, &sink);
    CFW_TEST_ASSERT_EQ(cfw_strview_from_cstr(&msg, "x"), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_log(NULL, CFW_LOG_INFO, "x"),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_log(&logger, (cfw_log_level_t)99, "x"),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_log(&logger, CFW_LOG_INFO, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_log_message(&logger, CFW_LOG_INFO, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_log_message(&logger, (cfw_log_level_t)-1, &msg),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_log_rate_limit(void)
{
    cfw_logger_t logger;
    cfw_test_sink_t sink;
    int i;

    cfw_test_logger_setup(&logger, &sink);
    cfw_logger_set_rate_limit(&logger, 2);

    for (i = 0; i < 5; i++) {
        CFW_TEST_ASSERT_EQ(cfw_log(&logger, CFW_LOG_INFO, "msg %d", i),
                           CFW_OK);
    }

    CFW_TEST_ASSERT_EQ(sink.count, 2);
    CFW_TEST_ASSERT_EQ(cfw_logger_dropped_count(&logger), 3);
    return 0;
}

static int test_log_long_message_is_bounded_and_marked(void)
{
    cfw_logger_t logger;
    cfw_test_sink_t sink;
    char big[3000];

    cfw_test_logger_setup(&logger, &sink);

    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    CFW_TEST_ASSERT_EQ(cfw_log(&logger, CFW_LOG_INFO, "%s", big), CFW_OK);
    CFW_TEST_ASSERT_EQ(sink.count, 1);
    CFW_TEST_ASSERT_EQ(sink.sizes[0], 1023);
    CFW_TEST_ASSERT(strcmp(sink.messages[0] + 1020, "...") == 0);
    return 0;
}

static int test_log_min_level_setter_and_message_throttle(void)
{
    cfw_logger_t logger;
    cfw_test_sink_t sink;
    cfw_strview_t msg;

    cfw_test_logger_setup(&logger, &sink);

    /* The setter validates the level; invalid values are ignored. */
    cfw_logger_set_min_level(&logger, CFW_LOG_ERROR);
    CFW_TEST_ASSERT_EQ(logger.min_level, CFW_LOG_ERROR);
    cfw_logger_set_min_level(&logger, (cfw_log_level_t)99);
    CFW_TEST_ASSERT_EQ(logger.min_level, CFW_LOG_ERROR);

    /* cfw_log_message honors the rate limiter: the second message in
     * the window is dropped and counted. */
    CFW_TEST_ASSERT_EQ(cfw_strview_from_cstr(&msg, "m"), CFW_OK);
    cfw_logger_set_rate_limit(&logger, 1);
    CFW_TEST_ASSERT_EQ(cfw_log_message(&logger, CFW_LOG_ERROR, &msg),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_log_message(&logger, CFW_LOG_ERROR, &msg),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(sink.count, 1);
    CFW_TEST_ASSERT_EQ(cfw_logger_dropped_count(&logger), 1);
    return 0;
}

const cfw_test_case cfw_test_log_cases[] = {
    {"log_init_defaults", test_log_init_defaults},
    {"log_level_filtering", test_log_level_filtering},
    {"log_formatted_message", test_log_formatted_message},
    {"log_message_passthrough", test_log_message_passthrough},
    {"log_invalid_arguments", test_log_invalid_arguments},
    {"log_rate_limit", test_log_rate_limit},
    {"log_min_level_setter_and_message_throttle",
     test_log_min_level_setter_and_message_throttle},
    {"log_long_message_is_bounded_and_marked",
     test_log_long_message_is_bounded_and_marked},
    {NULL, NULL},
};
