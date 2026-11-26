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

/* ------------------------------------------------------------------ */
/* Integration: handshake + framed data path over a live connection    */
/* ------------------------------------------------------------------ */

#if !defined(_WIN32)
    #include "private/net.h"

    #include <stdio.h>
    #include <stdlib.h>
    #include <sys/socket.h>
    #include <unistd.h>

typedef struct {
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_server_t server;
    cfw_ws_t *ws;
    int echo;
    int messages;
    cfw_ws_opcode_t last_opcode;
    unsigned char last_payload[2048];
    size_t last_payload_len;
    size_t max_message_size;
    char accept[CFW_WS_ACCEPT_KEY_CAPACITY];
} cfw_test_ws_state;

static cfw_test_ws_state cfw_test_ws_st;

static cfw_status_t cfw_test_ws_http_handler(cfw_request_t *req,
                                             cfw_response_t *res,
                                             void *user)
{
    cfw_test_ws_state *st = user;
    cfw_strview_t name;
    cfw_strview_t value;

    /* Response header strviews must outlive the handler: the accept
     * key is written into per-connection state, not the stack. */
    if (cfw_ws_handshake_validate(req) != CFW_OK ||
        cfw_ws_handshake_accept(req, st->accept,
                                sizeof(st->accept)) != CFW_OK) {
        res->status = CFW_HTTP_STATUS_BAD_REQUEST;
        return CFW_OK;
    }
    res->status = CFW_HTTP_STATUS_SWITCHING_PROTOCOLS;
    cfw_strview_from_cstr(&name, "Upgrade");
    cfw_strview_from_cstr(&value, "websocket");
    cfw_response_header_add(res, &name, &value);
    cfw_strview_from_cstr(&name, "Connection");
    cfw_strview_from_cstr(&value, "Upgrade");
    cfw_response_header_add(res, &name, &value);
    cfw_strview_from_cstr(&name, "Sec-WebSocket-Accept");
    cfw_strview_init(&value, st->accept, strlen(st->accept));
    cfw_response_header_add(res, &name, &value);
    return CFW_OK;
}

static cfw_status_t cfw_test_ws_on_message(cfw_ws_t *ws,
                                           const cfw_ws_message_t *msg,
                                           void *user)
{
    cfw_test_ws_state *st = user;
    (void)ws;
    st->messages++;
    st->last_opcode = msg->opcode;
    if (msg->payload.size <= sizeof(st->last_payload)) {
        memcpy(st->last_payload, msg->payload.data,
               msg->payload.size);
        st->last_payload_len = msg->payload.size;
    }
    if (st->echo) {
        if (msg->opcode == CFW_WS_OPCODE_TEXT) {
            return cfw_ws_send_text(ws, msg->payload.data,
                                    msg->payload.size);
        }
        return cfw_ws_send_binary(ws, msg->payload.data,
                                  msg->payload.size);
    }
    return CFW_OK;
}

static cfw_status_t cfw_test_ws_hook(cfw_upgraded_conn_t *conn,
                                     uint32_t events, void *user)
{
    cfw_test_ws_state *st = user;

    if (events == CFW_UPGRADE_EV_READY) {
        cfw_ws_options_t options;
        memset(&options, 0, sizeof(options));
        options.on_message = cfw_test_ws_on_message;
        options.user = st;
        options.max_message_size = st->max_message_size;
        st->ws = cfw_ws_create(conn, &options);
        return st->ws != NULL ? CFW_OK : CFW_ERROR_INTERNAL;
    }
    if (st->ws != NULL) {
        (void)cfw_ws_on_event(st->ws, events);
    }
    return CFW_OK;
}

static int cfw_test_ws_setup(cfw_test_ws_state *st)
{
    cfw_server_options_t options;
    cfw_dispatch_options_t doptions;
    cfw_strview_t pattern;

    memset(st, 0, sizeof(*st));
    st->max_message_size = 0;
    st->arena = cfw_arena_create(32768);
    CFW_TEST_ASSERT_TRUE(st->arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&st->router, st->arena), CFW_OK);

    memset(&doptions, 0, sizeof(doptions));
    doptions.router = &st->router;
    doptions.handler = cfw_test_ws_http_handler;
    doptions.handler_user = st;
    doptions.max_middleware = 4;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&st->dispatch, st->arena,
                                         &doptions),
                       CFW_OK);
    cfw_strview_from_cstr(&pattern, "/ws");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(&st->dispatch,
                                              CFW_HTTP_METHOD_GET,
                                              &pattern,
                                              cfw_test_ws_http_handler,
                                              st),
                       CFW_OK);

    memset(&options, 0, sizeof(options));
    options.host = "127.0.0.1";
    options.port = 0;
    options.backlog = 16;
    options.dispatch = &st->dispatch;
    options.http.max_head_size = 8192;
    options.http.max_body_size = 65536;
    options.http.max_pending_size = 65536;
    options.max_out_size = 65536;
    options.max_connections = 8;
    options.max_headers = 32;
    options.max_params = 16;
    options.upgrade = cfw_test_ws_hook;
    options.upgrade_user = st;
    CFW_TEST_ASSERT_EQ(cfw_server_init(&st->server, &options), CFW_OK);
    return 0;
}

