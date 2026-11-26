/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_PLATFORM_H
#define CFW_PLATFORM_H

#include <cfw/export.h>
#include <cfw/status.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cfw_os {
    CFW_OS_UNKNOWN = 0,
    CFW_OS_LINUX,
    CFW_OS_MACOS,
    CFW_OS_WINDOWS,
    CFW_OS_FREEBSD,
    CFW_OS_OPENBSD,
    CFW_OS_NETBSD,
    CFW_OS_OTHER_POSIX
} cfw_os_t;

typedef enum cfw_arch {
    CFW_ARCH_UNKNOWN = 0,
    CFW_ARCH_X86,
    CFW_ARCH_X86_64,
    CFW_ARCH_AARCH64,
    CFW_ARCH_ARM,
    CFW_ARCH_RISCV64,
    CFW_ARCH_PPC64,
    CFW_ARCH_S390X,
    CFW_ARCH_OTHER
} cfw_arch_t;

typedef enum cfw_endian {
    CFW_ENDIAN_UNKNOWN = 0,
    CFW_ENDIAN_LITTLE,
    CFW_ENDIAN_BIG
} cfw_endian_t;

/*
 * Platform capability detection. cfw_platform_detect reports compile-time
 * and runtime platform facts. The *_name functions return static strings
 * and map unrecognized enum values to "unknown".
 * cfw_platform_page_size returns the OS allocation granularity and fails
 * only when the underlying OS query fails. All of these are pure,
 * side-effect-free, and safe to call from any thread.
 */
typedef struct cfw_platform_info {
    cfw_os_t os;
    cfw_arch_t arch;
    cfw_endian_t endianness;
    uint32_t pointer_bits;
} cfw_platform_info_t;

CFW_API cfw_platform_info_t cfw_platform_detect(void);
CFW_API const char *cfw_os_name(cfw_os_t os);
CFW_API const char *cfw_arch_name(cfw_arch_t arch);
CFW_API const char *cfw_endian_name(cfw_endian_t endianness);
CFW_API cfw_status_t cfw_platform_page_size(size_t *page_size);

#ifdef __cplusplus
}
#endif

#endif /* CFW_PLATFORM_H */
