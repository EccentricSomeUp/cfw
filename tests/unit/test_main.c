/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "test_harness.h"

#include <stdio.h>
#include <string.h>

extern const cfw_test_case cfw_test_arena_cases[];
extern const cfw_test_case cfw_test_buffer_cases[];
extern const cfw_test_case cfw_test_checked_arithmetic_cases[];
extern const cfw_test_case cfw_test_log_cases[];
extern const cfw_test_case cfw_test_platform_cases[];
extern const cfw_test_case cfw_test_span_cases[];
extern const cfw_test_case cfw_test_status_cases[];
extern const cfw_test_case cfw_test_strview_cases[];
extern const cfw_test_case cfw_test_time_cases[];
extern const cfw_test_case cfw_test_version_cases[];

typedef struct cfw_test_suite {
    const char *name;
    const cfw_test_case *cases;
} cfw_test_suite;

static const cfw_test_suite cfw_test_suites[] = {
    {"arena", cfw_test_arena_cases},
    {"buffer", cfw_test_buffer_cases},
    {"checked_arithmetic", cfw_test_checked_arithmetic_cases},
    {"log", cfw_test_log_cases},
    {"platform", cfw_test_platform_cases},
    {"span", cfw_test_span_cases},
    {"status", cfw_test_status_cases},
    {"strview", cfw_test_strview_cases},
    {"time", cfw_test_time_cases},
    {"version", cfw_test_version_cases},
};

static int cfw_test_matches(const char *suite_name, const char *case_name,
                            const char *filter)
{
    if (filter == NULL) {
        return 1;
    }

    return strstr(suite_name, filter) != NULL ||
           strstr(case_name, filter) != NULL;
}

static void cfw_test_list(void)
{
    size_t suite_index;

    for (suite_index = 0; suite_index < sizeof(cfw_test_suites) /
                                        sizeof(cfw_test_suites[0]);
         suite_index++) {
        const cfw_test_suite *suite = &cfw_test_suites[suite_index];
        const cfw_test_case *test_case = suite->cases;

        while (test_case->name != NULL) {
            printf("%s/%s\n", suite->name, test_case->name);
            test_case++;
        }
    }
}

int main(int argc, char **argv)
{
    const char *filter = NULL;
    int list_only = 0;
    int executed = 0;
    int failed = 0;
    int argument_index;

    for (argument_index = 1; argument_index < argc; argument_index++) {
        if (strcmp(argv[argument_index], "--list") == 0) {
            list_only = 1;
        } else if (strcmp(argv[argument_index], "--filter") == 0 &&
                   argument_index + 1 < argc) {
            filter = argv[++argument_index];
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[argument_index]);
            fprintf(stderr, "usage: %s [--list] [--filter TEXT]\n", argv[0]);
            return 2;
        }
    }

    if (list_only) {
        cfw_test_list();
        return 0;
    }

    for (size_t suite_index = 0; suite_index < sizeof(cfw_test_suites) /
                                              sizeof(cfw_test_suites[0]);
         suite_index++) {
        const cfw_test_suite *suite = &cfw_test_suites[suite_index];
        const cfw_test_case *test_case = suite->cases;

        while (test_case->name != NULL) {
            if (cfw_test_matches(suite->name, test_case->name, filter)) {
                executed++;
                printf("RUN  %s/%s\n", suite->name, test_case->name);
                if (test_case->function() != 0) {
                    failed++;
                    printf("FAIL %s/%s\n", suite->name, test_case->name);
                } else {
                    printf("PASS %s/%s\n", suite->name, test_case->name);
                }
            }
            test_case++;
        }
    }

    printf("summary: %d executed, %d failed\n", executed, failed);
    return failed == 0 && executed > 0 ? 0 : 1;
}
