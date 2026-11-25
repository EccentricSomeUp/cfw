/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_VERSION_H
#define CFW_VERSION_H

#include <cfw/cfw_config.h>
#include <cfw/export.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cfw_version {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} cfw_version_t;

CFW_API cfw_version_t cfw_get_version(void);
CFW_API const char *cfw_get_version_string(void);
CFW_API uint32_t cfw_get_abi_version(void);

#ifdef __cplusplus
}
#endif

#endif /* CFW_VERSION_H */
