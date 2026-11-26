/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_FS_H
#define CFW_FS_H

#include <cfw/export.h>
#include <cfw/status.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Filesystem boundary primitives. These functions provide portable,
 * checked access to basic file metadata. They do not implement path
 * canonicalization or traversal protection; callers handling untrusted
 * paths must validate and confine paths themselves before use.
 */

CFW_API cfw_status_t cfw_file_size(const char *path, uint64_t *size);
CFW_API cfw_status_t cfw_file_is_directory(const char *path, int *is_dir);

#ifdef __cplusplus
}
#endif

#endif /* CFW_FS_H */
