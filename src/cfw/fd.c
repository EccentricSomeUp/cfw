/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/fd.h>

#if !defined(_WIN32)
    #include <unistd.h>
#endif

#define CFW_FD_INVALID ((intptr_t)-1)

void cfw_fd_init(cfw_fd_t *fd)
{
    if (fd == NULL) {
        return;
    }
    fd->handle = CFW_FD_INVALID;
}

int cfw_fd_valid(const cfw_fd_t *fd)
{
    if (fd == NULL) {
        return 0;
    }
    return fd->handle >= 0;
}

intptr_t cfw_fd_get(const cfw_fd_t *fd)
{
    if (!cfw_fd_valid(fd)) {
        return CFW_FD_INVALID;
    }
    return fd->handle;
}

intptr_t cfw_fd_release(cfw_fd_t *fd)
{
    intptr_t handle;

    if (fd == NULL) {
        return CFW_FD_INVALID;
    }

    handle = fd->handle;
    fd->handle = CFW_FD_INVALID;
    return handle;
}

cfw_status_t cfw_fd_close(cfw_fd_t *fd)
{
    if (fd == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (!cfw_fd_valid(fd)) {
        return CFW_OK;
    }

#if defined(_WIN32)
    /* Windows sockets require closesocket(); that backend arrives with the
     * socket layer. The wrapper is left untouched so the descriptor is not
     * lost. */
    (void)fd;
    return CFW_ERROR_UNAVAILABLE;
#else
    if (close((int)fd->handle) != 0) {
        return CFW_ERROR_IO;
    }
    fd->handle = CFW_FD_INVALID;
    return CFW_OK;
#endif
}
