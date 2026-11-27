/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Coverage-guided fuzz target for cfw_static_serve path handling
 * (ADR 0077): the fuzz input becomes the request path (and, via a
 * length prefix, optional request headers are skipped — paths are the
 * traversal surface). The mount root is the checked-in fuzz corpus
 * directory (read-only), so traversal attempts must be rejected
 * lexically before any filesystem access.
 *
 * Built with -DCFW_FUZZ_STATIC_ROOT=<dir>; without it the target is a
 * no-op.
 */

#include <cfw/cfw.h>
#include <cfw/static.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef CFW_FUZZ_STATIC_ROOT
    #define CFW_FUZZ_STATIC_ROOT ""
#endif

#define CFW_FUZZ_STATIC_MAX_INPUT 4096u

static cfw_status_t cfw_fuzz_static_next(cfw_dispatch_frame_t *frame)
{
    (void)frame;
    return CFW_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static cfw_arena_t *arena = NULL;
    static cfw_static_mount_t mount;
    static int ready = 0;
    cfw_request_t req;
    cfw_response_t res;
    cfw_http_header_t res_headers[16];
    cfw_strview_t path;
    cfw_static_options_t opts;

    if (size == 0 || size > CFW_FUZZ_STATIC_MAX_INPUT ||
        CFW_FUZZ_STATIC_ROOT[0] == '\0') {
        return 0;
    }

    if (!ready) {
        memset(&opts, 0, sizeof(opts));
        opts.root = CFW_FUZZ_STATIC_ROOT;
        opts.url_prefix = "/";
        opts.max_concurrent = 4;
        opts.buffer_size = 1024;
        arena = cfw_arena_create(64u * 1024u);
        if (arena == NULL) {
            return 0;
        }
        opts.arena = arena;
        memset(&mount, 0, sizeof(mount));
        if (cfw_static_mount_init(&mount, &opts) != CFW_OK) {
            return 0;
        }
        ready = 1;
    }

    memset(&req, 0, sizeof(req));
    req.method = CFW_HTTP_METHOD_GET;
    /* The path view borrows the fuzz input; serve() must validate it
     * without requiring NUL termination. */
    path.data = data;
    path.size = size;
    req.path = path;

    memset(&res, 0, sizeof(res));
    res.headers = res_headers;
    res.header_capacity = sizeof(res_headers) / sizeof(res_headers[0]);
    res.status = CFW_HTTP_STATUS_OK;

    (void)cfw_static_serve(&req, &res, cfw_fuzz_static_next, NULL,
                           &mount);
    return 0;
}
