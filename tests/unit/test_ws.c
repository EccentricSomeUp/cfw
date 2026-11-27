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

#if CFW_PLATFORM_POSIX
    #include "private/net.h"

    #include <errno.h>
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
    /* FNV-1a over the full payload regardless of size — verifies
     * reassembly of messages larger than last_payload. */
    uint64_t last_payload_hash;
    size_t max_message_size;
    char accept[CFW_WS_ACCEPT_KEY_CAPACITY];
    /* All live cfw_ws_t objects (multi-connection tests); st->ws
     * mirrors the most recently created one for single-conn tests. */
    cfw_ws_t *ws_list[16];
    size_t ws_count;
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
    {
        uint64_t h = 1469598103934665603ull;
        size_t i;
        for (i = 0; i < msg->payload.size; i++) {
            h = (h ^ msg->payload.data[i]) * 1099511628211ull;
        }
        st->last_payload_hash = h;
    }
    st->last_payload_len = msg->payload.size;
    if (msg->payload.size != 0 &&
        msg->payload.size <= sizeof(st->last_payload)) {
        memcpy(st->last_payload, msg->payload.data,
               msg->payload.size);
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
    cfw_ws_t *ws;

    if (events == CFW_UPGRADE_EV_CLOSE) {
        ws = cfw_upgraded_user(conn);
        if (ws != NULL) {
            size_t i;
            cfw_ws_destroy(ws);
            for (i = 0; i < st->ws_count; i++) {
                if (st->ws_list[i] == ws) {
                    st->ws_list[i] = st->ws_list[--st->ws_count];
                    break;
                }
            }
            if (st->ws == ws) {
                st->ws = NULL;
            }
        }
        return CFW_OK;
    }
    if (events == CFW_UPGRADE_EV_READY) {
        cfw_ws_options_t options;
        memset(&options, 0, sizeof(options));
        options.on_message = cfw_test_ws_on_message;
        options.user = st;
        options.max_message_size = st->max_message_size;
        ws = cfw_ws_create(conn, &options);
        if (ws == NULL) {
            return CFW_ERROR_INTERNAL;
        }
        cfw_upgraded_set_user(conn, ws);
        if (st->ws_count < sizeof(st->ws_list) / sizeof(st->ws_list[0])) {
            st->ws_list[st->ws_count++] = ws;
        }
        st->ws = ws;
        return CFW_OK;
    }
    ws = cfw_upgraded_user(conn);
    if (ws != NULL) {
        (void)cfw_ws_on_event(ws, events);
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
    /* Freeing the server tears down live conns, which delivers
     * EV_CLOSE and lets the hook destroy each ws object. Anything
     * still tracked afterwards was missed by the hook; destroy it
     * directly so nothing leaks. */
    cfw_server_free(&st->server);
    while (st->ws_count != 0) {
        cfw_ws_destroy(st->ws_list[--st->ws_count]);
    }
    st->ws = NULL;
    cfw_arena_destroy(st->arena);
}

static cfw_status_t cfw_test_ws_connect(cfw_fd_t *client, uint16_t port)
{
    cfw_sockaddr_t addr;
    cfw_status_t st;

    CFW_TEST_ASSERT_EQ(cfw_addr_from_str(&addr, "127.0.0.1", port), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_socket(AF_INET, SOCK_STREAM, 0, client),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_set_nonblocking(client, 0), CFW_OK);
    st = cfw_net_connect(client, &addr);
    CFW_TEST_ASSERT_EQ(cfw_net_set_nonblocking(client, 1), CFW_OK);
    return st;
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

/* Returns the total wire length of the first frame in `buf`
 * (header + payload), or 0 when incomplete/invalid. */
static size_t cfw_test_ws_frame_len(const uint8_t *buf, size_t len)
{
    uint64_t plen;
    size_t pos = 2;
    int i;

    if (len < 2) {
        return 0;
    }
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
    if (buf[1] & 0x80) {
        return 0; /* server frames are unmasked */
    }
    if (plen > len - pos || plen > SIZE_MAX - pos) {
        return 0;
    }
    return pos + (size_t)plen;
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

    /* The server finishes the close and tears the conn down; the
     * EV_CLOSE hook event must release the ws object exactly once. */
    {
        int attempts;
        for (attempts = 0; attempts < 100; attempts++) {
            cfw_timepoint_t deadline;
            cfw_duration_t slice;
            cfw_duration_from_millis(20, &slice);
            cfw_deadline_from_now(&deadline, slice);
            CFW_TEST_ASSERT_EQ(cfw_server_step(&st->server, &deadline),
                               CFW_OK);
            if (cfw_server_connection_count(&st->server) == 0) {
                break;
            }
        }
        CFW_TEST_ASSERT_EQ(cfw_server_connection_count(&st->server), 0);
        CFW_TEST_ASSERT_TRUE(st->ws == NULL);
    }

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

/* Sends one masked frame and expects the connection to die with the
 * given close status code (2-byte big-endian in the close payload). */
static int cfw_test_ws_expect_close(cfw_test_ws_state *st,
                                    cfw_fd_t *client, char *response,
                                    size_t capacity, const uint8_t *frame,
                                    size_t flen, uint16_t status)
{
    size_t used = 0;
    char nstr[5];

    /* Close frame: 0x88 0x02 <status hi> <status lo>. */
    nstr[0] = (char)0x88;
    nstr[1] = (char)0x02;
    nstr[2] = (char)(status >> 8);
    nstr[3] = (char)status;
    nstr[4] = '\0';
    CFW_TEST_ASSERT_EQ(cfw_test_ws_send(client, frame, flen), 0);
    CFW_TEST_ASSERT_TRUE(cfw_test_ws_recv_until(
                             &st->server, client, response, capacity,
                             nstr, &used) > 0);
    CFW_TEST_ASSERT_TRUE(cfw_ws_is_closed(st->ws));
    CFW_TEST_ASSERT_EQ(st->messages, 0);
    return 0;
}

static int test_ws_continuation_without_start(void)
{
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t client;
    char response[8192];
    uint8_t frame[64];
    size_t flen;

    cfw_test_ws_setup(st);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_connect(&client,
                                           cfw_server_port(&st->server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_handshake(&st->server, &client,
                                             response,
                                             sizeof(response)),
                       0);

    /* Continuation frame with no fragmented message in progress. */
    flen = cfw_test_ws_masked_frame(frame, 1, 0x0, "x", 1);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_expect_close(st, &client, response,
                                                sizeof(response), frame,
                                                flen, 1002),
                       0);

    cfw_fd_close(&client);
    cfw_test_ws_teardown(st);
    return 0;
}

static int test_ws_data_opcode_mid_fragment(void)
{
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t client;
    char response[8192];
    uint8_t frame[64];
    size_t flen;

    cfw_test_ws_setup(st);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_connect(&client,
                                           cfw_server_port(&st->server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_handshake(&st->server, &client,
                                             response,
                                             sizeof(response)),
                       0);

    /* Start a fragmented text message, then a new binary data frame
     * before the final continuation: protocol error. */
    flen = cfw_test_ws_masked_frame(frame, 0, 0x1, "a", 1);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_send(&client, frame, flen), 0);
    flen = cfw_test_ws_masked_frame(frame, 1, 0x2, "b", 1);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_expect_close(st, &client, response,
                                                sizeof(response), frame,
                                                flen, 1002),
                       0);

    cfw_fd_close(&client);
    cfw_test_ws_teardown(st);
    return 0;
}

static int test_ws_control_frame_too_big(void)
{
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t client;
    char response[8192];
    uint8_t frame[256];
    uint8_t payload[126];
    size_t flen;

    cfw_test_ws_setup(st);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_connect(&client,
                                           cfw_server_port(&st->server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_handshake(&st->server, &client,
                                             response,
                                             sizeof(response)),
                       0);

    /* Ping with 126-byte payload exceeds the 125 control limit. */
    memset(payload, 'p', sizeof(payload));
    flen = cfw_test_ws_masked_frame(frame, 1, 0x9, payload,
                                    sizeof(payload));
    CFW_TEST_ASSERT_EQ(cfw_test_ws_expect_close(st, &client, response,
                                                sizeof(response), frame,
                                                flen, 1002),
                       0);

    cfw_fd_close(&client);
    cfw_test_ws_teardown(st);
    return 0;
}

static int test_ws_rsv_bits_rejected(void)
{
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t client;
    char response[8192];
    uint8_t frame[64];
    size_t flen;

    cfw_test_ws_setup(st);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_connect(&client,
                                           cfw_server_port(&st->server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_handshake(&st->server, &client,
                                             response,
                                             sizeof(response)),
                       0);

    /* RSV1 set on a data frame: no extensions are negotiated. */
    flen = cfw_test_ws_masked_frame(frame, 1, 0x1, "hi", 2);
    frame[0] |= 0x40;
    CFW_TEST_ASSERT_EQ(cfw_test_ws_expect_close(st, &client, response,
                                                sizeof(response), frame,
                                                flen, 1002),
                       0);

    cfw_fd_close(&client);
    cfw_test_ws_teardown(st);
    return 0;
}

static int test_ws_binary_echo(void)
{
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t client;
    char response[8192];
    uint8_t frame[64];
    static const uint8_t blob[] = {0x00, 0xff, 0xfe, 0x41};
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

    /* Binary payload is not UTF-8 validated and echoes verbatim. */
    flen = cfw_test_ws_masked_frame(frame, 1, 0x2, blob, sizeof(blob));
    CFW_TEST_ASSERT_EQ(cfw_test_ws_send(&client, frame, flen), 0);

    /* The marker must not contain NUL (strlen-bounded); "A" is the
     * payload's last byte and cannot appear in the 2-byte header. */
    CFW_TEST_ASSERT_TRUE(cfw_test_ws_recv_until(
                             &st->server, &client, response,
                             sizeof(response), "A", &used) > 0);
    CFW_TEST_ASSERT_TRUE(cfw_test_ws_parse_frame(
                             (const uint8_t *)response, used, &opcode,
                             &payload, &payload_len));
    CFW_TEST_ASSERT_EQ(opcode, 0x2);
    CFW_TEST_ASSERT_EQ(payload_len, sizeof(blob));
    CFW_TEST_ASSERT_EQ(memcmp(payload, blob, sizeof(blob)), 0);
    CFW_TEST_ASSERT_EQ(st->last_opcode, CFW_WS_OPCODE_BINARY);

    cfw_fd_close(&client);
    cfw_test_ws_teardown(st);
    return 0;
}

static int test_ws_send_wouldblock(void)
{
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t client;
    char response[8192];
    static uint8_t big[100 * 1024];

    cfw_test_ws_setup(st);
    cfw_fd_init(&client);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_connect(&client,
                                           cfw_server_port(&st->server)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_handshake(&st->server, &client,
                                             response,
                                             sizeof(response)),
                       0);
    CFW_TEST_ASSERT_TRUE(st->ws != NULL);

    /* 100 KiB exceeds max_out_size (64 KiB): the whole send must fail
     * atomically rather than queue a partial message. */
    memset(big, 0x5a, sizeof(big));
    CFW_TEST_ASSERT_EQ(cfw_ws_send_binary(st->ws, big, sizeof(big)),
                       CFW_ERROR_WOULDBLOCK);
    CFW_TEST_ASSERT_FALSE(cfw_ws_is_closed(st->ws));

    cfw_fd_close(&client);
    cfw_test_ws_teardown(st);
    return 0;
}

static int test_ws_close_with_reason(void)
{
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t client;
    char response[8192];
    uint8_t frame[64];
    uint8_t payload[8];
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

    /* Close: status 1000 + reason "bye" → echo carries all 5 bytes. */
    payload[0] = 0x03;
    payload[1] = 0xe8;
    memcpy(payload + 2, "bye", 3);
    flen = cfw_test_ws_masked_frame(frame, 1, 0x8, payload, 5);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_send(&client, frame, flen), 0);

    /* The echoed close frame is exactly: 0x88 0x05 0x03 0xE8 "bye". */
    {
        uint8_t exact[7] = {0x88, 0x05, 0x03, 0xe8, 'b', 'y', 'e'};
        char n7[8];
        memcpy(n7, exact, 7);
        n7[7] = '\0';
        CFW_TEST_ASSERT_TRUE(cfw_test_ws_recv_until(
                                 &st->server, &client, response,
                                 sizeof(response), n7, &used) > 0);
    }
    CFW_TEST_ASSERT_TRUE(cfw_ws_is_closed(st->ws));

    cfw_fd_close(&client);
    cfw_test_ws_teardown(st);
    return 0;
}

static int test_ws_ping_interleaved_fragment(void)
{
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t client;
    char response[8192];
    uint8_t frame[64];
    size_t flen;
    size_t used = 0;
    size_t pos = 0;
    int opcode;
    const uint8_t *payload;
    size_t payload_len;
    size_t flen2;

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

    /* Fragmented "Hello": text(fin=0) "Hel", ping "x" interleaved,
     * continuation(fin=1) "lo". Expect pong then echoed message. */
    flen = cfw_test_ws_masked_frame(frame, 0, 0x1, "Hel", 3);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_send(&client, frame, flen), 0);
    flen = cfw_test_ws_masked_frame(frame, 1, 0x9, "x", 1);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_send(&client, frame, flen), 0);
    flen = cfw_test_ws_masked_frame(frame, 1, 0x0, "lo", 2);
    CFW_TEST_ASSERT_EQ(cfw_test_ws_send(&client, frame, flen), 0);

    CFW_TEST_ASSERT_TRUE(cfw_test_ws_recv_until(
                             &st->server, &client, response,
                             sizeof(response), "Hello", &used) > 0);
    /* First frame must be the pong (opcode 0xA, payload "x"). */
    CFW_TEST_ASSERT_TRUE(cfw_test_ws_parse_frame(
                             (const uint8_t *)response + pos, used - pos,
                             &opcode, &payload, &payload_len));
    CFW_TEST_ASSERT_EQ(opcode, 0xA);
    CFW_TEST_ASSERT_EQ(payload_len, 1);
    CFW_TEST_ASSERT_EQ(payload[0], 'x');
    flen2 = cfw_test_ws_frame_len((const uint8_t *)response + pos,
                                  used - pos);
    CFW_TEST_ASSERT_TRUE(flen2 != 0);
    pos += flen2;
    /* Second frame: echoed text "Hello". */
    CFW_TEST_ASSERT_TRUE(cfw_test_ws_parse_frame(
                             (const uint8_t *)response + pos, used - pos,
                             &opcode, &payload, &payload_len));
    CFW_TEST_ASSERT_EQ(opcode, 0x1);
    CFW_TEST_ASSERT_EQ(payload_len, 5);
    CFW_TEST_ASSERT_EQ(memcmp(payload, "Hello", 5), 0);
    CFW_TEST_ASSERT_EQ(st->messages, 1);
    CFW_TEST_ASSERT_EQ(st->last_payload_len, 5);

    cfw_fd_close(&client);
    cfw_test_ws_teardown(st);
    return 0;
}

static int test_ws_soak(void)
{
    enum { NCONN = 6, ROUNDS = 25 };
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t clients[NCONN];
    char response[8192];
    uint8_t frame[128];
    size_t flen;
    int c;
    int r;

    cfw_test_ws_setup(st);
    st->echo = 1;
    st->messages = 0;

    /* Handshake each connection sequentially (deterministic connect
     * completion), then interleave message rounds across all of
     * them. */
    for (c = 0; c < NCONN; c++) {
        cfw_fd_init(&clients[c]);
        CFW_TEST_ASSERT_EQ(cfw_test_ws_connect(
                               &clients[c],
                               cfw_server_port(&st->server)),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_test_ws_handshake(
                               &st->server, &clients[c], response,
                               sizeof(response)),
                           0);
    }
    CFW_TEST_ASSERT_EQ(st->ws_count, (size_t)NCONN);

    flen = cfw_test_ws_masked_frame(frame, 1, 0x1, "soak-msg", 8);
    for (r = 0; r < ROUNDS; r++) {
        for (c = 0; c < NCONN; c++) {
            CFW_TEST_ASSERT_EQ(cfw_test_ws_send(&clients[c], frame,
                                                flen),
                               0);
        }
        for (c = 0; c < NCONN; c++) {
            size_t used = 0;
            CFW_TEST_ASSERT_TRUE(cfw_test_ws_recv_until(
                                     &st->server, &clients[c], response,
                                     sizeof(response), "soak-msg",
                                     &used) > 0);
        }
    }
    CFW_TEST_ASSERT_EQ(st->messages, NCONN * ROUNDS);

    /* Orderly close on every connection; EV_CLOSE must reap each ws
     * object exactly once. */
    for (c = 0; c < NCONN; c++) {
        cfw_fd_close(&clients[c]);
    }
    {
        int attempts;
        for (attempts = 0; attempts < 200; attempts++) {
            cfw_timepoint_t deadline;
            cfw_duration_t slice;
            cfw_duration_from_millis(20, &slice);
            cfw_deadline_from_now(&deadline, slice);
            CFW_TEST_ASSERT_EQ(cfw_server_step(&st->server, &deadline),
                               CFW_OK);
            if (st->ws_count == 0) {
                break;
            }
        }
        CFW_TEST_ASSERT_EQ(st->ws_count, 0);
        CFW_TEST_ASSERT_EQ(cfw_server_connection_count(&st->server), 0);
    }

    cfw_test_ws_teardown(st);
    return 0;
}

static int test_ws_large_frame_across_recvs(void)
{
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t client;
    char response[256 * 1024];
    static uint8_t payload[40 * 1024];
    uint8_t frame[64];
    size_t flen;
    size_t used = 0;
    size_t i;

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

    /* 40 KiB single-frame message: far larger than the server's 16 KiB
     * recv buffer, so payload arrives across many recv calls and must
     * be reassembled incrementally. */
    for (i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 31u + 7u);
    }
    /* Build the frame manually: header (masked, 16-bit length) +
     * masked payload streamed in chunks. */
    {
        static const uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
        size_t off = 0;
        frame[0] = 0x82; /* FIN + binary */
        frame[1] = (uint8_t)(0x80 | 126);
        frame[2] = (uint8_t)(sizeof(payload) >> 8);
        frame[3] = (uint8_t)sizeof(payload);
        memcpy(frame + 4, mask, 4);
        flen = 8;
        CFW_TEST_ASSERT_EQ(cfw_test_ws_send(&client, frame, flen), 0);
        /* Payload in 8 KiB masked chunks. */
        while (off < sizeof(payload)) {
            uint8_t chunk[8192];
            size_t n = sizeof(payload) - off;
            size_t k;
            if (n > sizeof(chunk)) {
                n = sizeof(chunk);
            }
            for (k = 0; k < n; k++) {
                chunk[k] = (uint8_t)(payload[off + k] ^
                                     mask[(off + k) & 3]);
            }
            CFW_TEST_ASSERT_EQ(cfw_test_ws_send(&client, chunk, n), 0);
            off += n;
        }
    }

    /* Drain the echo: the server fragments it at max_frame_payload
     * (16 KiB) into multiple frames, so just accumulate until the
     * expected wire size arrives — this proves the large send flushed
     * without error. The reassembled inbound message is verified via
     * st->last_payload below. */
    {
        /* 3 frames (16384 + 16384 + 8192 payload): each exceeds 125
         * bytes so each header is 4 bytes (126 + 16-bit length). */
        size_t want = 40960 + 12;
        cfw_duration_t slice;
        int attempts;
        cfw_duration_from_millis(50, &slice);
        for (attempts = 0;
             attempts < 4000 && used + 1 < sizeof(response);
             attempts++) {
            cfw_timepoint_t deadline;
            cfw_deadline_from_now(&deadline, slice);
            ssize_t n;
            if (cfw_server_step(&st->server, &deadline) != CFW_OK) {
                break;
            }
            n = recv((int)cfw_fd_get(&client), response + used,
                     sizeof(response) - used - 1, MSG_DONTWAIT);
            if (n > 0) {
                used += (size_t)n;
                if (used >= want) {
                    break;
                }
            } else if (n == 0) {
                break;
            }
        }
        CFW_TEST_ASSERT_TRUE(used >= want);
    }
    CFW_TEST_ASSERT_EQ(st->messages, 1);
    CFW_TEST_ASSERT_EQ(st->last_payload_len, sizeof(payload));
    {
        uint64_t h = 1469598103934665603ull;
        for (i = 0; i < sizeof(payload); i++) {
            h = (h ^ payload[i]) * 1099511628211ull;
        }
        CFW_TEST_ASSERT_EQ(st->last_payload_hash, h);
    }

    cfw_fd_close(&client);
    cfw_test_ws_teardown(st);
    return 0;
}

