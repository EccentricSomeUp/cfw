/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#if !defined(_WIN32)

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CFW_TEST_STATIC_ROOT "cfw_test_static_root"
#define CFW_TEST_STATIC_OUTSIDE_FILE "cfw_test_static_outside.txt"
#define CFW_TEST_STATIC_OUTSIDE_DIR "cfw_test_static_outside_dir"

static int cfw_test_next_calls;

static cfw_status_t cfw_test_next(cfw_dispatch_frame_t *frame)
{
    (void)frame;
    cfw_test_next_calls++;
    return CFW_OK;
}

static int cfw_test_write_file(const char *path, const char *data)
{
    FILE *f = fopen(path, "wb");
    CFW_TEST_ASSERT(f != NULL);
    CFW_TEST_ASSERT_EQ(fwrite(data, 1, strlen(data), f), strlen(data));
    CFW_TEST_ASSERT_EQ(fclose(f), 0);
    return 0;
}

static int cfw_test_static_setup(cfw_static_mount_t *mount,
                                 cfw_arena_t **arena_out,
                                 size_t max_concurrent)
{
    cfw_arena_t *arena;
    cfw_static_options_t opts;
    char path[512];

    /* Document root with a small tree. mkdir tolerates EEXIST so a
     * previous failed run does not poison the next one. */
    CFW_TEST_ASSERT_TRUE(mkdir(CFW_TEST_STATIC_ROOT, 0755) == 0 ||
                         errno == EEXIST);
    snprintf(path, sizeof(path), "%s/index.html",
             CFW_TEST_STATIC_ROOT);
    CFW_TEST_ASSERT_EQ(cfw_test_write_file(path, "<h1>index</h1>"),
                       0);
    snprintf(path, sizeof(path), "%s/hello.txt", CFW_TEST_STATIC_ROOT);
    CFW_TEST_ASSERT_EQ(cfw_test_write_file(path,
                                           "hello static world"),
                       0);
    snprintf(path, sizeof(path), "%s/noext", CFW_TEST_STATIC_ROOT);
    CFW_TEST_ASSERT_EQ(cfw_test_write_file(path, "no extension"), 0);
    snprintf(path, sizeof(path), "%s/empty.txt", CFW_TEST_STATIC_ROOT);
    CFW_TEST_ASSERT_EQ(cfw_test_write_file(path, ""), 0);
    snprintf(path, sizeof(path), "%s/UPPER.HTML",
             CFW_TEST_STATIC_ROOT);
    CFW_TEST_ASSERT_EQ(cfw_test_write_file(path, "upper"), 0);
    snprintf(path, sizeof(path), "%s/sub", CFW_TEST_STATIC_ROOT);
    CFW_TEST_ASSERT_TRUE(mkdir(path, 0755) == 0 || errno == EEXIST);
    snprintf(path, sizeof(path), "%s/sub/site.css",
             CFW_TEST_STATIC_ROOT);
    CFW_TEST_ASSERT_EQ(cfw_test_write_file(path,
                                           "body { color: red; }"),
                       0);
    snprintf(path, sizeof(path), "%s/emptydir",
             CFW_TEST_STATIC_ROOT);
    CFW_TEST_ASSERT_TRUE(mkdir(path, 0755) == 0 || errno == EEXIST);

    /* Files outside the root, reachable only through symlinks. */
    CFW_TEST_ASSERT_EQ(cfw_test_write_file(CFW_TEST_STATIC_OUTSIDE_FILE,
                                           "outside secret"),
                       0);
    CFW_TEST_ASSERT_TRUE(mkdir(CFW_TEST_STATIC_OUTSIDE_DIR, 0755) == 0 ||
                         errno == EEXIST);
    snprintf(path, sizeof(path), "%s/secret.txt",
             CFW_TEST_STATIC_OUTSIDE_DIR);
    CFW_TEST_ASSERT_EQ(cfw_test_write_file(path, "outside dir secret"),
                       0);
    CFW_TEST_ASSERT_TRUE(symlink("../" CFW_TEST_STATIC_OUTSIDE_FILE,
                                 "cfw_test_static_root/link.txt") == 0 ||
                         errno == EEXIST);
    CFW_TEST_ASSERT_TRUE(symlink("../" CFW_TEST_STATIC_OUTSIDE_DIR,
                                 "cfw_test_static_root/linkdir") == 0 ||
                         errno == EEXIST);

    arena = cfw_arena_create(4096);
    CFW_TEST_ASSERT(arena != NULL);
    memset(&opts, 0, sizeof(opts));
    opts.root = CFW_TEST_STATIC_ROOT;
    opts.url_prefix = "/static";
    opts.max_concurrent = max_concurrent;
    opts.buffer_size = 16;
    opts.arena = arena;
    CFW_TEST_ASSERT_EQ(cfw_static_mount_init(mount, &opts), CFW_OK);
    *arena_out = arena;
    return 0;
}

