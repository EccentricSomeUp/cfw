/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/version.h>

cfw_version_t cfw_get_version(void)
{
    cfw_version_t version = {
        .major = CFW_VERSION_MAJOR,
        .minor = CFW_VERSION_MINOR,
        .patch = CFW_VERSION_PATCH,
    };

    return version;
}

const char *cfw_get_version_string(void)
{
    return CFW_VERSION_STRING;
}

uint32_t cfw_get_abi_version(void)
{
    return CFW_ABI_VERSION;
}
