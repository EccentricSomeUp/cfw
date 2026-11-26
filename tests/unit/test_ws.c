/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <string.h>

/* Builds a minimal cfw_request_t with the given headers. `headers` must
 * have room for `count` entries; names/values are string literals. */
static void cfw_test_ws_req(cfw_request_t *req, cfw_http_header_t *headers,
                            const char *const names[],
                            const char *const values[], size_t count)
{
    size_t i;
    memset(req, 0, sizeof(*req));
    memset(headers, 0, sizeof(*headers) * count);
    req->method = CFW_HTTP_METHOD_GET;
    req->headers = headers;
    req->header_count = count;
    for (i = 0; i < count; i++) {
        cfw_strview_from_cstr(&headers[i].name, names[i]);
        cfw_strview_from_cstr(&headers[i].value, values[i]);
    }
}

static int test_ws_accept_key_rfc_vector(void)
{
    /* RFC 6455 section 1.3 example: the accept value is a fixed
     * known-answer covering SHA-1 + base64 + GUID concatenation. */
    char out[CFW_WS_ACCEPT_KEY_CAPACITY];
    CFW_TEST_ASSERT_EQ(cfw_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==",
                                         24, out, sizeof(out)),
                       CFW_OK);
    CFW_TEST_ASSERT_STR_EQ(out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    CFW_TEST_ASSERT_EQ(strlen(out), 28);
    return 0;
}

