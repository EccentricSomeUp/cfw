/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_VERSION_H
#define CFW_VERSION_H

#include <cfw/cfw_config.h>
#include <cfw/export.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Version getters return compile-time constants describing the library
 * build: cfw_get_version reports the project version,
 * cfw_get_version_string its dotted string form, and
 * cfw_get_abi_version the stable ABI version (which moves independently
 * of the project version). All three are pure, side-effect-free, and safe
 * to call from any thread, including before any other initialization.
 */
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