static int test_ws_drip_feed(void)
{
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t client;
    char response[8192];
    uint8_t frame[64];
    size_t flen;
    size_t i;
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

    /* Deliver a masked text frame one byte at a time, stepping the
     * server between sends: exercises partial header and partial
     * payload state across recv calls. */
    flen = cfw_test_ws_masked_frame(frame, 1, 0x1, "drip", 4);
    for (i = 0; i < flen; i++) {
        CFW_TEST_ASSERT_EQ(cfw_test_ws_send(&client, frame + i, 1), 0);
        {
            cfw_timepoint_t deadline;
            cfw_duration_t slice;
            cfw_duration_from_millis(5, &slice);
            cfw_deadline_from_now(&deadline, slice);
            CFW_TEST_ASSERT_EQ(cfw_server_step(&st->server, &deadline),
                               CFW_OK);
        }
    }

    CFW_TEST_ASSERT_TRUE(cfw_test_ws_recv_until(
                             &st->server, &client, response,
                             sizeof(response), "drip", &used) > 0);
    CFW_TEST_ASSERT_EQ(st->messages, 1);
    CFW_TEST_ASSERT_EQ(st->last_payload_len, 4);
    CFW_TEST_ASSERT_EQ(memcmp(st->last_payload, "drip", 4), 0);

    cfw_fd_close(&client);
    cfw_test_ws_teardown(st);
    return 0;
}

