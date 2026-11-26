/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_EXPORT_H
#define CFW_EXPORT_H

#if defined(CFW_BUILD_SHARED)
    #if defined(_WIN32)
        #if defined(CFW_BUILDING_DLL)
            #define CFW_API __declspec(dllexport)
        #else
            #define CFW_API __declspec(dllimport)
        #endif
    #elif defined(__GNUC__) || defined(__clang__)
        #define CFW_API __attribute__((visibility("default")))
    #else
        #define CFW_API
    #endif
#else
    #define CFW_API
#endif

#if defined(__GNUC__) || defined(__clang__)
    #define CFW_DEPRECATED __attribute__((deprecated))
#elif defined(_MSC_VER)
    #define CFW_DEPRECATED __declspec(deprecated)
#else
    #define CFW_DEPRECATED
#endif

/*
 * Marks a variadic function's printf-style format parameter so the
 * compiler checks call sites. format_index is the 1-based position of
 * the format parameter; first_variadic_index is the position of the
 * first variadic argument (...). MSVC has no equivalent attribute.
 */
#if defined(__GNUC__) || defined(__clang__)
    #define CFW_PRINTF_FORMAT(format_index, first_variadic_index) \
        __attribute__((format(printf, format_index, first_variadic_index)))
#else
    #define CFW_PRINTF_FORMAT(format_index, first_variadic_index)
#endif

#endif /* CFW_EXPORT_H */
