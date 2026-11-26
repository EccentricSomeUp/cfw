/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <stdint.h>
#include <string.h>

#if CFW_PLATFORM_POSIX
#include <dirent.h>
#endif

/*
 * Deterministic mutation fuzzing for the HTTP parser surface. Not
 * coverage-guided; it exists to shake out memory-safety and bounds bugs,
 * which the sanitizers preset turns into hard failures. The PRNG is a
 * fixed-seed xorshift64 so failures reproduce exactly.
 */

static uint64_t cfw_test_rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t cfw_test_rng(void)
{
    uint64_t x = cfw_test_rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    cfw_test_rng_state = x;
    return x;
}

static const char *cfw_test_seeds[] = {
    "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n",
    "POST /submit?a=1 HTTP/1.1\r\nContent-Length: 11\r\n\r\nhello world",
    "PUT /x HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n"
    "4\r\nWiki\r\n5\r\npedia\r\n0\r\nX-T: y\r\n\r\n",
    "OPTIONS * HTTP/1.1\r\nConnection: keep-alive, Upgrade\r\n\r\n",
    "DELETE /admin HTTP/1.1\r\nContent-Length: 0\r\n\r\n",
    "POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nabcde",
    "GET http://example.com/path?q=1 HTTP/1.1\r\n\r\n",
    "CONNECT example.com:443 HTTP/1.1\r\n\r\n",
    "PATCH /p HTTP/1.1\r\nContent-Length: 18446744073709551615\r\n\r\n",
    "POST /x HTTP/1.1\r\nContent-Length: 4\r\nContent-Length: 5\r\n\r\n",
    "GET /x HTTP/1.1\nHost: bare-lf\n\n",
    "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
    "Z\r\n",
    "GET /x HTTP/1.1\r\nX-Fold:\r\n continued\r\n\r\n",
    "POST /x HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
    "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n",
};

static const char cfw_test_tokens[] =
    "\r\n\n\r Content-Length:Transfer-Encoding:chunkedGETPOSTHTTP/1.1"
    "0123456789abcdefABCDEF:;, \t\x00\x01\x7f\x80";

/* Mutates `buf` in place; returns the new length. */
static size_t cfw_test_mutate(char *buf, size_t len, size_t capacity)
{
    size_t ops = 1 + (size_t)(cfw_test_rng() % 4);
    size_t i;
    for (i = 0; i < ops; i++) {
        uint64_t op = cfw_test_rng() % 5;
        if (len == 0) {
            break;
        }
        switch (op) {
        case 0: { /* byte flip */
            size_t pos = (size_t)(cfw_test_rng() % len);
            buf[pos] = (char)(cfw_test_rng() & 0xFF);
            break;
        }
        case 1: { /* truncate */
            len = (size_t)(cfw_test_rng() % (len + 1));
            break;
        }
        case 2: { /* insert token byte sequence */
            size_t pos = (size_t)(cfw_test_rng() % (len + 1));
            size_t tlen = 1 + (size_t)(cfw_test_rng() % 8);
            size_t k;
            if (len + tlen > capacity) {
                break;
            }
            memmove(buf + pos + tlen, buf + pos, len - pos);
            for (k = 0; k < tlen; k++) {
                buf[pos + k] =
                    cfw_test_tokens[cfw_test_rng() %
                                     (sizeof(cfw_test_tokens) - 1)];
            }
            len += tlen;
            break;
        }
        case 3: { /* delete a slice */
            size_t pos = (size_t)(cfw_test_rng() % len);
            size_t dlen = (size_t)(cfw_test_rng() % (len - pos + 1));
            memmove(buf + pos, buf + pos + dlen, len - pos - dlen);
            len -= dlen;
            break;
        }
        default: { /* duplicate a slice */
            size_t pos = (size_t)(cfw_test_rng() % len);
            size_t dlen = (size_t)(cfw_test_rng() % (len - pos));
            if (dlen == 0 || len + dlen > capacity) {
                break;
            }
            memmove(buf + pos + dlen, buf + pos, len - pos);
            memcpy(buf + pos, buf + pos + dlen, dlen);
            len += dlen;
            break;
        }
        }
    }
    return len;
}

