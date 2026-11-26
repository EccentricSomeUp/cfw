/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include <stdio.h>
#include <string.h>

int main(void)
{
    const cfw_version_t version = cfw_get_version();
    if (version.major == 0 && version.minor == 0 && version.patch == 0) {
        fprintf(stderr, "unexpected zero version\n");
        return 1;
    }
    if (cfw_get_abi_version() == 0) {
        fprintf(stderr, "unexpected zero ABI version\n");
        return 1;
    }
    if (strlen(cfw_get_version_string()) == 0) {
        fprintf(stderr, "empty version string\n");
        return 1;
    }
    if (cfw_status_string(CFW_OK) == NULL) {
        fprintf(stderr, "null status string\n");
        return 1;
    }

    cfw_buffer_t buffer;
    if (cfw_buffer_init(&buffer) != CFW_OK) {
        return 1;
    }
    const char payload[] = "cfw-install-smoke";
    cfw_status_t status = cfw_buffer_append(&buffer, payload,
                                            sizeof(payload) - 1);
    if (status != CFW_OK) {
        cfw_buffer_free(&buffer);
        return 1;
    }
    cfw_span_t span;
    status = cfw_buffer_span(&buffer, &span);
    if (status != CFW_OK || span.size != sizeof(payload) - 1 ||
        memcmp(span.data, payload, span.size) != 0) {
        cfw_buffer_free(&buffer);
        return 1;
    }
    cfw_buffer_free(&buffer);

    puts("cfw install smoke consumer OK");
    return 0;
}
