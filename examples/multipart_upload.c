/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Multipart form parsing example: a POST /upload route that parses
 * multipart/form-data bodies with cfw_multipart_boundary +
 * cfw_multipart_parse (ADR 0043) and reports each part's field name,
 * filename, and size.
 *
 * Usage: cfw_example_multipart_upload
 * Test with:
 *   curl -F name=alice -F avatar=@/tmp/pic.png http://127.0.0.1:8080/upload
 */

#include <cfw/cfw.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CFW_Multipart_MAX_PARTS 16

static cfw_status_t upload(cfw_request_t *req, cfw_response_t *res,
                           void *user)
{
    cfw_multipart_part_t parts[CFW_Multipart_MAX_PARTS];
    cfw_strview_t content_type;
    cfw_strview_t boundary;
    char *scratch = NULL;
    char out[4096];
    size_t count = 0;
    size_t dropped = 0;
    size_t used = 0;
    size_t i;
    (void)user;

    res->status = CFW_HTTP_STATUS_OK;

    {
        cfw_strview_t ct_name;
        cfw_strview_from_cstr(&ct_name, "Content-Type");
        if (cfw_request_header(req, &ct_name, &content_type) != CFW_OK) {
            res->status = CFW_HTTP_STATUS_BAD_REQUEST;
            return CFW_OK;
        }
    }
    if (cfw_multipart_boundary(&content_type, &boundary) != CFW_OK) {
        res->status = CFW_HTTP_STATUS_BAD_REQUEST;
        return CFW_OK;
    }

    /* Unescaped name/filename bytes need up to body->size scratch. */
    scratch = (char *)malloc(req->body.size == 0 ? 1 : req->body.size);
    if (scratch == NULL) {
        res->status = CFW_HTTP_STATUS_INTERNAL_SERVER_ERROR;
        return CFW_OK;
    }
    if (cfw_multipart_parse(&req->body, &boundary, parts,
                            CFW_Multipart_MAX_PARTS, scratch,
                            req->body.size, &count, &dropped) != CFW_OK) {
        free(scratch);
        res->status = CFW_HTTP_STATUS_BAD_REQUEST;
        return CFW_OK;
    }

    used = (size_t)snprintf(out, sizeof(out), "parts: %zu\n",
                            count + dropped);
    for (i = 0; i < count && used < sizeof(out); i++) {
        int n = snprintf(out + used, sizeof(out) - used,
                         "- name=%.*s filename=%.*s size=%zu type=%.*s\n",
                         (int)parts[i].name.size, parts[i].name.data,
                         (int)parts[i].filename.size,
                         parts[i].filename.data, parts[i].body.size,
                         (int)parts[i].content_type.size,
                         parts[i].content_type.data);
        if (n < 0) {
            break;
        }
        used += (size_t)n < sizeof(out) - used
                    ? (size_t)n
                    : sizeof(out) - used - 1u;
    }
    free(scratch);

    res->body.data = (const uint8_t *)out;
    res->body.size = used;
    return CFW_OK;
}

int main(void)
{
    cfw_arena_t *arena = cfw_arena_create(1u << 20);
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t dopt = {0};
    cfw_server_options_t sopt = {0};
    cfw_server_t server;
    cfw_strview_t pattern;
    int ok = 0;

    if (arena == NULL) {
        return 1;
    }
    if (cfw_router_init(&router, arena) != CFW_OK) {
        goto fail;
    }
    memset(&dopt, 0, sizeof(dopt));
    dopt.router = &router;
    dopt.handler = upload;
    dopt.max_middleware = 4;
    if (cfw_dispatch_init(&dispatch, arena, &dopt) != CFW_OK) {
        goto fail;
    }
    if (cfw_strview_from_cstr(&pattern, "/upload") != CFW_OK ||
        cfw_dispatch_add_route(&dispatch, CFW_HTTP_METHOD_POST, &pattern,
                               upload, NULL) != CFW_OK) {
        goto fail;
    }
    memset(&sopt, 0, sizeof(sopt));
    sopt.host = "127.0.0.1";
    sopt.port = 8080;
    sopt.dispatch = &dispatch;
    sopt.http.max_head_size = 8192;
    sopt.http.max_pending_size = 1u << 20; /* multipart bodies */
    sopt.max_out_size = 8192;
    sopt.max_connections = 128;
    sopt.max_headers = 32;
    sopt.max_params = 16;
    if (cfw_server_init(&server, &sopt) != CFW_OK) {
        goto fail;
    }
    fprintf(stderr, "cfw multipart example listening on "
                    "http://%s:%u/upload\n",
            sopt.host, (unsigned)cfw_server_port(&server));
    cfw_server_run(&server);
    ok = 1;

fail:
    cfw_arena_destroy(arena);
    return ok ? 0 : 1;
}