/*
 * Dispatch fuzz context: a router with a literal route and a parameterized
 * route feeding cfw_dispatch_request, so successfully parsed fuzz inputs
 * also exercise routing, header/param scratch handling, middleware chain
 * setup, and error mapping — not just the parsers.
 */
typedef struct cfw_fuzz_dispatch {
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
} cfw_fuzz_dispatch_t;

static cfw_status_t cfw_fuzz_handler(cfw_request_t *req, cfw_response_t *res,
                                     void *user)
{
    (void)req;
    (void)user;
    res->status = CFW_HTTP_STATUS_OK;
    return CFW_OK;
}

static cfw_status_t cfw_fuzz_dispatch_init(cfw_fuzz_dispatch_t *ctx)
{
    cfw_dispatch_options_t dopt = {0};
    cfw_strview_t pattern;

    memset(ctx, 0, sizeof(*ctx));
    ctx->arena = cfw_arena_create(64u * 1024u);
    if (ctx->arena == NULL) {
        return CFW_ERROR_NOMEM;
    }
    if (cfw_router_init(&ctx->router, ctx->arena) != CFW_OK) {
        goto fail;
    }
    memset(&dopt, 0, sizeof(dopt));
    dopt.router = &ctx->router;
    dopt.handler = cfw_fuzz_handler;
    dopt.max_middleware = 1;
    if (cfw_dispatch_init(&ctx->dispatch, ctx->arena, &dopt) != CFW_OK) {
        goto fail;
    }
    if (cfw_strview_from_cstr(&pattern, "/") != CFW_OK ||
        cfw_dispatch_add_route(&ctx->dispatch, CFW_HTTP_METHOD_GET, &pattern,
                               cfw_fuzz_handler, NULL) != CFW_OK) {
        goto fail;
    }
    if (cfw_strview_from_cstr(&pattern, "/users/:id") != CFW_OK ||
        cfw_dispatch_add_route(&ctx->dispatch, CFW_HTTP_METHOD_GET, &pattern,
                               cfw_fuzz_handler, NULL) != CFW_OK) {
        goto fail;
    }
    return CFW_OK;

fail:
    cfw_arena_destroy(ctx->arena);
    ctx->arena = NULL;
    return CFW_ERROR_INTERNAL;
}

static void cfw_fuzz_dispatch_free(cfw_fuzz_dispatch_t *ctx)
{
    if (ctx->arena != NULL) {
        cfw_arena_destroy(ctx->arena);
        ctx->arena = NULL;
    }
}

static void cfw_test_fuzz_one(const char *data, size_t len,
                              const cfw_fuzz_dispatch_t *dispatch_ctx)
{
    cfw_strview_t view;
    cfw_http_request_head_t head;
    cfw_http_message_framing_t framing;
    cfw_http_chunked_decoder_t decoder;
    cfw_span_t span;
    cfw_span_t output;
    cfw_http1_server_options_t options;
    cfw_http1_server_t server;
    cfw_http1_event_t event;
    size_t consumed;
    cfw_status_t st;
    int finished = 0;
    int i;

    view.data = data;
    view.size = len;

    /* Head parser must never crash; any status is acceptable. */
    st = cfw_http_request_head_parse(&view, &head);

    if (st == CFW_OK) {
        /* Framing on a successfully parsed head. */
        (void)cfw_http_request_framing(&head, &framing);

        /* Dispatch pipeline on a successfully parsed head. */
        if (dispatch_ctx != NULL) {
            cfw_http_header_t res_headers[8];
            cfw_route_param_t params[4];
            cfw_response_t res;
            cfw_span_t empty_body;

            memset(&res, 0, sizeof(res));
            res.headers = res_headers;
            res.header_capacity = 8;
            memset(&empty_body, 0, sizeof(empty_body));
            (void)cfw_dispatch_request(&dispatch_ctx->dispatch, &head,
                                       &empty_body, res_headers, 8,
                                       params, 4, &res);
        }
    }

    /* Chunked decoder over the raw bytes in one shot. */
    cfw_http_chunked_decoder_init(&decoder);
    cfw_span_init(&span, data, len);
    for (i = 0; i < 64 && !finished; i++) {
        st = cfw_http_chunked_decode(&decoder, &span, &output,
                                     &consumed, &finished);
        if (st != CFW_OK) {
            break;
        }
        if (consumed == 0 && output.size == 0) {
            break;
        }
        span.data += consumed;
        span.size -= consumed;
        if (span.size == 0) {
            break;
        }
    }

    /* Full server pipeline: feed everything, drain events. */
    options.max_head_size = 8192;
    options.max_body_size = 0;
    options.max_pending_size = 1u << 20;
    if (cfw_http1_server_init(&server, &options) == CFW_OK) {
        cfw_span_init(&span, data, len);
        (void)cfw_http1_server_feed(&server, &span);
        for (i = 0; i < 256; i++) {
            st = cfw_http1_server_next_event(&server, &event);
            if (st != CFW_OK) {
                break;
            }
        }
        cfw_http1_server_free(&server);
    }
}

