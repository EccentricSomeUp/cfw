/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_MAX_ALIGN_H
#define CFW_MAX_ALIGN_H

/*
 * cfw_max_align_t is a maximally-aligned opaque storage type for public
 * structs that carry platform primitives in inline byte storage
 * (cfw_mutex_t, cfw_thread_t, ...). It replaces max_align_t, which is a
 * POSIX extension absent from ISO C11 and from MSVC.
 *
 * The union covers every fundamental scalar alignment on supported
 * platforms: object pointers, the widest integer, the widest floating
 * type, and function pointers. On platforms where long double is not
 * wider than double this still yields the platform's maximum scalar
 * alignment, which is all the storage-padding use case requires.
 */
typedef union cfw_max_align {
    void *object_pointer;
    long long long_long_integer;
    long double long_double_float;
    void (*function_pointer)(void);
} cfw_max_align_t;

#endif /* CFW_MAX_ALIGN_H */
