/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_PRIVATE_PLATFORM_DETECT_H
#define CFW_PRIVATE_PLATFORM_DETECT_H

/*
 * Single internal platform/feature selection point. CMake generates
 * cfw_platform_internal.h with normalized CFW_PLATFORM_* and CFW_HAVE_*
 * macros; this header adds the feature-test macros required before system
 * headers are included. Raw compiler/OS macros must not be used for platform
 * selection outside this header and platform.c's public detection routine.
 */

#include <cfw/cfw_platform_internal.h>

#if CFW_PLATFORM_WINDOWS
    /* SRWLOCK and the condition-variable APIs used by sync.c require
     * _WIN32_WINNT >= 0x0600; pin a modern floor so SDK defaults cannot
     * silently hide their declarations. */
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0A00
    #endif
#elif CFW_PLATFORM_LINUX
    #ifndef _GNU_SOURCE
        #define _GNU_SOURCE
    #endif
#elif !CFW_PLATFORM_WINDOWS
    #ifndef _POSIX_C_SOURCE
        #define _POSIX_C_SOURCE 200809L
    #endif
    #if CFW_PLATFORM_MACOS
        #ifndef _DARWIN_C_SOURCE
            #define _DARWIN_C_SOURCE
        #endif
    #endif
    #if CFW_PLATFORM_FREEBSD || CFW_PLATFORM_OPENBSD || \
        CFW_PLATFORM_NETBSD || CFW_PLATFORM_DRAGONFLY
        #ifndef _BSD_SOURCE
            #define _BSD_SOURCE
        #endif
    #endif
#endif

#if !CFW_PLATFORM_WINDOWS
    #ifndef _FILE_OFFSET_BITS
        #define _FILE_OFFSET_BITS 64
    #endif
#endif

#endif /* CFW_PRIVATE_PLATFORM_DETECT_H */
