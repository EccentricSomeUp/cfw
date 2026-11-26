/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_STATIC_H
#define CFW_STATIC_H

#include <cfw/arena.h>
#include <cfw/dispatch.h>
#include <cfw/status.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Static file serving (ADR 0036). cfw_static_serve is a dispatch
 * middleware that intercepts GET/HEAD requests under a configured URL
 * prefix and streams the matching file from a document root. All other
 * requests pass through to the router.
 *
 * Install once per dispatch:
 *
 *     cfw_static_mount_t mount;
 *     cfw_static_options_t opts = { ... };
 *     cfw_static_mount_init(&mount, &opts);
 *     cfw_dispatch_use(&dispatch, cfw_static_serve, &mount);
 *
 * `root` and `url_prefix` must outlive the mount. `arena` must outlive
 * the mount as well; stream slots are carved from it once at init.
 */
typedef struct cfw_static_options {
    /* Document root (trusted configuration). Joined with the decoded
     * URL suffix; the suffix is validated to contain no ".." segments,
     * NUL bytes, or backslashes. On POSIX the served file is opened
     * relative to this root without following symlink components
     * (ADR 0037); Windows remains best-effort while server support is
     * deferred. */
    const char *root;
    /* URL prefix to intercept, e.g. "/static" or "/". Must start with
     * '/'; must not end with '/' unless it is exactly "/". */
    const char *url_prefix;
    /* Maximum concurrent file streams for this mount; exhaustion
     * yields 503. Slot memory is fixed: max_concurrent *
     * (sizeof(cfw_file_stream_t) + buffer_size), allocated once. */
    size_t max_concurrent;
    /* Per-slot read scratch, reused for every chunk. */
    size_t buffer_size;
    /* Arena for slot storage; must outlive the mount. */
    cfw_arena_t *arena;
} cfw_static_options_t;

typedef struct cfw_static_mount {
    const char *root;
    const char *url_prefix;
    cfw_arena_t *arena;
    unsigned char *slots;
    size_t slot_count;
    size_t slot_stride;
    size_t buffer_size;
    int ready;
} cfw_static_mount_t;

CFW_API cfw_status_t cfw_static_mount_init(
    cfw_static_mount_t *mount, const cfw_static_options_t *options);

/* Dispatch middleware (cfw_middleware_fn signature). `user` must be
 * the mount pointer. Returns CFW_OK for both pass-through (next()
 * invoked) and short-circuited file responses. */
CFW_API cfw_status_t cfw_static_serve(cfw_request_t *req,
                                      cfw_response_t *res,
                                      cfw_dispatch_next_t next,
                                      cfw_dispatch_frame_t *frame,
                                      void *user);

#ifdef __cplusplus
}
#endif

#endif /* CFW_STATIC_H */
