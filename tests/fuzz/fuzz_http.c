/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Coverage-guided fuzz target for the HTTP/1.x parsing and dispatch
 * surface (ADR 0077). Runs the same cfw_fuzz_http_one pipeline as the
 * deterministic mutation fuzzer: request/response head parsers,
 * framing, dispatch, chunked decoder, and the in-memory
 * cfw_http1_server event pipeline.
 *
 * The dispatch context is initialized lazily on first input and kept
 * for the process lifetime — libFuzzer calls this entry point
 * millions of times, so per-input arena setup would dominate runtime.
 */

#include "fuzz_support.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static cfw_fuzz_dispatch_t ctx;
    static int ready = 0;

    if (!ready) {
        if (cfw_fuzz_dispatch_init(&ctx) != CFW_OK) {
            return 0;
        }
        ready = 1;
    }
    cfw_fuzz_http_one(data, size, &ctx);
    return 0;
}
