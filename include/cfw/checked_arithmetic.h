/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_CHECKED_ARITHMETIC_H
#define CFW_CHECKED_ARITHMETIC_H

#include <cfw/status.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

CFW_API cfw_status_t cfw_checked_add_u32(uint32_t a, uint32_t b,
                                         uint32_t *result);
CFW_API cfw_status_t cfw_checked_sub_u32(uint32_t a, uint32_t b,
                                         uint32_t *result);
CFW_API cfw_status_t cfw_checked_mul_u32(uint32_t a, uint32_t b,
                                         uint32_t *result);

CFW_API cfw_status_t cfw_checked_add_u64(uint64_t a, uint64_t b,
                                         uint64_t *result);
CFW_API cfw_status_t cfw_checked_sub_u64(uint64_t a, uint64_t b,
                                         uint64_t *result);
CFW_API cfw_status_t cfw_checked_mul_u64(uint64_t a, uint64_t b,
                                         uint64_t *result);

CFW_API cfw_status_t cfw_checked_add_i64(int64_t a, int64_t b, int64_t *result);
CFW_API cfw_status_t cfw_checked_sub_i64(int64_t a, int64_t b, int64_t *result);
CFW_API cfw_status_t cfw_checked_mul_i64(int64_t a, int64_t b, int64_t *result);

CFW_API cfw_status_t cfw_checked_add_size(size_t a, size_t b, size_t *result);
CFW_API cfw_status_t cfw_checked_sub_size(size_t a, size_t b, size_t *result);
CFW_API cfw_status_t cfw_checked_mul_size(size_t a, size_t b, size_t *result);

#ifdef __cplusplus
}
#endif

#endif /* CFW_CHECKED_ARITHMETIC_H */