/* Deterministic mutation fuzz over the WS frame parser (server
 * side): valid masked frames and random blobs are mutated and fed
 * through a real upgraded connection. Any crash, hang, or sanitizer
 * finding fails the test; protocol-level rejection is expected and
 * fine — the conn may close at any point, in which case the client
 * reconnects and re-handshakes. */
static uint64_t cfw_test_ws_rng_state = 0xD1B54A32D192ED03ull;

static uint64_t cfw_test_ws_rng(void)
{
    uint64_t x = cfw_test_ws_rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    cfw_test_ws_rng_state = x;
    return x;
}

static int cfw_test_ws_fuzz_send(cfw_server_t *server, cfw_fd_t *client,
                                 const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;
    int guard = 0;

    while (sent < len && guard++ < 4096) {
        ssize_t n = send((int)cfw_fd_get(client), p + sent, len - sent,
                         MSG_DONTWAIT);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            cfw_timepoint_t deadline;
            cfw_duration_t slice;
            cfw_duration_from_millis(5, &slice);
            cfw_deadline_from_now(&deadline, slice);
            if (cfw_server_step(server, &deadline) != CFW_OK) {
                return 1;
            }
            continue;
        }
        return 1; /* EPIPE etc.: the conn is gone */
    }
    return sent != len;
}