static int cfw_test_static_teardown(cfw_static_mount_t *mount,
                                    cfw_arena_t *arena)
{
    char cmd[600];
    (void)mount;
    cfw_arena_destroy(arena);
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s %s", CFW_TEST_STATIC_ROOT,
             CFW_TEST_STATIC_OUTSIDE_FILE, CFW_TEST_STATIC_OUTSIDE_DIR);
    CFW_TEST_ASSERT_EQ(system(cmd), 0);
    return 0;
}

/* Runs cfw_static_serve for `path` with the given method and optional
 * request headers. Returns 0 on success; the response status and
 * whether next() ran are reported via out-params. */
static int cfw_test_serve_ex(cfw_static_mount_t *mount,
                             cfw_http_method_t method, const char *path,
                             const cfw_http_header_t *req_headers,
                             size_t req_hcount, cfw_response_t *res,
                             cfw_http_header_t *hdrs, size_t hdrcap,
                             cfw_http_status_t *status_out, int *nexted)
{
    cfw_request_t req;
    cfw_strview_t path_sv;

    memset(&req, 0, sizeof(req));
    req.method = method;
    cfw_strview_from_cstr(&path_sv, path);
    req.path = path_sv;
    req.headers = req_headers;
    req.header_count = req_hcount;

    memset(res, 0, sizeof(*res));
    res->headers = hdrs;
    res->header_capacity = hdrcap;
    res->status = CFW_HTTP_STATUS_OK; /* dispatch resets to 200 */

    cfw_test_next_calls = 0;
    /* frame is opaque and unused by the stub next(). */
    CFW_TEST_ASSERT_EQ(cfw_static_serve(&req, res, cfw_test_next,
                                        NULL, mount),
                       CFW_OK);
    *status_out = res->status;
    *nexted = cfw_test_next_calls;
    return 0;
}

static int cfw_test_serve(cfw_static_mount_t *mount,
                          cfw_http_method_t method, const char *path,
                          cfw_response_t *res,
                          cfw_http_header_t *hdrs, size_t hdrcap,
                          cfw_http_status_t *status_out, int *nexted)
{
    return cfw_test_serve_ex(mount, method, path, NULL, 0, res, hdrs,
                             hdrcap, status_out, nexted);
}

/* Builds a one-entry request header array from name/value literals. */
static void cfw_test_req_header(cfw_http_header_t *hdr,
                                const char *name, const char *value)
{
    cfw_strview_from_cstr(&hdr->name, name);
    cfw_strview_from_cstr(&hdr->value, value);
}

/* Pulls the whole stream body (if any) into out; returns 0 and sets
 * *out_len. Mimics the server: calls stream_end after eof. */
static int cfw_test_pull_body(cfw_response_t *res, char *out,
                              size_t cap, size_t *out_len)
{
    size_t len = 0;
    *out_len = 0;
    if (res->stream == NULL) {
        return 0;
    }
    for (;;) {
        cfw_span_t chunk;
        int eof = 0;
        CFW_TEST_ASSERT_EQ(res->stream(res->stream_user, &chunk, &eof),
                           CFW_OK);
        if (eof) {
            break;
        }
        CFW_TEST_ASSERT_TRUE(len + chunk.size <= cap);
        memcpy(out + len, chunk.data, chunk.size);
        len += chunk.size;
    }
    if (res->stream_end != NULL) {
        res->stream_end(res->stream_user);
    }
    *out_len = len;
    return 0;
}