static void cfw_test_ws_teardown(cfw_test_ws_state *st)
{
    if (st->ws != NULL) {
        cfw_ws_destroy(st->ws);
        st->ws = NULL;
    }
    cfw_server_free(&st->server);
    cfw_arena_destroy(st->arena);
}

static cfw_status_t cfw_test_ws_connect(cfw_fd_t *client, uint16_t port)
{
    cfw_sockaddr_t addr;
    CFW_TEST_ASSERT_EQ(cfw_addr_from_str(&addr, "127.0.0.1", port),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_socket(AF_INET, SOCK_STREAM, 0, client),
                       CFW_OK);
    {
        cfw_status_t st = cfw_net_connect(client, &addr);
        if (st != CFW_OK && st != CFW_ERROR_WOULDBLOCK) {
            return st;
        }
    }
    return CFW_OK;
}

static int cfw_test_ws_send(cfw_fd_t *client, const void *data,
                            size_t size)
{
    ssize_t n = send((int)cfw_fd_get(client), data, size, 0);
    return n == (ssize_t)size ? 0 : 1;
}

/* Steps the server and appends received bytes until `marker` appears,
 * EOF, or the attempt budget is exhausted. */
static ssize_t cfw_test_ws_recv_until(cfw_server_t *server,
                                      cfw_fd_t *client, char *buffer,
                                      size_t capacity, const char *marker,
                                      size_t *used)
{
    cfw_duration_t slice;
    int attempts;
    size_t mlen = strlen(marker);

    cfw_duration_from_millis(50, &slice);
    for (attempts = 0; attempts < 400 && *used + 1 < capacity;
         attempts++) {
        cfw_timepoint_t deadline;
        cfw_deadline_from_now(&deadline, slice);
        ssize_t n;
        if (cfw_server_step(server, &deadline) != CFW_OK) {
            return -1;
        }
        n = recv((int)cfw_fd_get(client), buffer + *used,
                 capacity - *used - 1, MSG_DONTWAIT);
        if (n > 0) {
            *used += (size_t)n;
            buffer[*used] = '\0';
            if (*used >= mlen &&
                memmem(buffer, *used, marker, mlen) != NULL) {
                return (ssize_t)*used;
            }
        } else if (n == 0) {
            return (ssize_t)*used;
        }
    }
    return -1;
}

/* Builds a masked client->server frame into `out` (capacity must hold
 * header + payload). Returns the frame length. */
static size_t cfw_test_ws_masked_frame(uint8_t *out, int fin, int opcode,
                                       const void *payload, size_t len)
{
    static const uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    const uint8_t *p = payload;
    size_t n = 0;
    size_t i;

    out[n++] = (uint8_t)((fin ? 0x80 : 0) | opcode);
    if (len < 126) {
        out[n++] = (uint8_t)(0x80 | len);
    } else if (len <= 0xffff) {
        out[n++] = (uint8_t)(0x80 | 126);
        out[n++] = (uint8_t)(len >> 8);
        out[n++] = (uint8_t)len;
    } else {
        int k;
        out[n++] = (uint8_t)(0x80 | 127);
        for (k = 7; k >= 0; k--) {
            out[n++] = (uint8_t)((uint64_t)len >> (k * 8));
        }
    }
    memcpy(out + n, mask, 4);
    n += 4;
    for (i = 0; i < len; i++) {
        out[n++] = (uint8_t)(p[i] ^ mask[i & 3]);
    }
    return n;
}

/* Parses the first server->client (unmasked) frame in `buf`.
 * Returns 1 on success and fills opcode/payload pointer+size. */
