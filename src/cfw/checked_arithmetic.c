/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/checked_arithmetic.h>

#include <stdint.h>

cfw_status_t cfw_checked_add_u32(uint32_t a, uint32_t b, uint32_t *result)
{
    uint64_t sum;

    if (result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    sum = (uint64_t)a + (uint64_t)b;
    if (sum > UINT32_MAX) {
        return CFW_ERROR_OVERFLOW;
    }

    *result = (uint32_t)sum;
    return CFW_OK;
}

cfw_status_t cfw_checked_sub_u32(uint32_t a, uint32_t b, uint32_t *result)
{
    if (result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (a < b) {
        return CFW_ERROR_OVERFLOW;
    }

    *result = a - b;
    return CFW_OK;
}

cfw_status_t cfw_checked_mul_u32(uint32_t a, uint32_t b, uint32_t *result)
{
    uint64_t product;

    if (result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    product = (uint64_t)a * (uint64_t)b;
    if (product > UINT32_MAX) {
        return CFW_ERROR_OVERFLOW;
    }

    *result = (uint32_t)product;
    return CFW_OK;
}

cfw_status_t cfw_checked_add_u64(uint64_t a, uint64_t b, uint64_t *result)
{
    if (result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (a > UINT64_MAX - b) {
        return CFW_ERROR_OVERFLOW;
    }

    *result = a + b;
    return CFW_OK;
}

cfw_status_t cfw_checked_sub_u64(uint64_t a, uint64_t b, uint64_t *result)
{
    if (result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (a < b) {
        return CFW_ERROR_OVERFLOW;
    }

    *result = a - b;
    return CFW_OK;
}

cfw_status_t cfw_checked_mul_u64(uint64_t a, uint64_t b, uint64_t *result)
{
    if (result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (a != 0 && b > UINT64_MAX / a) {
        return CFW_ERROR_OVERFLOW;
    }

    *result = a * b;
    return CFW_OK;
}

cfw_status_t cfw_checked_add_i64(int64_t a, int64_t b, int64_t *result)
{
    if (result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        return CFW_ERROR_OVERFLOW;
    }

    *result = a + b;
    return CFW_OK;
}

cfw_status_t cfw_checked_sub_i64(int64_t a, int64_t b, int64_t *result)
{
    if (result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) {
        return CFW_ERROR_OVERFLOW;
    }

    *result = a - b;
    return CFW_OK;
}

cfw_status_t cfw_checked_mul_i64(int64_t a, int64_t b, int64_t *result)
{
    if (result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (a == 0 || b == 0) {
        *result = 0;
        return CFW_OK;
    }
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b) {
                return CFW_ERROR_OVERFLOW;
            }
        } else if (b < INT64_MIN / a) {
            return CFW_ERROR_OVERFLOW;
        }
    } else {
        if (b > 0) {
            if (a < INT64_MIN / b) {
                return CFW_ERROR_OVERFLOW;
            }
        } else if (a < INT64_MAX / b) {
            return CFW_ERROR_OVERFLOW;
        }
    }

    *result = a * b;
    return CFW_OK;
}

cfw_status_t cfw_checked_add_size(size_t a, size_t b, size_t *result)
{
    if (result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (a > SIZE_MAX - b) {
        return CFW_ERROR_OVERFLOW;
    }

    *result = a + b;
    return CFW_OK;
}

cfw_status_t cfw_checked_sub_size(size_t a, size_t b, size_t *result)
{
    if (result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (a < b) {
        return CFW_ERROR_OVERFLOW;
    }

    *result = a - b;
    return CFW_OK;
}

cfw_status_t cfw_checked_mul_size(size_t a, size_t b, size_t *result)
{
    if (result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (a != 0 && b > SIZE_MAX / a) {
        return CFW_ERROR_OVERFLOW;
    }

    *result = a * b;
    return CFW_OK;
}