static const char *cfw_test_header(cfw_response_t *res,
                                   const char *name)
{
    cfw_strview_t want;
    size_t i;
    cfw_strview_from_cstr(&want, name);
    for (i = 0; i < res->header_count; i++) {
        int eq = 0;
        if (cfw_strview_equal(&res->headers[i].name, &want,
                              &eq) == CFW_OK &&
            eq) {
            /* Values are string literals here, so NUL-terminated. */
            return res->headers[i].value.data;
        }
    }
    return NULL;
}

static int test_static_mount_init_validation(void)
{
    cfw_static_mount_t mount;
    cfw_static_options_t opts;
    cfw_arena_t *arena = cfw_arena_create(4096);

    memset(&opts, 0, sizeof(opts));
    CFW_TEST_ASSERT_EQ(cfw_static_mount_init(NULL, &opts),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_static_mount_init(&mount, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);

    opts.root = "x";
    opts.url_prefix = "/s";
    opts.max_concurrent = 1;
    opts.buffer_size = 16;
    /* arena NULL */
    CFW_TEST_ASSERT_EQ(cfw_static_mount_init(&mount, &opts),
                       CFW_ERROR_INVALID_ARGUMENT);

    opts.arena = arena;
    opts.url_prefix = "noslash";
    CFW_TEST_ASSERT_EQ(cfw_static_mount_init(&mount, &opts),
                       CFW_ERROR_INVALID_ARGUMENT);
    opts.url_prefix = "/s/";
    CFW_TEST_ASSERT_EQ(cfw_static_mount_init(&mount, &opts),
                       CFW_ERROR_INVALID_ARGUMENT);
    opts.url_prefix = "/";
    opts.max_concurrent = 0;
    CFW_TEST_ASSERT_EQ(cfw_static_mount_init(&mount, &opts),
                       CFW_ERROR_INVALID_ARGUMENT);
    opts.max_concurrent = 1;
    opts.buffer_size = 0;
    CFW_TEST_ASSERT_EQ(cfw_static_mount_init(&mount, &opts),
                       CFW_ERROR_INVALID_ARGUMENT);

    cfw_arena_destroy(arena);
    return 0;
}

static int test_static_get_file(void)
{
    cfw_static_mount_t mount;
    cfw_arena_t *arena;
    cfw_http_header_t hdrs[8];
    cfw_response_t res;
    cfw_http_status_t status;
    int nexted;
    char body[256];
    size_t len = 0;
    const char *ct;

    CFW_TEST_ASSERT_EQ(cfw_test_static_setup(&mount, &arena, 4), 0);

    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/hello.txt", &res, hdrs,
                                      8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_OK);
    CFW_TEST_ASSERT_EQ(nexted, 0);
    ct = cfw_test_header(&res, "Content-Type");
    CFW_TEST_ASSERT_TRUE(ct != NULL);
    CFW_TEST_ASSERT_TRUE(strstr(ct, "text/plain") != NULL);
    CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res, body, sizeof(body),
                                          &len),
                       0);
    CFW_TEST_ASSERT_EQ(len, strlen("hello static world"));
    CFW_TEST_ASSERT_EQ(memcmp(body, "hello static world", len), 0);

    CFW_TEST_ASSERT_EQ(cfw_test_static_teardown(&mount, arena), 0);
    return 0;
}

static int test_static_index_and_subdir(void)
{
    cfw_static_mount_t mount;
    cfw_arena_t *arena;
    cfw_http_header_t hdrs[8];
    cfw_response_t res;
    cfw_http_status_t status;
    int nexted;
    char body[256];
    size_t len = 0;
    const char *ct;

    CFW_TEST_ASSERT_EQ(cfw_test_static_setup(&mount, &arena, 4), 0);

    /* Directory request resolves to index.html. */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/", &res, hdrs, 8,
                                      &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_OK);
    ct = cfw_test_header(&res, "Content-Type");
    CFW_TEST_ASSERT_TRUE(ct != NULL && strstr(ct, "text/html") != NULL);
    CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res, body, sizeof(body),
                                          &len),
                       0);
    CFW_TEST_ASSERT_EQ(memcmp(body, "<h1>index</h1>", len), 0);

    /* Nested path + CSS mime. */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/sub/site.css", &res,
                                      hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_OK);
    ct = cfw_test_header(&res, "Content-Type");
    CFW_TEST_ASSERT_TRUE(ct != NULL && strstr(ct, "text/css") != NULL);
    CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res, body, sizeof(body),
                                          &len),
                       0);
    CFW_TEST_ASSERT_TRUE(strstr(body, "color: red") != NULL);

    CFW_TEST_ASSERT_EQ(cfw_test_static_teardown(&mount, arena), 0);
    return 0;
}

