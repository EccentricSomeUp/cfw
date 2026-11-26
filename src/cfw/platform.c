/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "private/platform_detect.h"

#include <cfw/platform.h>

#include <stdint.h>

#if CFW_PLATFORM_WINDOWS
    #include <windows.h>
#elif CFW_PLATFORM_POSIX
    #include <unistd.h>
#endif

static cfw_os_t cfw_detect_os(void)
{
#if CFW_PLATFORM_WINDOWS
    return CFW_OS_WINDOWS;
#elif CFW_PLATFORM_MACOS
    return CFW_OS_MACOS;
#elif CFW_PLATFORM_LINUX
    return CFW_OS_LINUX;
#elif CFW_PLATFORM_FREEBSD
    return CFW_OS_FREEBSD;
#elif CFW_PLATFORM_OPENBSD
    return CFW_OS_OPENBSD;
#elif CFW_PLATFORM_NETBSD
    return CFW_OS_NETBSD;
#elif CFW_PLATFORM_POSIX
    return CFW_OS_OTHER_POSIX;
#else
    return CFW_OS_UNKNOWN;
#endif
}

static cfw_arch_t cfw_detect_arch(void)
{
#if defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
    return CFW_ARCH_X86_64;
#elif defined(__i386__) || defined(__i486__) || defined(__i586__) || \
    defined(__i686__) || defined(_M_IX86)
    return CFW_ARCH_X86;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return CFW_ARCH_AARCH64;
#elif defined(__arm__) || defined(_M_ARM)
    return CFW_ARCH_ARM;
#elif defined(__riscv) && (__riscv_xlen == 64)
    return CFW_ARCH_RISCV64;
#elif defined(__powerpc64__) || defined(__PPC64__)
    return CFW_ARCH_PPC64;
#elif defined(__s390x__)
    return CFW_ARCH_S390X;
#elif CFW_PLATFORM_POSIX || CFW_PLATFORM_WINDOWS
    return CFW_ARCH_OTHER;
#else
    return CFW_ARCH_UNKNOWN;
#endif
}

static cfw_endian_t cfw_detect_endian(void)
{
#if defined(__BYTE_ORDER__)
    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        return CFW_ENDIAN_LITTLE;
    #elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        return CFW_ENDIAN_BIG;
    #endif
#elif CFW_PLATFORM_WINDOWS || defined(__i386__) || defined(__x86_64__) || \
    defined(__aarch64__) || defined(_M_ARM64)
    /* All currently supported Windows and common little-endian targets. */
    return CFW_ENDIAN_LITTLE;
#endif

    {
        const uint32_t value = UINT32_C(1);
        const unsigned char *bytes = (const unsigned char *)&value;
        if (bytes[0] == 1) {
            return CFW_ENDIAN_LITTLE;
        }
        if (bytes[sizeof(value) - 1] == 1) {
            return CFW_ENDIAN_BIG;
        }
    }
    return CFW_ENDIAN_UNKNOWN;
}

static uint32_t cfw_pointer_bits(void)
{
    return SIZE_MAX > UINT32_MAX ? 64u : 32u;
}

cfw_platform_info_t cfw_platform_detect(void)
{
    cfw_platform_info_t info;

    info.os = cfw_detect_os();
    info.arch = cfw_detect_arch();
    info.endianness = cfw_detect_endian();
    info.pointer_bits = cfw_pointer_bits();
    return info;
}

const char *cfw_os_name(cfw_os_t os)
{
    switch (os) {
    case CFW_OS_LINUX:
        return "linux";
    case CFW_OS_MACOS:
        return "macos";
    case CFW_OS_WINDOWS:
        return "windows";
    case CFW_OS_FREEBSD:
        return "freebsd";
    case CFW_OS_OPENBSD:
        return "openbsd";
    case CFW_OS_NETBSD:
        return "netbsd";
    case CFW_OS_OTHER_POSIX:
        return "posix";
    case CFW_OS_UNKNOWN:
    default:
        return "unknown";
    }
}

const char *cfw_arch_name(cfw_arch_t arch)
{
    switch (arch) {
    case CFW_ARCH_X86:
        return "x86";
    case CFW_ARCH_X86_64:
        return "x86_64";
    case CFW_ARCH_AARCH64:
        return "aarch64";
    case CFW_ARCH_ARM:
        return "arm";
    case CFW_ARCH_RISCV64:
        return "riscv64";
    case CFW_ARCH_PPC64:
        return "ppc64";
    case CFW_ARCH_S390X:
        return "s390x";
    case CFW_ARCH_OTHER:
        return "other";
    case CFW_ARCH_UNKNOWN:
    default:
        return "unknown";
    }
}

const char *cfw_endian_name(cfw_endian_t endianness)
{
    switch (endianness) {
    case CFW_ENDIAN_LITTLE:
        return "little";
    case CFW_ENDIAN_BIG:
        return "big";
    case CFW_ENDIAN_UNKNOWN:
    default:
        return "unknown";
    }
}

cfw_status_t cfw_platform_page_size(size_t *page_size)
{
    if (page_size == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

#if CFW_PLATFORM_WINDOWS
    {
        SYSTEM_INFO system_info;
        GetSystemInfo(&system_info);
        if (system_info.dwPageSize == 0) {
            return CFW_ERROR_UNAVAILABLE;
        }
        *page_size = (size_t)system_info.dwPageSize;
        return CFW_OK;
    }
#elif CFW_HAVE_SC_PAGESIZE || CFW_HAVE_SC_PAGE_SIZE
    {
        long result;
        #if CFW_HAVE_SC_PAGESIZE
            result = sysconf(_SC_PAGESIZE);
        #else
            result = sysconf(_SC_PAGE_SIZE);
        #endif
        if (result <= 0) {
            return CFW_ERROR_UNAVAILABLE;
        }
        *page_size = (size_t)result;
        return CFW_OK;
    }
#else
    return CFW_ERROR_UNAVAILABLE;
#endif
}
