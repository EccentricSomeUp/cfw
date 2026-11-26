/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_TEST_HARNESS_H
#define CFW_TEST_HARNESS_H

#include "private/platform_detect.h"

#include <stdio.h>
#include <string.h>

typedef int (*cfw_test_fn)(void);

typedef struct cfw_test_case {
    const char *name;
    cfw_test_fn function;
} cfw_test_case;

#define CFW_TEST_ASSERT(condition)                                                  \
    do {                                                                            \
        if (!(condition)) {                                                         \
            fprintf(stderr, "  assertion failed: %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                                    \
            return 1;                                                               \
        }                                                                           \
    } while (0)

#define CFW_TEST_ASSERT_TRUE(condition) CFW_TEST_ASSERT(condition)
#define CFW_TEST_ASSERT_FALSE(condition) CFW_TEST_ASSERT(!(condition))

#define CFW_TEST_ASSERT_EQ(actual, expected)                                        \
    do {                                                                            \
        long long cfw_test_actual_ = (long long)(actual);                           \
        long long cfw_test_expected_ = (long long)(expected);                       \
        if (cfw_test_actual_ != cfw_test_expected_) {                               \
            fprintf(stderr,                                                          \
                    "  assertion failed: %s:%d: %s == %s (%lld != %lld)\n",         \
                    __FILE__, __LINE__, #actual, #expected, cfw_test_actual_,       \
                    cfw_test_expected_);                                            \
            return 1;                                                               \
        }                                                                           \
    } while (0)

#define CFW_TEST_ASSERT_NE(actual, expected)                                        \
    do {                                                                            \
        long long cfw_test_actual_ = (long long)(actual);                           \
        long long cfw_test_expected_ = (long long)(expected);                       \
        if (cfw_test_actual_ == cfw_test_expected_) {                               \
            fprintf(stderr,                                                          \
                    "  assertion failed: %s:%d: %s != %s (both %lld)\n",            \
                    __FILE__, __LINE__, #actual, #expected, cfw_test_actual_);      \
            return 1;                                                               \
        }                                                                           \
    } while (0)

#define CFW_TEST_ASSERT_STR_EQ(actual, expected)                                    \
    do {                                                                            \
        const char *cfw_test_actual_ = (actual);                                    \
        const char *cfw_test_expected_ = (expected);                                \
        if (cfw_test_actual_ == NULL || cfw_test_expected_ == NULL ||               \
            strcmp(cfw_test_actual_, cfw_test_expected_) != 0) {                    \
            fprintf(stderr,                                                          \
                    "  assertion failed: %s:%d: %s == %s\n", __FILE__, __LINE__,   \
                    #actual, #expected);                                            \
            return 1;                                                               \
        }                                                                           \
    } while (0)

#endif /* CFW_TEST_HARNESS_H */
