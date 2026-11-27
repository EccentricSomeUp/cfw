/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "test_harness.h"

#include <stdio.h>

extern const cfw_test_case cfw_test_http_client_cases[];
extern const cfw_test_case cfw_test_pipeline_cases[];
extern const cfw_test_case cfw_test_ws_client_cases[];

int main(void)
{
    static const cfw_test_case *const suites[] = {
        cfw_test_http_client_cases,
        cfw_test_pipeline_cases,
        cfw_test_ws_client_cases,
    };
    int executed = 0;
    int failed = 0;
    size_t suite;

    for (suite = 0; suite < sizeof(suites) / sizeof(suites[0]); suite++) {
        const cfw_test_case *test_case = suites[suite];
        while (test_case->name != NULL) {
            executed++;
            printf("RUN  integration/%s\n", test_case->name);
            if (test_case->function() != 0) {
                failed++;
                printf("FAIL integration/%s\n", test_case->name);
            } else {
                printf("PASS integration/%s\n", test_case->name);
            }
            test_case++;
        }
    }

    printf("summary: %d executed, %d failed\n", executed, failed);
    return failed == 0 && executed > 0 ? 0 : 1;
}