static int test_static_head_and_mime(void)
{
    cfw_static_mount_t mount;
    cfw_arena_t *arena;
    cfw_http_header_t hdrs[8];
    cfw_response_t res;
    cfw_http_status_t status;
    int nexted;
    const char *ct;

    CFW_TEST_ASSERT_EQ(cfw_test_static_setup(&mount, &arena, 4), 0);

    /* HEAD: headers only, no stream. */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_HEAD,
                                      "/static/hello.txt", &res, hdrs,
                                      8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_OK);
    CFW_TEST_ASSERT_EQ(nexted, 0);
    CFW_TEST_ASSERT_TRUE(res.stream == NULL);
    ct = cfw_test_header(&res, "Content-Type");
    CFW_TEST_ASSERT_TRUE(ct != NULL);

    /* Extensionless file → octet-stream. */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/noext", &res, hdrs, 8,
                                      &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_OK);
    ct = cfw_test_header(&res, "Content-Type");
    CFW_TEST_ASSERT_TRUE(ct != NULL &&
                         strstr(ct, "application/octet-stream") !=
                             NULL);
    if (res.stream_end != NULL) {
        res.stream_end(res.stream_user);
    }

    /* Uppercase extension matches case-insensitively. */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/UPPER.HTML", &res,
                                      hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_OK);
    ct = cfw_test_header(&res, "Content-Type");
    CFW_TEST_ASSERT_TRUE(ct != NULL && strstr(ct, "text/html") != NULL);
    if (res.stream_end != NULL) {
        res.stream_end(res.stream_user);
    }

    CFW_TEST_ASSERT_EQ(cfw_test_static_teardown(&mount, arena), 0);
    return 0;
}

static int test_static_errors_and_passthrough(void)
{
    cfw_static_mount_t mount;
    cfw_arena_t *arena;
    cfw_http_header_t hdrs[8];
    cfw_response_t res;
    cfw_http_status_t status;
    int nexted;

    CFW_TEST_ASSERT_EQ(cfw_test_static_setup(&mount, &arena, 4), 0);

    /* Missing file → 404. */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/missing.txt", &res,
                                      hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_NOT_FOUND);
    CFW_TEST_ASSERT_EQ(nexted, 0);

    /* Directory without index.html → 403. */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/emptydir/", &res,
                                      hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_FORBIDDEN);

    /* Traversal attempts → 404 (or 400 for malformed escapes). */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/../secret", &res,
                                      hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_NOT_FOUND);
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/%2e%2e/secret", &res,
                                      hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_NOT_FOUND);
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/a%2f..%2fsecret", &res,
                                      hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_NOT_FOUND);
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/%zz", &res, hdrs, 8,
                                      &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_BAD_REQUEST);
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/a%00b", &res, hdrs, 8,
                                      &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_BAD_REQUEST);
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/a%5Cb", &res, hdrs, 8,
                                      &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_BAD_REQUEST);

    /* Outside the prefix → next(). */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/other/x", &res, hdrs, 8,
                                      &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(nexted, 1);
    /* Prefix boundary: /staticx must not match /static. */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/staticx/f", &res, hdrs, 8,
                                      &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(nexted, 1);
    /* Wrong method → next(). */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_POST,
                                      "/static/hello.txt", &res,
                                      hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(nexted, 1);

    CFW_TEST_ASSERT_EQ(cfw_test_static_teardown(&mount, arena), 0);
    return 0;
}