static int cfw_test_ws_fuzz_step(cfw_server_t *server)
{
    cfw_timepoint_t deadline;
    cfw_duration_t slice;
    cfw_duration_from_millis(5, &slice);
    cfw_deadline_from_now(&deadline, slice);
    return cfw_server_step(server, &deadline) != CFW_OK;
}

static int test_ws_fuzz_frames(void)
{
    enum { ITERS = 400 };
    cfw_test_ws_state *st = &cfw_test_ws_st;
    cfw_fd_t client;
    char response[8192];
    uint8_t buf[512];
    size_t i;
    int connected = 0;

    cfw_test_ws_setup(st);
    st->echo = 0;
    cfw_fd_init(&client);

    for (i = 0; i < ITERS; i++) {
        size_t len;
        size_t mutations;
        size_t k;

        if (!connected) {
            CFW_TEST_ASSERT_EQ(
                cfw_test_ws_connect(&client,
                                    cfw_server_port(&st->server)),
                CFW_OK);
            CFW_TEST_ASSERT_EQ(cfw_test_ws_handshake(
                                   &st->server, &client, response,
                                   sizeof(response)),
                               0);
            connected = 1;
        }

        /* Half the iterations start from a valid masked frame, half
         * from raw bytes; both are then mutated. */
        if ((cfw_test_ws_rng() & 1u) != 0) {
            static const struct cfw_test_ws_fuzz_seed {
                int fin;
                int op;
                const char *payload;
            } seeds[] = {
                {1, 0x1, "hello"},
                {1, 0x2, "\x00\x01\x02\x03"},
                {1, 0x8, ""},
                {0, 0x1, "frag"},
                {1, 0x9, "ping"},
                {1, 0xA, "pong"},
            };
            const struct cfw_test_ws_fuzz_seed *seed =
                &seeds[cfw_test_ws_rng() %
                      (sizeof(seeds) / sizeof(seeds[0]))];
            len = cfw_test_ws_masked_frame(
                buf, seed->fin, seed->op, seed->payload,
                strlen(seed->payload));
        } else {
            len = 1 + (size_t)(cfw_test_ws_rng() % 200);
            for (k = 0; k < len; k++) {
                buf[k] = (uint8_t)cfw_test_ws_rng();
            }
        }
        mutations = 1 + (size_t)(cfw_test_ws_rng() % 4);
        for (k = 0; k < mutations && len != 0; k++) {
            size_t pos = (size_t)(cfw_test_ws_rng() % len);
            buf[pos] = (uint8_t)cfw_test_ws_rng();
        }

        if (cfw_test_ws_fuzz_send(&st->server, &client, buf, len) != 0) {
            connected = 0;
            continue;
        }
        /* Drain: a few steps let the parser consume whatever arrived. */
        for (k = 0; k < 3; k++) {
            if (cfw_test_ws_fuzz_step(&st->server) != 0) {
                break;
            }
        }
        if (cfw_server_connection_count(&st->server) == 0) {
            connected = 0;
        }
    }

    if (connected) {
        cfw_fd_close(&client);
    }
    cfw_test_ws_teardown(st);
    return 0;
}
#endif /* POSIX */