static int cfw_test_ws_parse_frame(const uint8_t *buf, size_t len,
                                   int *opcode, const uint8_t **payload,
                                   size_t *payload_len)
{
    uint64_t plen;
    size_t pos = 2;
    int i;

    if (len < 2) {
        return 0;
    }
    *opcode = buf[0] & 0x0f;
    plen = buf[1] & 0x7f;
    if (plen == 126) {
        if (len < 4) {
            return 0;
        }
        plen = ((uint64_t)buf[2] << 8) | buf[3];
        pos = 4;
    } else if (plen == 127) {
        if (len < 10) {
            return 0;
        }
        plen = 0;
        for (i = 0; i < 8; i++) {
            plen = (plen << 8) | buf[2 + i];
        }
        pos = 10;
    }
    if ((buf[1] & 0x80) || plen > len - pos) {
        return 0; /* server frames must be unmasked; need full payload */
    }
    *payload = buf + pos;
    *payload_len = (size_t)plen;
    return 1;
}

/* Performs the WS opening handshake and returns the 101 response. */
static int cfw_test_ws_handshake(cfw_server_t *server, cfw_fd_t *client,
                                 char *response, size_t capacity)
{
    size_t used = 0;
    char request[1024];
    int n = snprintf(request, sizeof(request),
                     "GET /ws HTTP/1.1\r\n"
                     "Host: t\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "Sec-WebSocket-Key: %s\r\n\r\n",
                     "dGhlIHNhbXBsZSBub25jZQ==");
    CFW_TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(request));
    CFW_TEST_ASSERT_EQ(cfw_test_ws_send(client, request,
                                        (size_t)n),
                       0);
    n = (int)cfw_test_ws_recv_until(server, client, response, capacity,
                                    "101", &used);
    CFW_TEST_ASSERT_TRUE(n > 0);
    response[used] = '\0';
    CFW_TEST_ASSERT_TRUE(strstr(response, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") !=
                         NULL);
    return 0;
}

static int test_ws_text_echo(void)
{
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t client;
    char response[8192];
    uint8_t frame[256];
    size_t flen;
    size_t used = 0;
    int opcode;
    const uint8_t *payload;
    size_t payload_len;

    cfw_test_ws_setup(st);
    st->echo = 1;
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_connect(&client,
                                           cfw_server_port(&st->server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_handshake(&st->server, &client,
                                             response,
                                             sizeof(response)),
                       0);

    /* Masked text frame "hello". */
    flen = cfw_test_ws_masked_frame(frame, 1, 0x1, "hello", 5);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_send(&client, frame, flen), 0);

    CFW_TEST_ASSERT_TRUE(cfw_test_ws_recv_until(
                             &st->server, &client, response,
                             sizeof(response), "hello", &used) > 0);
    CFW_TEST_ASSERT_TRUE(cfw_test_ws_parse_frame(
                             (const uint8_t *)response, used, &opcode,
                             &payload, &payload_len));
    CFW_TEST_ASSERT_EQ(opcode, 0x1);
    CFW_TEST_ASSERT_EQ(payload_len, 5);
    CFW_TEST_ASSERT_EQ(memcmp(payload, "hello", 5), 0);
    CFW_TEST_ASSERT_EQ(st->messages, 1);
    CFW_TEST_ASSERT_EQ(st->last_opcode, CFW_WS_OPCODE_TEXT);

    cfw_fd_close(&client);
    cfw_test_ws_teardown(st);
    return 0;
}

static int test_ws_fragmented_message(void)
{
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t client;
    char response[8192];
    uint8_t frame[256];
    size_t flen;
    size_t used = 0;

    cfw_test_ws_setup(st);
    st->echo = 1;
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_connect(&client,
                                           cfw_server_port(&st->server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_handshake(&st->server, &client,
                                             response,
                                             sizeof(response)),
                       0);

    /* Text frame fin=0 "Hel" + continuation fin=1 "lo world". */
    flen = cfw_test_ws_masked_frame(frame, 0, 0x1, "Hel", 3);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_send(&client, frame, flen), 0);
    flen = cfw_test_ws_masked_frame(frame, 1, 0x0, "lo world", 8);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_send(&client, frame, flen), 0);

    CFW_TEST_ASSERT_TRUE(cfw_test_ws_recv_until(
                             &st->server, &client, response,
                             sizeof(response), "Hello world", &used) > 0);
    CFW_TEST_ASSERT_EQ(st->messages, 1);
    CFW_TEST_ASSERT_EQ(st->last_payload_len, 11);
    CFW_TEST_ASSERT_EQ(memcmp(st->last_payload, "Hello world", 11), 0);

    cfw_fd_close(&client);
    cfw_test_ws_teardown(st);
    return 0;
}

