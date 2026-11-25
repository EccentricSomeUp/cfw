/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/status.h>

const char *cfw_status_string(cfw_status_t status)
{
    switch (status) {
    case CFW_OK:
        return "ok";
    case CFW_ERROR_INVALID_ARGUMENT:
        return "invalid_argument";
    case CFW_ERROR_NOMEM:
        return "nomem";
    case CFW_ERROR_IO:
        return "io";
    case CFW_ERROR_TIMEOUT:
        return "timeout";
    case CFW_ERROR_UNAVAILABLE:
        return "unavailable";
    case CFW_ERROR_INTERNAL:
        return "internal";
    case CFW_ERROR_OVERFLOW:
        return "overflow";
    default:
        return "unknown";
    }
}