static int test_http_fuzz_mutation(void)
{
    enum { ITERS = 100000, CAP = 512 };
    char buf[CAP];
    size_t seed_count = sizeof(cfw_test_seeds) / sizeof(cfw_test_seeds[0]);
    cfw_fuzz_dispatch_t dispatch_ctx;
    size_t iter;

    CFW_TEST_ASSERT_EQ(cfw_fuzz_dispatch_init(&dispatch_ctx), CFW_OK);

    for (iter = 0; iter < ITERS; iter++) {
        const char *seed = cfw_test_seeds[cfw_test_rng() % seed_count];
        size_t len = strlen(seed);
        if (len > CAP) {
            len = CAP;
        }
        memcpy(buf, seed, len);
        len = cfw_test_mutate(buf, len, CAP);
        cfw_test_fuzz_one(buf, len, &dispatch_ctx);
    }

    cfw_fuzz_dispatch_free(&dispatch_ctx);
    return 0;
}

/*
 * Replays every file in tests/fuzz/corpus through the same parser,
 * decoder, and server-pipeline surface as the mutation fuzzer. The corpus
 * directory is passed via the CFW_FUZZ_CORPUS_DIR compile definition.
 */
static int test_http_fuzz_corpus(void)
{
#if defined(CFW_FUZZ_CORPUS_DIR) && CFW_PLATFORM_POSIX
    enum { MAX_CORPUS_INPUT = 8192 };
    DIR *dir;
    struct dirent *entry;
    cfw_fuzz_dispatch_t dispatch_ctx;
    size_t files = 0;

    CFW_TEST_ASSERT_EQ(cfw_fuzz_dispatch_init(&dispatch_ctx), CFW_OK);

    dir = opendir(CFW_FUZZ_CORPUS_DIR);
    CFW_TEST_ASSERT(dir != NULL);

    while ((entry = readdir(dir)) != NULL) {
        char path[4096];
        FILE *file;
        char buf[MAX_CORPUS_INPUT];
        size_t got;

        if (entry->d_name[0] == '.') {
            continue;
        }
        if (snprintf(path, sizeof(path), "%s/%s", CFW_FUZZ_CORPUS_DIR,
                     entry->d_name) >= (int)sizeof(path)) {
            continue;
        }
        file = fopen(path, "rb");
        if (file == NULL) {
            continue;
        }
        got = fread(buf, 1, sizeof(buf), file);
        fclose(file);
        if (got == 0) {
            continue;
        }
        cfw_test_fuzz_one(buf, got, &dispatch_ctx);
        files++;
    }
    closedir(dir);
    cfw_fuzz_dispatch_free(&dispatch_ctx);

    CFW_TEST_ASSERT(files > 0);
#else
    /* No corpus directory on this platform/configuration. */
#endif
    return 0;
}

const cfw_test_case cfw_test_http_fuzz_cases[] = {
    {"http_fuzz_mutation", test_http_fuzz_mutation},
    {"http_fuzz_corpus", test_http_fuzz_corpus},
    {NULL, NULL},
};