static int test_ws_accept_key_args(void)
{
    char out[CFW_WS_ACCEPT_KEY_CAPACITY];
    CFW_TEST_ASSERT_EQ(cfw_ws_accept_key(NULL, 4, out, sizeof(out)),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_ws_accept_key("abc", 0, out, sizeof(out)),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_ws_accept_key("abc", 300, out, sizeof(out)),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_ws_accept_key("abc", 3, NULL,
                                         CFW_WS_ACCEPT_KEY_CAPACITY),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_ws_accept_key("abc", 3, out,
                                         CFW_WS_ACCEPT_KEY_CAPACITY - 1),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_ws_handshake_validate_ok(void)
{
    cfw_request_t req;
    cfw_http_header_t headers[5];
    const char *names[] = {"Host", "Upgrade", "Connection",
                           "Sec-WebSocket-Version", "Sec-WebSocket-Key"};
    const char *values[] = {"example.com", "websocket",
                            "keep-alive, Upgrade", "13",
                            "dGhlIHNhbXBsZSBub25jZQ=="};

    cfw_test_ws_req(&req, headers, names, values, 5);
    CFW_TEST_ASSERT_EQ(cfw_ws_handshake_validate(&req), CFW_OK);
    return 0;
}

static int test_ws_handshake_validate_rejects(void)
{
    cfw_request_t req;
    cfw_http_header_t headers[5];
    const char *names[] = {"Upgrade", "Connection", "Sec-WebSocket-Version",
                           "Sec-WebSocket-Key", "Host"};
    const char *good_values[] = {"websocket", "Upgrade", "13",
                                 "dGhlIHNhbXBsZSBub25jZQ==", "h"};
    const char *values[5];
    size_t i;

    /* Baseline valid set, then mutate one field per case. */
    for (i = 0; i < 5; i++) {
        values[i] = good_values[i];
    }

    /* Wrong method. */
    cfw_test_ws_req(&req, headers, names, values, 5);
    req.method = CFW_HTTP_METHOD_POST;
    CFW_TEST_ASSERT_EQ(cfw_ws_handshake_validate(&req),
                       CFW_ERROR_INVALID_ARGUMENT);
    req.method = CFW_HTTP_METHOD_GET;

    /* Missing Upgrade header. */
    {
        const char *n2[] = {"Connection", "Sec-WebSocket-Version",
                            "Sec-WebSocket-Key", "Host"};
        const char *v2[] = {"Upgrade", "13",
                            "dGhlIHNhbXBsZSBub25jZQ==", "h"};
        cfw_test_ws_req(&req, headers, n2, v2, 4);
        CFW_TEST_ASSERT_EQ(cfw_ws_handshake_validate(&req),
                           CFW_ERROR_INVALID_ARGUMENT);
    }

    /* Upgrade token not "websocket". */
    values[0] = "h2c";
    cfw_test_ws_req(&req, headers, names, values, 5);
    CFW_TEST_ASSERT_EQ(cfw_ws_handshake_validate(&req),
                       CFW_ERROR_INVALID_ARGUMENT);
    values[0] = "websocket";

    /* Connection without the upgrade token. */
    values[1] = "keep-alive";
    cfw_test_ws_req(&req, headers, names, values, 5);
    CFW_TEST_ASSERT_EQ(cfw_ws_handshake_validate(&req),
                       CFW_ERROR_INVALID_ARGUMENT);
    values[1] = "Upgrade";

    /* Wrong version. */
    values[2] = "12";
    cfw_test_ws_req(&req, headers, names, values, 5);
    CFW_TEST_ASSERT_EQ(cfw_ws_handshake_validate(&req),
                       CFW_ERROR_INVALID_ARGUMENT);
    values[2] = "13";

    /* Key not base64 / wrong decoded length. */
    values[3] = "!!!not-base64!!!";
    cfw_test_ws_req(&req, headers, names, values, 5);
    CFW_TEST_ASSERT_EQ(cfw_ws_handshake_validate(&req),
                       CFW_ERROR_INVALID_ARGUMENT);
    values[3] = "AAAA"; /* valid base64 but decodes to 3 bytes */
    cfw_test_ws_req(&req, headers, names, values, 5);
    CFW_TEST_ASSERT_EQ(cfw_ws_handshake_validate(&req),
                       CFW_ERROR_INVALID_ARGUMENT);
    values[3] = "dGhlIHNhbXBsZSBub25jZQ==";

    /* NULL request. */
    CFW_TEST_ASSERT_EQ(cfw_ws_handshake_validate(NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_ws_handshake_accept_extract(void)
{
    cfw_request_t req;
    cfw_http_header_t headers[2];
    const char *names[] = {"Sec-WebSocket-Key", "Host"};
    const char *values[] = {"dGhlIHNhbXBsZSBub25jZQ==", "h"};
    char out[CFW_WS_ACCEPT_KEY_CAPACITY];
    char direct[CFW_WS_ACCEPT_KEY_CAPACITY];

    cfw_test_ws_req(&req, headers, names, values, 2);
    CFW_TEST_ASSERT_EQ(cfw_ws_handshake_accept(&req, out, sizeof(out)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_ws_accept_key(values[0], strlen(values[0]),
                                         direct, sizeof(direct)),
                       CFW_OK);
    CFW_TEST_ASSERT_STR_EQ(out, direct);
    CFW_TEST_ASSERT_STR_EQ(out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    /* Missing key header. */
    {
        const char *n2[] = {"Host"};
        const char *v2[] = {"h"};
        cfw_request_t req2;
        cfw_http_header_t h2[1];
        cfw_test_ws_req(&req2, h2, n2, v2, 1);
        CFW_TEST_ASSERT_EQ(cfw_ws_handshake_accept(&req2, out,
                                                 sizeof(out)),
                           CFW_ERROR_INVALID_ARGUMENT);
    }
    CFW_TEST_ASSERT_EQ(cfw_ws_handshake_accept(NULL, out, sizeof(out)),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

const cfw_test_case cfw_test_ws_cases[] = {
    {"ws_accept_key_rfc_vector", test_ws_accept_key_rfc_vector},
    {"ws_accept_key_args", test_ws_accept_key_args},
    {"ws_handshake_validate_ok", test_ws_handshake_validate_ok},
    {"ws_handshake_validate_rejects", test_ws_handshake_validate_rejects},
    {"ws_handshake_accept_extract", test_ws_handshake_accept_extract},
    {NULL, NULL},
};
