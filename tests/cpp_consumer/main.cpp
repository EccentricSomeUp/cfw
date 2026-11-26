// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// C++ consumer smoke test: verifies that every public header compiles as
// C++ and that exported symbols link correctly.

#include <cfw/cfw.h>

#include <cstdio>
#include <cstring>

int main(void)
{
    const cfw_version_t version = cfw_get_version();
    if (version.major == 0 && version.minor == 0 && version.patch == 0) {
        std::fprintf(stderr, "unexpected zero version\n");
        return 1;
    }
    if (cfw_get_abi_version() == 0) {
        std::fprintf(stderr, "unexpected zero ABI version\n");
        return 1;
    }
    if (std::strlen(cfw_get_version_string()) == 0) {
        std::fprintf(stderr, "empty version string\n");
        return 1;
    }
    if (cfw_status_string(CFW_OK) == nullptr) {
        std::fprintf(stderr, "null status string\n");
        return 1;
    }

    cfw_buffer_t buffer;
    if (cfw_buffer_init(&buffer) != CFW_OK) {
        return 1;
    }
    const char payload[] = "cfw-cpp-consumer";
    cfw_status_t status = cfw_buffer_append(&buffer, payload,
                                            sizeof(payload) - 1);
    if (status != CFW_OK) {
        cfw_buffer_free(&buffer);
        return 1;
    }
    cfw_span_t span;
    status = cfw_buffer_span(&buffer, &span);
    if (status != CFW_OK ||
        span.size != sizeof(payload) - 1 ||
        std::memcmp(span.data, payload, span.size) != 0) {
        cfw_buffer_free(&buffer);
        return 1;
    }
    cfw_buffer_free(&buffer);

    uint64_t random_value = 0;
    if (cfw_random_u64(&random_value) != CFW_OK) {
        return 1;
    }

    std::puts("cfw C++ consumer OK");
    return 0;
}
