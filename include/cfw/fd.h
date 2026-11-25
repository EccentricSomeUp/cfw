/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_FD_H
#define CFW_FD_H

#include <cfw/export.h>
#include <cfw/status.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cfw_fd_t is an owning wrapper around a platform descriptor. On POSIX
 * systems it holds a file descriptor; the intptr_t storage also accommodates
 * Windows SOCKET values when that backend is implemented.
 *
 * Ownership rules (see docs/decisions/0011):
 * - A default-initialized wrapper owns nothing and holds the invalid value.
 * - Ownership is unique: only one wrapper may own a live descriptor.
 * - cfw_fd_close closes the descriptor and invalidates the wrapper; it is
 *   safe to call on an empty wrapper.
 * - cfw_fd_release returns the raw descriptor and invalidates the wrapper,
 *   transferring ownership to the caller.
 * - Copying the struct copies the handle value but does not transfer
 *   ownership; copies must be treated as non-owning aliases and must never
 *   be closed or released.
 */
typedef struct cfw_fd {
    intptr_t handle;
} cfw_fd_t;

CFW_API void cfw_fd_init(cfw_fd_t *fd);
CFW_API int cfw_fd_valid(const cfw_fd_t *fd);
CFW_API intptr_t cfw_fd_get(const cfw_fd_t *fd);
CFW_API intptr_t cfw_fd_release(cfw_fd_t *fd);
CFW_API cfw_status_t cfw_fd_close(cfw_fd_t *fd);

#ifdef __cplusplus
}
#endif

#endif /* CFW_FD_H */
