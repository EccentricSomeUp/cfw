/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "private/platform_detect.h"

#if CFW_PLATFORM_WINDOWS
    /* Must precede every CRT header: <stdio.h>/<string.h> below pull in
     * the CRT declarations that honor this macro. */
    #ifndef _CRT_SECURE_NO_WARNINGS
        #define _CRT_SECURE_NO_WARNINGS
    #endif
#endif

#include <cfw/fs.h>
#include "private/fs_confined.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if CFW_PLATFORM_WINDOWS
    #include <fcntl.h>
    #include <io.h>
    #include <sys/stat.h>
    #define CFW_STAT_STRUCT struct _stat64
    #define CFW_STAT_FUNC _stat64
    #define CFW_FILE_STAT_FUNC _fstat64
    #define CFW_FILE_OPEN_FLAGS (_O_RDONLY | _O_BINARY)
    #define CFW_FILE_OPEN_FUNC _open
    #define CFW_FILE_READ_FUNC _read
    #define CFW_FILE_CLOSE_FUNC _close
    #define CFW_FILE_SEEK_FUNC _lseeki64
    /* POSIX mode-test macros are not provided by the MSVC CRT headers;
     * MinGW already defines them, so only fill in when absent. */
    #ifndef S_ISREG
        #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
    #endif
    #ifndef S_ISDIR
        #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
    #endif
#else
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>
    #define CFW_STAT_STRUCT struct stat
    #define CFW_STAT_FUNC stat
    #define CFW_FILE_STAT_FUNC fstat
    #define CFW_FILE_OPEN_FLAGS O_RDONLY
    #define CFW_FILE_OPEN_FUNC open
    #define CFW_FILE_READ_FUNC read
    #define CFW_FILE_CLOSE_FUNC close
    #define CFW_FILE_SEEK_FUNC lseek
#endif

#define CFW_FILE_INVALID ((intptr_t)-1)
#define CFW_FS_CONFINED_PATH_CAP 4096u

