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

#endif /* CFW_EXPORT_H */
