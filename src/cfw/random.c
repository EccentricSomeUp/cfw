/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#define _POSIX_C_SOURCE 200809L

#include <cfw/random.h>

#include <string.h>

#if defined(_WIN32)
    #include <windows.h>
    #include <bcrypt.h>
#elif defined(__linux__) || defined(__ANDROID__)
    #include <errno.h>
    #include <fcntl.h>
    #include <sys/random.h>
    #include <unistd.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
    #include <stdlib.h>
#else
    #include <errno.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif

static cfw_status_t cfw_random_read(void *buffer, size_t size)
{
    size_t offset = 0;
    (void)offset;

#if defined(_WIN32)
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
#elif defined(__linux__) || defined(__ANDROID__)
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

#if !defined(_WIN32) && !(defined(__APPLE__) || defined(__FreeBSD__) || \
                           defined(__OpenBSD__) || defined(__NetBSD__))
    {
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
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
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
    if (size != 0) {
        arc4random_buf(buffer, size);
    }
    return CFW_OK;
#else
    (void)buffer;
    (void)size;
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
