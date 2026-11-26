/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_FS_H
#define CFW_FS_H

#include <cfw/export.h>
#include <cfw/span.h>
#include <cfw/status.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Filesystem boundary primitives. These functions provide portable,
 * checked access to basic file metadata and sequential file reads.
 * They do not implement path canonicalization or traversal protection;
 * callers handling untrusted paths must validate and confine paths
 * themselves before use.
 */

CFW_API cfw_status_t cfw_file_size(const char *path, uint64_t *size);
CFW_API cfw_status_t cfw_file_is_directory(const char *path, int *is_dir);

/*
 * cfw_file_t is an owning wrapper around a portable read-only file
 * handle (a CRT file descriptor on every supported platform). Ownership
 * rules mirror cfw_fd_t:
 * - A default-initialized wrapper owns nothing and is invalid.
 * - Ownership is unique; only one wrapper may own a live handle.
 * - cfw_file_close closes the handle and invalidates the wrapper; it is
 *   safe to call on an invalid wrapper.
 * - Copies alias the handle value without transferring ownership and
 *   must never be closed.
 */
typedef struct cfw_file {
    intptr_t handle;
} cfw_file_t;

CFW_API void cfw_file_init(cfw_file_t *file);
CFW_API int cfw_file_valid(const cfw_file_t *file);
CFW_API cfw_status_t cfw_file_open(cfw_file_t *file, const char *path);
/* Reads up to `capacity` bytes into `buffer` and stores the count in
 * `out_size`. A return of CFW_OK with *out_size == 0 signals EOF.
 * `capacity` must be nonzero. */
CFW_API cfw_status_t cfw_file_read(cfw_file_t *file, void *buffer,
                                   size_t capacity, size_t *out_size);
CFW_API cfw_status_t cfw_file_close(cfw_file_t *file);

/* Metadata for an open file, obtained from the handle itself so the
 * values describe the same object the reads will see. */
typedef struct cfw_file_stat {
    uint64_t size;
    /* Modification time in seconds since the Unix epoch; may be
     * negative for timestamps before 1970. */
    int64_t mtime_seconds;
} cfw_file_stat_t;

CFW_API cfw_status_t cfw_file_fstat(const cfw_file_t *file,
                                    cfw_file_stat_t *stat);
/* Repositions an open file to `offset` bytes from the start
 * (SEEK_SET). Offsets are limited to INT64_MAX. */
CFW_API cfw_status_t cfw_file_seek(cfw_file_t *file, uint64_t offset);

/*
 * cfw_file_stream_t adapts an open file to the pull-style streaming
 * body contract used by cfw_response_t.stream (see dispatch.h): assign
 * cfw_file_stream_read to res->stream, the stream object to
 * res->stream_user, and cfw_file_stream_end to res->stream_end so the
 * file is closed exactly once whether the stream completes or the
 * connection dies mid-response.
 *
 * `buffer`/`buffer_size` are caller-owned scratch space reused for every
 * chunk; buffer_size must be nonzero. The emitted chunk remains valid
 * only until the next cfw_file_stream_read call, matching the stream
 * contract. cfw_file_stream_close may still be called for streams that
 * were initialized but never installed on a response.
 */
typedef struct cfw_file_stream {
    cfw_file_t file;
    unsigned char *buffer;
    size_t buffer_size;
    int eof;
} cfw_file_stream_t;

CFW_API cfw_status_t cfw_file_stream_init(cfw_file_stream_t *stream,
                                          const char *path,
                                          void *buffer,
                                          size_t buffer_size);
CFW_API cfw_status_t cfw_file_stream_read(void *user, cfw_span_t *chunk,
                                          int *eof);
CFW_API void cfw_file_stream_end(void *user);
CFW_API void cfw_file_stream_close(cfw_file_stream_t *stream);

#ifdef __cplusplus
}
#endif

#endif /* CFW_FS_H */
