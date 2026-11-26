/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/fs.h>

#include <string.h>

#if defined(_WIN32)
    #include <fcntl.h>
    #include <io.h>
    #include <sys/stat.h>
    #define CFW_STAT_STRUCT struct _stat64
    #define CFW_STAT_FUNC _stat64
    #define CFW_FILE_OPEN_FLAGS (_O_RDONLY | _O_BINARY)
    #define CFW_FILE_OPEN_FUNC _open
    #define CFW_FILE_READ_FUNC _read
    #define CFW_FILE_CLOSE_FUNC _close
#else
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>
    #define CFW_STAT_STRUCT struct stat
    #define CFW_STAT_FUNC stat
    #define CFW_FILE_OPEN_FLAGS O_RDONLY
    #define CFW_FILE_OPEN_FUNC open
    #define CFW_FILE_READ_FUNC read
    #define CFW_FILE_CLOSE_FUNC close
#endif

#define CFW_FILE_INVALID ((intptr_t)-1)

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

cfw_status_t cfw_file_read(cfw_file_t *file, void *buffer,
                           size_t capacity, size_t *out_size)
{
#if defined(_WIN32)
    int got;
#else
    ssize_t got;
#endif

    if (file == NULL || buffer == NULL || out_size == NULL ||
        capacity == 0 || !cfw_file_valid(file)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

#if defined(_WIN32)
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

void cfw_file_stream_close(cfw_file_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }
    cfw_file_close(&stream->file);
}