static int test_static_symlink_confinement(void)
{
    cfw_static_mount_t mount;
    cfw_arena_t *arena;
    cfw_http_header_t hdrs[8];
    cfw_response_t res;
    cfw_http_status_t status;
    int nexted;
    char body[128];
    size_t len = 0;

    CFW_TEST_ASSERT_EQ(cfw_test_static_setup(&mount, &arena, 4), 0);

    /* A symlinked file inside root must not expose the outside target. */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/link.txt", &res, hdrs,
                                      8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_NOT_FOUND);
    CFW_TEST_ASSERT_EQ(nexted, 0);
    CFW_TEST_ASSERT_TRUE(res.stream == NULL);

    /* HEAD applies the same confinement decision as GET. */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_HEAD,
                                      "/static/link.txt", &res, hdrs,
                                      8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_NOT_FOUND);

    /* A symlinked directory component is rejected during the walk. */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/linkdir/secret.txt",
                                      &res, hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_NOT_FOUND);
    CFW_TEST_ASSERT_EQ(nexted, 0);

    /* A failed confined open must not leak the stream slot. */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/hello.txt", &res, hdrs,
                                      8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res, body, sizeof(body),
                                          &len),
                       0);
    CFW_TEST_ASSERT_EQ(len, strlen("hello static world"));

    CFW_TEST_ASSERT_EQ(cfw_test_static_teardown(&mount, arena), 0);
    return 0;
}

static int test_static_slot_exhaustion(void)
{
    cfw_static_mount_t mount;
    cfw_arena_t *arena;
    cfw_http_header_t hdrs[8];
    cfw_response_t res1, res2;
    cfw_http_status_t status;
    int nexted;
    char body[256];
    size_t len = 0;

    CFW_TEST_ASSERT_EQ(cfw_test_static_setup(&mount, &arena, 1), 0);

    /* First stream holds the only slot. */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/hello.txt", &res1,
                                      hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_OK);
    CFW_TEST_ASSERT_TRUE(res1.stream != NULL);

    /* Second concurrent stream → 503. */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/index.html", &res2,
                                      hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_SERVICE_UNAVAILABLE);

    /* Ending the first frees the slot. */
    CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res1, body, sizeof(body),
                                          &len),
                       0);
    CFW_TEST_ASSERT_EQ(len, strlen("hello static world"));
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/index.html", &res2,
                                      hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_OK);
    CFW_TEST_ASSERT_TRUE(res2.stream != NULL);
    CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res2, body, sizeof(body),
                                          &len),
                       0);

    CFW_TEST_ASSERT_EQ(cfw_test_static_teardown(&mount, arena), 0);
    return 0;
}

