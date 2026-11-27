/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_FUZZ_SUPPORT_H
#define CFW_FUZZ_SUPPORT_H

/*
 * Shared fuzzing surface used by both the deterministic mutation
 * fuzzer (tests/unit/test_http_fuzz.c) and the coverage-guided
 * libFuzzer targets (tests/fuzz/fuzz_*.c, ADR 0077). Everything here
 * operates on in-memory buffers through the public API — no sockets,
 * threads, or filesystem access — so the same code path runs under
 * libFuzzer, ASan/UBSan, and the standalone corpus driver.
 */

#include <cfw/cfw.h>

#include <stddef.h>

typedef struct cfw_fuzz_dispatch {
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
} cfw_fuzz_dispatch_t;

cfw_status_t cfw_fuzz_dispatch_init(cfw_fuzz_dispatch_t *ctx);
void cfw_fuzz_dispatch_free(cfw_fuzz_dispatch_t *ctx);

/* Feeds `data` through the HTTP head parser, framing logic, dispatch
 * pipeline (when dispatch_ctx is non-NULL), the chunked decoder, and
 * the full cfw_http1_server in-memory pipeline. Any status is
 * acceptable; crashes and sanitizer reports are the failure signal. */
void cfw_fuzz_http_one(const void *data, size_t len,
                       const cfw_fuzz_dispatch_t *dispatch_ctx);

#endif /* CFW_FUZZ_SUPPORT_H */
