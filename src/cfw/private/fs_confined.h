/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_PRIVATE_FS_CONFINED_H
#define CFW_PRIVATE_FS_CONFINED_H

/*
 * Internal confined file-open primitive (ADR 0037). Not part of the
 * public API. Opens `path` relative to the trusted `root` directory
 * without following symlink components on POSIX, verifies the result is
 * a regular file, and returns the open descriptor in `file`.
 *
 * `path` must be relative, non-empty, contain no backslashes, and contain
 * no ".." segment. "" and "." segments are normalized away; "." path
 * components are preserved where they affect directory identity. The
 * function rejects paths whose normalized form exceeds 4096 bytes.
 */

#include <cfw/fs.h>
#include <cfw/status.h>

cfw_status_t cfw_file_open_confined(cfw_file_t *file, const char *root,
                                    const char *path);

#endif /* CFW_PRIVATE_FS_CONFINED_H */