static int test_static_conditional_requests(void)
{
    cfw_static_mount_t mount;
    cfw_arena_t *arena;
    cfw_http_header_t hdrs[8];
    cfw_http_header_t req_hdrs[2];
    cfw_response_t res;
    cfw_http_status_t status;
    int nexted;
    const char *lm;
    const char *ar;
    char body[64];
    size_t len = 0;

    CFW_TEST_ASSERT_EQ(cfw_test_static_setup(&mount, &arena, 4), 0);

    /* A plain GET exposes the validators conditional requests need. */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/hello.txt", &res, hdrs,
                                      8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_OK);
    lm = cfw_test_header(&res, "Last-Modified");
    CFW_TEST_ASSERT_TRUE(lm != NULL);
    ar = cfw_test_header(&res, "Accept-Ranges");
    CFW_TEST_ASSERT_TRUE(ar != NULL);
    CFW_TEST_ASSERT_EQ(strcmp(ar, "bytes"), 0);
    CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res, body, sizeof(body),
                                          &len),
                       0);

    /* If-Modified-Since at the emitted Last-Modified yields 304 for
     * both GET and HEAD, with no stream and no slot held. */
    cfw_test_req_header(&req_hdrs[0], "If-Modified-Since", lm);
    CFW_TEST_ASSERT_EQ(cfw_test_serve_ex(
                           &mount, CFW_HTTP_METHOD_GET,
                           "/static/hello.txt", req_hdrs, 1, &res,
                           hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_NOT_MODIFIED);
    CFW_TEST_ASSERT_TRUE(res.stream == NULL);
    CFW_TEST_ASSERT_EQ(nexted, 0);
    CFW_TEST_ASSERT_TRUE(cfw_test_header(&res, "Last-Modified") != NULL);

    CFW_TEST_ASSERT_EQ(cfw_test_serve_ex(
                           &mount, CFW_HTTP_METHOD_HEAD,
                           "/static/hello.txt", req_hdrs, 1, &res,
                           hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_NOT_MODIFIED);
    CFW_TEST_ASSERT_TRUE(res.stream == NULL);

    /* A stale If-Modified-Since still serves the file. */
    cfw_test_req_header(&req_hdrs[0], "If-Modified-Since",
                        "Sun, 06 Nov 1994 08:49:37 GMT");
    CFW_TEST_ASSERT_EQ(cfw_test_serve_ex(
                           &mount, CFW_HTTP_METHOD_GET,
                           "/static/hello.txt", req_hdrs, 1, &res,
                           hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res, body, sizeof(body),
                                          &len),
                       0);

    /* A malformed date is ignored, not fatal. */
    cfw_test_req_header(&req_hdrs[0], "If-Modified-Since",
                        "not a date");
    CFW_TEST_ASSERT_EQ(cfw_test_serve_ex(
                           &mount, CFW_HTTP_METHOD_GET,
                           "/static/hello.txt", req_hdrs, 1, &res,
                           hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res, body, sizeof(body),
                                          &len),
                       0);

    /* If-Unmodified-Since in the past rejects the changed file. */
    cfw_test_req_header(&req_hdrs[0], "If-Unmodified-Since",
                        "Sun, 06 Nov 1994 08:49:37 GMT");
    CFW_TEST_ASSERT_EQ(cfw_test_serve_ex(
                           &mount, CFW_HTTP_METHOD_GET,
                           "/static/hello.txt", req_hdrs, 1, &res,
                           hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_PRECONDITION_FAILED);
    CFW_TEST_ASSERT_TRUE(res.stream == NULL);

    /* At the emitted Last-Modified the file counts as unmodified. */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/hello.txt", &res, hdrs,
                                      8, &status, &nexted),
                       0);
    lm = cfw_test_header(&res, "Last-Modified");
    CFW_TEST_ASSERT_TRUE(lm != NULL);
    CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res, body, sizeof(body),
                                          &len),
                       0);
    cfw_test_req_header(&req_hdrs[0], "If-Unmodified-Since", lm);
    CFW_TEST_ASSERT_EQ(cfw_test_serve_ex(
                           &mount, CFW_HTTP_METHOD_GET,
                           "/static/hello.txt", req_hdrs, 1, &res,
                           hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res, body, sizeof(body),
                                          &len),
                       0);

    CFW_TEST_ASSERT_EQ(cfw_test_static_teardown(&mount, arena), 0);
    return 0;
}