static int test_ws_ping_auto_pong(void)
{
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t client;
    char response[8192];
    uint8_t frame[64];
    size_t flen;
    size_t used = 0;
    int opcode;
    const uint8_t *payload;
    size_t payload_len;

    cfw_test_ws_setup(st);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_connect(&client,
                                           cfw_server_port(&st->server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_handshake(&st->server, &client,
                                             response,
                                             sizeof(response)),
                       0);

    flen = cfw_test_ws_masked_frame(frame, 1, 0x9, "abc", 3);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_send(&client, frame, flen), 0);

    CFW_TEST_ASSERT_TRUE(cfw_test_ws_recv_until(
                             &st->server, &client, response,
                             sizeof(response), "abc", &used) > 0);
    /* The pong payload "abc" appears verbatim (server frames are
     * unmasked). */
    CFW_TEST_ASSERT_TRUE(cfw_test_ws_parse_frame(
                             (const uint8_t *)response, used, &opcode,
                             &payload, &payload_len));
    CFW_TEST_ASSERT_EQ(opcode, 0xA);
    CFW_TEST_ASSERT_EQ(payload_len, 3);
    CFW_TEST_ASSERT_EQ(memcmp(payload, "abc", 3), 0);
    CFW_TEST_ASSERT_EQ(st->messages, 0);

    cfw_fd_close(&client);
    cfw_test_ws_teardown(st);
    return 0;
}

