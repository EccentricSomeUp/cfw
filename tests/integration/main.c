/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "test_harness.h"

#include <stdio.h>

extern const cfw_test_case cfw_test_integration_cases[];

int main(void)
{
    const cfw_test_case *test_case = cfw_test_integration_cases;
    int executed = 0;
    int failed = 0;

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

    printf("summary: %d executed, %d failed\n", executed, failed);
    return failed == 0 && executed > 0 ? 0 : 1;
}
