/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_RANDOM_H
#define CFW_RANDOM_H

#include <cfw/export.h>
#include <cfw/status.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cfw_random_* functions read from the operating system's cryptographically
 * secure random source. They are suitable for security tokens, nonces, and
 * cryptographic key material, not merely for statistical simulation.
 *
 * There is no seedable or deterministic mode; tests requiring determinism
 * must inject their own source rather than rely on weakening this API.
 */

CFW_API cfw_status_t cfw_random_bytes(void *buffer, size_t size);
CFW_API cfw_status_t cfw_random_u64(uint64_t *value);
CFW_API cfw_status_t cfw_random_below(uint64_t bound, uint64_t *value);

#ifdef __cplusplus
}
#endif

#endif /* CFW_RANDOM_H */