static int test_static_range_requests(void)
{
    cfw_static_mount_t mount;
    cfw_arena_t *arena;
    cfw_http_header_t hdrs[8];
    cfw_http_header_t req_hdrs[2];
    cfw_response_t res;
    cfw_http_status_t status;
    int nexted;
    const char *cr;
    const char *lm;
    char body[64];
    size_t len = 0;

    CFW_TEST_ASSERT_EQ(cfw_test_static_setup(&mount, &arena, 4), 0);

    /* Explicit bounds: "bytes=0-4" of "hello static world" (18 B). */
    cfw_test_req_header(&req_hdrs[0], "Range", "bytes=0-4");
    CFW_TEST_ASSERT_EQ(cfw_test_serve_ex(
                           &mount, CFW_HTTP_METHOD_GET,
                           "/static/hello.txt", req_hdrs, 1, &res,
                           hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_PARTIAL_CONTENT);
    cr = cfw_test_header(&res, "Content-Range");
    CFW_TEST_ASSERT_TRUE(cr != NULL);
    CFW_TEST_ASSERT_EQ(strcmp(cr, "bytes 0-4/18"), 0);
    CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res, body, sizeof(body),
                                          &len),
                       0);
    CFW_TEST_ASSERT_EQ(len, 5);
    CFW_TEST_ASSERT_EQ(memcmp(body, "hello", 5), 0);

    /* Open-ended and suffix forms. */
    cfw_test_req_header(&req_hdrs[0], "Range", "bytes=6-");
    CFW_TEST_ASSERT_EQ(cfw_test_serve_ex(
                           &mount, CFW_HTTP_METHOD_GET,
                           "/static/hello.txt", req_hdrs, 1, &res,
                           hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_PARTIAL_CONTENT);
    cr = cfw_test_header(&res, "Content-Range");
    CFW_TEST_ASSERT_EQ(strcmp(cr, "bytes 6-17/18"), 0);
    CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res, body, sizeof(body),
                                          &len),
                       0);
    CFW_TEST_ASSERT_EQ(len, 12);
    CFW_TEST_ASSERT_EQ(memcmp(body, "static world", 12), 0);

    cfw_test_req_header(&req_hdrs[0], "Range", "bytes=-5");
    CFW_TEST_ASSERT_EQ(cfw_test_serve_ex(
                           &mount, CFW_HTTP_METHOD_GET,
                           "/static/hello.txt", req_hdrs, 1, &res,
                           hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_PARTIAL_CONTENT);
    cr = cfw_test_header(&res, "Content-Range");
    CFW_TEST_ASSERT_EQ(strcmp(cr, "bytes 13-17/18"), 0);
    CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res, body, sizeof(body),
                                          &len),
                       0);
    CFW_TEST_ASSERT_EQ(len, 5);
    CFW_TEST_ASSERT_EQ(memcmp(body, "world", 5), 0);

    /* An end past EOF clamps to the last byte. */
    cfw_test_req_header(&req_hdrs[0], "Range", "bytes=0-100");
    CFW_TEST_ASSERT_EQ(cfw_test_serve_ex(
                           &mount, CFW_HTTP_METHOD_GET,
                           "/static/hello.txt", req_hdrs, 1, &res,
                           hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_PARTIAL_CONTENT);
    cr = cfw_test_header(&res, "Content-Range");
    CFW_TEST_ASSERT_EQ(strcmp(cr, "bytes 0-17/18"), 0);
    CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res, body, sizeof(body),
                                          &len),
                       0);
    CFW_TEST_ASSERT_EQ(len, 18);

    /* Unsatisfiable ranges → 416 with the complete length. */
    cfw_test_req_header(&req_hdrs[0], "Range", "bytes=18-");
    CFW_TEST_ASSERT_EQ(cfw_test_serve_ex(
                           &mount, CFW_HTTP_METHOD_GET,
                           "/static/hello.txt", req_hdrs, 1, &res,
                           hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_RANGE_NOT_SATISFIABLE);
    CFW_TEST_ASSERT_TRUE(res.stream == NULL);
    cr = cfw_test_header(&res, "Content-Range");
    CFW_TEST_ASSERT_TRUE(cr != NULL);
    CFW_TEST_ASSERT_EQ(strcmp(cr, "bytes */18"), 0);

    cfw_test_req_header(&req_hdrs[0], "Range", "bytes=-0");
    CFW_TEST_ASSERT_EQ(cfw_test_serve_ex(
                           &mount, CFW_HTTP_METHOD_GET,
                           "/static/hello.txt", req_hdrs, 1, &res,
                           hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_RANGE_NOT_SATISFIABLE);

    /* Any range on an empty file is unsatisfiable. */
    cfw_test_req_header(&req_hdrs[0], "Range", "bytes=0-");
    CFW_TEST_ASSERT_EQ(cfw_test_serve_ex(
                           &mount, CFW_HTTP_METHOD_GET,
                           "/static/empty.txt", req_hdrs, 1, &res,
                           hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_RANGE_NOT_SATISFIABLE);
    cr = cfw_test_header(&res, "Content-Range");
    CFW_TEST_ASSERT_EQ(strcmp(cr, "bytes */0"), 0);

    /* Malformed, multi-range, and foreign-unit headers are ignored
     * and yield the full 200 response. */
    {
        static const char *const ignored[] = {
            "bytes=5-3", "bytes=0-1,3-4", "items=0-1",
            "bytes=abc", "bytes=", "bytes=1-2-3",
        };
        size_t i;
        for (i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++) {
            cfw_test_req_header(&req_hdrs[0], "Range", ignored[i]);
            CFW_TEST_ASSERT_EQ(cfw_test_serve_ex(
                                   &mount, CFW_HTTP_METHOD_GET,
                                   "/static/hello.txt", req_hdrs, 1,
                                   &res, hdrs, 8, &status, &nexted),
                               0);
            CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_OK);
            CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res, body,
                                                  sizeof(body),
                                                  &len),
                               0);
            CFW_TEST_ASSERT_EQ(len, 18);
        }
    }

    /* HEAD with a range reports 206 headers and no body. */
    cfw_test_req_header(&req_hdrs[0], "Range", "bytes=0-4");
    CFW_TEST_ASSERT_EQ(cfw_test_serve_ex(
                           &mount, CFW_HTTP_METHOD_HEAD,
                           "/static/hello.txt", req_hdrs, 1, &res,
                           hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_PARTIAL_CONTENT);
    CFW_TEST_ASSERT_TRUE(res.stream == NULL);
    cr = cfw_test_header(&res, "Content-Range");
    CFW_TEST_ASSERT_EQ(strcmp(cr, "bytes 0-4/18"), 0);

    /* If-Range with the emitted Last-Modified honors the range; a
     * stale date or an entity-tag falls back to the full body. */
    CFW_TEST_ASSERT_EQ(cfw_test_serve(&mount, CFW_HTTP_METHOD_GET,
                                      "/static/hello.txt", &res, hdrs,
                                      8, &status, &nexted),
                       0);
    lm = cfw_test_header(&res, "Last-Modified");
    CFW_TEST_ASSERT_TRUE(lm != NULL);
    CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res, body, sizeof(body),
                                          &len),
                       0);

    cfw_test_req_header(&req_hdrs[0], "Range", "bytes=0-4");
    cfw_test_req_header(&req_hdrs[1], "If-Range", lm);
    CFW_TEST_ASSERT_EQ(cfw_test_serve_ex(
                           &mount, CFW_HTTP_METHOD_GET,
                           "/static/hello.txt", req_hdrs, 2, &res,
                           hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_PARTIAL_CONTENT);
    CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res, body, sizeof(body),
                                          &len),
                       0);
    CFW_TEST_ASSERT_EQ(len, 5);

    cfw_test_req_header(&req_hdrs[1], "If-Range",
                        "Sun, 06 Nov 1994 08:49:37 GMT");
    CFW_TEST_ASSERT_EQ(cfw_test_serve_ex(
                           &mount, CFW_HTTP_METHOD_GET,
                           "/static/hello.txt", req_hdrs, 2, &res,
                           hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res, body, sizeof(body),
                                          &len),
                       0);
    CFW_TEST_ASSERT_EQ(len, 18);

    cfw_test_req_header(&req_hdrs[1], "If-Range", "\"abc123\"");
    CFW_TEST_ASSERT_EQ(cfw_test_serve_ex(
                           &mount, CFW_HTTP_METHOD_GET,
                           "/static/hello.txt", req_hdrs, 2, &res,
                           hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_pull_body(&res, body, sizeof(body),
                                          &len),
                       0);

    /* A satisfied If-Modified-Since wins over Range → 304. */
    cfw_test_req_header(&req_hdrs[0], "Range", "bytes=0-4");
    cfw_test_req_header(&req_hdrs[1], "If-Modified-Since", lm);
    CFW_TEST_ASSERT_EQ(cfw_test_serve_ex(
                           &mount, CFW_HTTP_METHOD_GET,
                           "/static/hello.txt", req_hdrs, 2, &res,
                           hdrs, 8, &status, &nexted),
                       0);
    CFW_TEST_ASSERT_EQ(status, CFW_HTTP_STATUS_NOT_MODIFIED);
    CFW_TEST_ASSERT_TRUE(res.stream == NULL);

    CFW_TEST_ASSERT_EQ(cfw_test_static_teardown(&mount, arena), 0);
    return 0;
}
#endif /* !_WIN32 */

const cfw_test_case cfw_test_static_cases[] = {
#if !defined(_WIN32)
    {"static_mount_init_validation",
     test_static_mount_init_validation},
    {"static_get_file", test_static_get_file},
    {"static_index_and_subdir", test_static_index_and_subdir},
    {"static_head_and_mime", test_static_head_and_mime},
    {"static_errors_and_passthrough",
     test_static_errors_and_passthrough},
    {"static_symlink_confinement", test_static_symlink_confinement},
    {"static_slot_exhaustion", test_static_slot_exhaustion},
    {"static_conditional_requests",
     test_static_conditional_requests},
    {"static_range_requests", test_static_range_requests},
#endif
    {NULL, NULL},
};
