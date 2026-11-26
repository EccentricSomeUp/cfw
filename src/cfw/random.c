/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "private/platform_detect.h"

#include <cfw/random.h>

#include <string.h>

#if CFW_PLATFORM_WINDOWS
    #include <windows.h>
    #include <bcrypt.h>
#else
    #include <errno.h>
    #include <fcntl.h>
    #include <unistd.h>
    #if CFW_HAVE_GETRANDOM
        #include <sys/random.h>
    #endif
    #if CFW_HAVE_ARC4RANDOM_BUF
        #include <stdlib.h>
    #endif
#endif

static cfw_status_t cfw_random_read(void *buffer, size_t size)
{
#if CFW_PLATFORM_WINDOWS
    if (size == 0) {
        return CFW_OK;
    }
    if (size > (size_t)0xFFFFFFFFu) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (BCryptGenRandom(NULL, (PUCHAR)buffer, (ULONG)size,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return CFW_ERROR_UNAVAILABLE;
    }
    return CFW_OK;
#elif CFW_HAVE_ARC4RANDOM_BUF
    if (size != 0) {
        arc4random_buf(buffer, size);
    }
    return CFW_OK;
#else
    size_t offset = 0;

    #if CFW_HAVE_GETRANDOM
    while (offset < size) {
        ssize_t got = getrandom((uint8_t *)buffer + offset, size - offset, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            /* Fall back to /dev/urandom for kernels without getrandom or
             * seccomp-restricted environments. */
            break;
        }
        if (got == 0) {
            return CFW_ERROR_UNAVAILABLE;
        }
        offset += (size_t)got;
    }
    if (offset == size) {
        return CFW_OK;
    }
    #endif

    #if CFW_PLATFORM_POSIX
    {
        int fd;
#if CFW_HAVE_O_CLOEXEC
        fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
#else
        fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
#endif
        if (fd < 0) {
            return CFW_ERROR_UNAVAILABLE;
        }
        while (offset < size) {
            ssize_t got = read(fd, (uint8_t *)buffer + offset, size - offset);
            if (got < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(fd);
                return CFW_ERROR_UNAVAILABLE;
            }
            if (got == 0) {
                close(fd);
                return CFW_ERROR_UNAVAILABLE;
            }
            offset += (size_t)got;
        }
        close(fd);
        return CFW_OK;
    }
    #endif

    (void)buffer;
    (void)size;
    (void)offset;
    return CFW_ERROR_UNAVAILABLE;
#endif
}

cfw_status_t cfw_random_bytes(void *buffer, size_t size)
{
    if (buffer == NULL && size != 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    return cfw_random_read(buffer, size);
}

cfw_status_t cfw_random_u64(uint64_t *value)
{
    if (value == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    return cfw_random_bytes(value, sizeof(*value));
}

cfw_status_t cfw_random_below(uint64_t bound, uint64_t *value)
{
    uint64_t remainder;
    uint64_t cutoff;
    uint64_t candidate;
    int attempts;

    if (value == NULL || bound == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    /* Rejection sampling: accept only candidates below the largest multiple
     * of bound representable in uint64_t, then reduce modulo bound. */
    remainder = (UINT64_MAX % bound) + 1;
    if (remainder == bound) {
        remainder = 0;
    }
    cutoff = UINT64_MAX - remainder;

    for (attempts = 0; attempts < 64; attempts++) {
        cfw_status_t status = cfw_random_u64(&candidate);
        if (status != CFW_OK) {
            return status;
        }
        if (candidate <= cutoff) {
            *value = candidate % bound;
            return CFW_OK;
        }
    }

    return CFW_ERROR_UNAVAILABLE;
}