const cfw_test_case cfw_test_ws_cases[] = {
    {"ws_accept_key_rfc_vector", test_ws_accept_key_rfc_vector},
    {"ws_accept_key_args", test_ws_accept_key_args},
    {"ws_handshake_validate_ok", test_ws_handshake_validate_ok},
    {"ws_handshake_validate_rejects", test_ws_handshake_validate_rejects},
    {"ws_handshake_accept_extract", test_ws_handshake_accept_extract},
#if CFW_PLATFORM_POSIX
    {"ws_text_echo", test_ws_text_echo},
    {"ws_fragmented_message", test_ws_fragmented_message},
    {"ws_ping_auto_pong", test_ws_ping_auto_pong},
    {"ws_close_echo", test_ws_close_echo},
    {"ws_unmasked_rejected", test_ws_unmasked_rejected},
    {"ws_invalid_utf8_rejected", test_ws_invalid_utf8_rejected},
    {"ws_message_too_big", test_ws_message_too_big},
    {"ws_send_close_from_app", test_ws_send_close_from_app},
    {"ws_continuation_without_start",
     test_ws_continuation_without_start},
    {"ws_data_opcode_mid_fragment", test_ws_data_opcode_mid_fragment},
    {"ws_control_frame_too_big", test_ws_control_frame_too_big},
    {"ws_rsv_bits_rejected", test_ws_rsv_bits_rejected},
    {"ws_binary_echo", test_ws_binary_echo},
    {"ws_send_wouldblock", test_ws_send_wouldblock},
    {"ws_close_with_reason", test_ws_close_with_reason},
    {"ws_ping_interleaved_fragment",
     test_ws_ping_interleaved_fragment},
    {"ws_soak", test_ws_soak},
    {"ws_large_frame_across_recvs",
     test_ws_large_frame_across_recvs},
    {"ws_drip_feed", test_ws_drip_feed},
    {"ws_fuzz_frames", test_ws_fuzz_frames},
#endif
    {NULL, NULL},
};