static int test_ws_close_echo(void)
{
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t client;
    char response[8192];
    uint8_t frame[64];
    size_t flen;
    size_t used = 0;
    int opcode;
    const uint8_t *payload;
    size_t payload_len;

    cfw_test_ws_setup(st);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_connect(&client,
                                           cfw_server_port(&st->server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_handshake(&st->server, &client,
                                             response,
                                             sizeof(response)),
                       0);

    /* Close frame with status 1000 (0x03E8) and no reason. */
    {
        uint8_t closepl[2] = {0x03, 0xe8};
        flen = cfw_test_ws_masked_frame(frame, 1, 0x8, closepl, 2);
    }
    CFW_TEST_ASSERT_EQ(cfw_test_ws_send(&client, frame, flen), 0);

    /* Wait for the echoed close frame: header 0x88 0x02 then status. */
    CFW_TEST_ASSERT_TRUE(cfw_test_ws_recv_until(
                             &st->server, &client, response,
                             sizeof(response), "\x88\x02\x03\xe8",
                             &used) > 0);
    CFW_TEST_ASSERT_TRUE(cfw_test_ws_parse_frame(
                             (const uint8_t *)response, used, &opcode,
                             &payload, &payload_len));
    CFW_TEST_ASSERT_EQ(opcode, 0x8);
    CFW_TEST_ASSERT_EQ(payload_len, 2);
    CFW_TEST_ASSERT_EQ(payload[0], 0x03);
    CFW_TEST_ASSERT_EQ(payload[1], 0xe8);
    CFW_TEST_ASSERT_TRUE(cfw_ws_is_closed(st->ws));

    cfw_fd_close(&client);
    cfw_test_ws_teardown(st);
    return 0;
}

static int test_ws_unmasked_rejected(void)
{
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t client;
    char response[8192];
    uint8_t frame[64];
    size_t used = 0;

    cfw_test_ws_setup(st);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_connect(&client,
                                           cfw_server_port(&st->server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_handshake(&st->server, &client,
                                             response,
                                             sizeof(response)),
                       0);

    /* Unmasked text frame: MASK bit clear → protocol error 1002. */
    frame[0] = 0x81;
    frame[1] = 0x02;
    frame[2] = 'h';
    frame[3] = 'i';
    CFW_TEST_ASSERT_EQ(cfw_test_ws_send(&client, frame, 4), 0);

    /* Expect a close frame carrying status 1002 (0x03EA). */
    CFW_TEST_ASSERT_TRUE(cfw_test_ws_recv_until(
                             &st->server, &client, response,
                             sizeof(response), "\x88\x02\x03\xea",
                             &used) > 0);
    CFW_TEST_ASSERT_TRUE(cfw_ws_is_closed(st->ws));
    CFW_TEST_ASSERT_EQ(st->messages, 0);

    cfw_fd_close(&client);
    cfw_test_ws_teardown(st);
    return 0;
}

static int test_ws_invalid_utf8_rejected(void)
{
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t client;
    char response[8192];
    uint8_t frame[64];
    size_t flen;
    size_t used = 0;

    cfw_test_ws_setup(st);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_connect(&client,
                                           cfw_server_port(&st->server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_handshake(&st->server, &client,
                                             response,
                                             sizeof(response)),
                       0);

    /* Masked text frame whose payload is not valid UTF-8 (0xFF). */
    flen = cfw_test_ws_masked_frame(frame, 1, 0x1, "\xff\xfe", 2);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_send(&client, frame, flen), 0);

    /* Close code 1007 (0x03EF). */
    CFW_TEST_ASSERT_TRUE(cfw_test_ws_recv_until(
                             &st->server, &client, response,
                             sizeof(response), "\x88\x02\x03\xef",
                             &used) > 0);
    CFW_TEST_ASSERT_EQ(st->messages, 0);

    cfw_fd_close(&client);
    cfw_test_ws_teardown(st);
    return 0;
}

static int test_ws_message_too_big(void)
{
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t client;
    char response[8192];
    uint8_t frame[256];
    size_t flen;
    size_t used = 0;

    /* The hook reads max_message_size at READY time (during the
     * handshake), so set the bound after setup but before connect. */
    cfw_test_ws_setup(st);
    st->max_message_size = 8;
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_connect(&client,
                                           cfw_server_port(&st->server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_handshake(&st->server, &client,
                                             response,
                                             sizeof(response)),
                       0);

    flen = cfw_test_ws_masked_frame(frame, 1, 0x2,
                                    "0123456789abcdef", 16);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_send(&client, frame, flen), 0);

    /* Close code 1009 (0x03F1). */
    CFW_TEST_ASSERT_TRUE(cfw_test_ws_recv_until(
                             &st->server, &client, response,
                             sizeof(response), "\x88\x02\x03\xf1",
                             &used) > 0);
    CFW_TEST_ASSERT_EQ(st->messages, 0);

    cfw_fd_close(&client);
    cfw_test_ws_teardown(st);
    return 0;
}

static int test_ws_send_close_from_app(void)
{
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t client;
    char response[8192];
    size_t used = 0;

    cfw_test_ws_setup(st);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_connect(&client,
                                           cfw_server_port(&st->server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_handshake(&st->server, &client,
                                             response,
                                             sizeof(response)),
                       0);

    /* Wait for READY (ws created during the handshake), then close
     * from the app side. */
    CFW_TEST_ASSERT_TRUE(st->ws != NULL);
    CFW_TEST_ASSERT_EQ(cfw_ws_send_close(st->ws, 1000, "bye", 3),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_ws_is_closed(st->ws));

    /* The close frame (0x88, len 5, status 1000, "bye") must reach the
     * client after the server steps. */
    {
        uint8_t expect[7] = {0x88, 0x05, 0x03, 0xe8, 'b', 'y', 'e'};
        char needle[8];
        memcpy(needle, expect, 7);
        needle[7] = '\0';
        CFW_TEST_ASSERT_TRUE(cfw_test_ws_recv_until(
                                 &st->server, &client, response,
                                 sizeof(response), needle, &used) > 0);
    }

    cfw_fd_close(&client);
    cfw_test_ws_teardown(st);
    return 0;
}
#endif /* !_WIN32 */

const cfw_test_case cfw_test_ws_cases[] = {
    {"ws_accept_key_rfc_vector", test_ws_accept_key_rfc_vector},
    {"ws_accept_key_args", test_ws_accept_key_args},
    {"ws_handshake_validate_ok", test_ws_handshake_validate_ok},
    {"ws_handshake_validate_rejects", test_ws_handshake_validate_rejects},
    {"ws_handshake_accept_extract", test_ws_handshake_accept_extract},
#if !defined(_WIN32)
    {"ws_text_echo", test_ws_text_echo},
    {"ws_fragmented_message", test_ws_fragmented_message},
    {"ws_ping_auto_pong", test_ws_ping_auto_pong},
    {"ws_close_echo", test_ws_close_echo},
    {"ws_unmasked_rejected", test_ws_unmasked_rejected},
    {"ws_invalid_utf8_rejected", test_ws_invalid_utf8_rejected},
    {"ws_message_too_big", test_ws_message_too_big},
    {"ws_send_close_from_app", test_ws_send_close_from_app},
#endif
    {NULL, NULL},
};