cfw_status_t cfw_file_size(const char *path, uint64_t *size)
{
    CFW_STAT_STRUCT st;

    if (path == NULL || size == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (CFW_STAT_FUNC(path, &st) != 0) {
        return CFW_ERROR_IO;
    }
    if (!S_ISREG(st.st_mode)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    *size = (uint64_t)st.st_size;
    return CFW_OK;
}

cfw_status_t cfw_file_is_directory(const char *path, int *is_dir)
{
    CFW_STAT_STRUCT st;

    if (path == NULL || is_dir == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (CFW_STAT_FUNC(path, &st) != 0) {
        return CFW_ERROR_IO;
    }

    *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    return CFW_OK;
}

void cfw_file_init(cfw_file_t *file)
{
    if (file == NULL) {
        return;
    }
    file->handle = CFW_FILE_INVALID;
}

int cfw_file_valid(const cfw_file_t *file)
{
    if (file == NULL) {
        return 0;
    }
    return file->handle >= 0;
}

cfw_status_t cfw_file_open(cfw_file_t *file, const char *path)
{
    intptr_t handle;

    if (file == NULL || path == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (cfw_file_valid(file)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    handle = (intptr_t)CFW_FILE_OPEN_FUNC(path, CFW_FILE_OPEN_FLAGS);
    if (handle < 0) {
        return CFW_ERROR_IO;
    }
    file->handle = handle;
    return CFW_OK;
}

/* Normalizes a relative confined path into `out`: rejects absolute
 * paths, backslashes, and ".." segments; drops empty segments from
 * repeated or trailing slashes; preserves "." segments because they can
 * still denote directory identity at the end of a path. */
static cfw_status_t cfw_file_confined_normalize(const char *path,
                                                char *out, size_t cap,
                                                size_t *out_len)
{
    const char *p = path;
    size_t o = 0;

    if (path == NULL || path[0] == '\0' || path[0] == '/' ||
        path[0] == '\\') {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    for (;;) {
        const char *end = strchr(p, '/');
        size_t seglen = end != NULL ? (size_t)(end - p) : strlen(p);
        size_t i;

        for (i = 0; i < seglen; i++) {
            if (p[i] == '\\') {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
        }
        if (seglen == 2 && p[0] == '.' && p[1] == '.') {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        if (seglen > 0) {
            size_t need = o + seglen + (o > 0 ? 1u : 0u) + 1u;
            if (need > cap) {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
            if (o > 0) {
                out[o++] = '/';
            }
            memcpy(out + o, p, seglen);
            o += seglen;
        }
        if (end == NULL) {
            break;
        }
        p = end + 1;
    }

    if (o == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    out[o] = '\0';
    *out_len = o;
    return CFW_OK;
}

#if CFW_PLATFORM_POSIX && CFW_HAVE_OPENAT && CFW_HAVE_O_NOFOLLOW
static int cfw_file_open_mode(int extra)
{
    int flags = O_RDONLY | extra;
#if CFW_HAVE_O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    return flags;
}
#endif

cfw_status_t cfw_file_open_confined(cfw_file_t *file, const char *root,
                                    const char *path)
{
    char norm[CFW_FS_CONFINED_PATH_CAP];
    size_t norm_len = 0;
    cfw_status_t status;

    if (file == NULL || root == NULL || path == NULL ||
        cfw_file_valid(file)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    status = cfw_file_confined_normalize(path, norm, sizeof(norm),
                                         &norm_len);
    if (status != CFW_OK) {
        return status;
    }
    (void)norm_len;

#if CFW_PLATFORM_WINDOWS
    {
        char full[CFW_FS_CONFINED_PATH_CAP];
        size_t rootlen = strlen(root);
        size_t o = rootlen;
        intptr_t handle;
        CFW_STAT_STRUCT st;

        if (rootlen >= sizeof(full) || norm_len >= sizeof(full) ||
            rootlen + norm_len + 2u > sizeof(full)) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        memcpy(full, root, rootlen);
        if (o > 0 && full[o - 1] != '/' && full[o - 1] != '\\') {
            full[o++] = '/';
        }
        memcpy(full + o, norm, norm_len + 1u);

        /* Match the POSIX branch's contract: a resolved path that is not
         * a regular file is INVALID_ARGUMENT. _open cannot open
         * directories at all, so without this pre-check they surface as
         * CFW_ERROR_IO instead. Confinement on Windows is documented
         * best-effort (ADR 0037), so the extra stat is acceptable. */
        {
            CFW_STAT_STRUCT pre;
            if (CFW_STAT_FUNC(full, &pre) == 0 && !S_ISREG(pre.st_mode)) {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
        }

        handle = (intptr_t)CFW_FILE_OPEN_FUNC(full, CFW_FILE_OPEN_FLAGS);
        if (handle < 0) {
            return CFW_ERROR_IO;
        }
        if (CFW_FILE_STAT_FUNC((int)handle, &st) != 0 ||
            !S_ISREG(st.st_mode)) {
            CFW_FILE_CLOSE_FUNC((int)handle);
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        file->handle = handle;
        return CFW_OK;
    }
#else
    #if CFW_HAVE_OPENAT && CFW_HAVE_O_NOFOLLOW
    {
        CFW_STAT_STRUCT st;
        int dirfd;
        char *cursor = norm;

        dirfd = open(root, cfw_file_open_mode(0));
        if (dirfd < 0) {
            return CFW_ERROR_IO;
        }
        if (fstat(dirfd, &st) != 0 || !S_ISDIR(st.st_mode)) {
            close(dirfd);
            return CFW_ERROR_INVALID_ARGUMENT;
        }

        while (cursor != NULL && *cursor != '\0') {
            char *slash = strchr(cursor, '/');
            int last = slash == NULL;
            int nextfd;

            if (slash != NULL) {
                *slash = '\0';
            }
            if (last) {
                int fd = openat(dirfd, cursor,
                                cfw_file_open_mode(O_NOFOLLOW));
                if (fd < 0) {
                    close(dirfd);
                    return CFW_ERROR_IO;
                }
                if (fstat(fd, &st) != 0) {
                    close(fd);
                    close(dirfd);
                    return CFW_ERROR_IO;
                }
                if (!S_ISREG(st.st_mode)) {
                    close(fd);
                    close(dirfd);
                    return CFW_ERROR_INVALID_ARGUMENT;
                }
                close(dirfd);
                file->handle = (intptr_t)fd;
                return CFW_OK;
            }

            nextfd = openat(dirfd, cursor,
                            cfw_file_open_mode(O_NOFOLLOW));
            if (nextfd < 0) {
                close(dirfd);
                return CFW_ERROR_IO;
            }
            if (fstat(nextfd, &st) != 0) {
                close(nextfd);
                close(dirfd);
                return CFW_ERROR_IO;
            }
            if (!S_ISDIR(st.st_mode)) {
                close(nextfd);
                close(dirfd);
                return CFW_ERROR_INVALID_ARGUMENT;
            }
            close(dirfd);
            dirfd = nextfd;
            cursor = slash + 1;
        }

        close(dirfd);
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    #else
        (void)norm;
        (void)root;
        (void)file;
        return CFW_ERROR_UNAVAILABLE;
    #endif
#endif
}

cfw_status_t cfw_file_read(cfw_file_t *file, void *buffer,
                           size_t capacity, size_t *out_size)
{
#if CFW_PLATFORM_WINDOWS
    int got;
#else
    ssize_t got;
#endif

    if (file == NULL || buffer == NULL || out_size == NULL ||
        capacity == 0 || !cfw_file_valid(file)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

#if CFW_PLATFORM_WINDOWS
    if (capacity > 0xFFFFFFFFu) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    got = CFW_FILE_READ_FUNC((int)file->handle, buffer,
                             (unsigned int)capacity);
#else
    got = CFW_FILE_READ_FUNC((int)file->handle, buffer, capacity);
#endif
    if (got < 0) {
        return CFW_ERROR_IO;
    }
    *out_size = (size_t)got;
    return CFW_OK;
}

cfw_status_t cfw_file_fstat(const cfw_file_t *file,
                            cfw_file_stat_t *stat)
{
    CFW_STAT_STRUCT st;

    if (file == NULL || stat == NULL || !cfw_file_valid(file)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (CFW_FILE_STAT_FUNC((int)file->handle, &st) != 0) {
        return CFW_ERROR_IO;
    }
    stat->size = (uint64_t)st.st_size;
    stat->mtime_seconds = (int64_t)st.st_mtime;
    return CFW_OK;
}

cfw_status_t cfw_file_seek(cfw_file_t *file, uint64_t offset)
{
    if (file == NULL || !cfw_file_valid(file) ||
        offset > (uint64_t)INT64_MAX) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (CFW_FILE_SEEK_FUNC((int)file->handle, (int64_t)offset,
                           SEEK_SET) < 0) {
        return CFW_ERROR_IO;
    }
    return CFW_OK;
}

cfw_status_t cfw_file_close(cfw_file_t *file)
{
    if (file == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (!cfw_file_valid(file)) {
        return CFW_OK;
    }
    if (CFW_FILE_CLOSE_FUNC((int)file->handle) != 0) {
        file->handle = CFW_FILE_INVALID;
        return CFW_ERROR_IO;
    }
    file->handle = CFW_FILE_INVALID;
    return CFW_OK;
}

cfw_status_t cfw_file_stream_init(cfw_file_stream_t *stream,
                                  const char *path, void *buffer,
                                  size_t buffer_size)
{
    cfw_status_t status;

    if (stream == NULL || path == NULL || buffer == NULL ||
        buffer_size == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    memset(stream, 0, sizeof(*stream));
    cfw_file_init(&stream->file);

    status = cfw_file_open(&stream->file, path);
    if (status != CFW_OK) {
        return status;
    }
    stream->buffer = (unsigned char *)buffer;
    stream->buffer_size = buffer_size;
    return CFW_OK;
}

cfw_status_t cfw_file_stream_read(void *user, cfw_span_t *chunk,
                                  int *eof)
{
    cfw_file_stream_t *stream = user;
    size_t got = 0;
    cfw_status_t status;

    if (stream == NULL || chunk == NULL || eof == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    *eof = 0;
    cfw_span_init(chunk, NULL, 0);

    if (stream->eof) {
        *eof = 1;
        return CFW_OK;
    }
    status = cfw_file_read(&stream->file, stream->buffer,
                           stream->buffer_size, &got);
    if (status != CFW_OK) {
        return status;
    }
    if (got == 0) {
        stream->eof = 1;
        *eof = 1;
        return CFW_OK;
    }
    return cfw_span_init(chunk, stream->buffer, got);
}

void cfw_file_stream_end(void *user)
{
    cfw_file_stream_close(user);
}

void cfw_file_stream_close(cfw_file_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }
    cfw_file_close(&stream->file);
}
