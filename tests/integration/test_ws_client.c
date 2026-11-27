/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/* WebSocket client integration tests (ADR 0071): a real cfw_server
 * running the RFC 6455 echo path (cfw/ws.h + upgrade hook) on a
 * worker thread, with cfw_ws_client driving the client side over
 * loopback — cleartext and TLS. A raw-socket fake server covers the
 * masked-server-frame rejection the real server never emits.
 * POSIX only; the client is unavailable on Windows. */

#include <cfw/cfw.h>

#include "test_harness.h"

#if CFW_PLATFORM_POSIX
    #include <cfw/net.h>

    #include <netinet/in.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <time.h>
    #include <unistd.h>

/* ------------------------------------------------------------------ */
/* cfw_server ws-echo fixture                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_server_t server;
    cfw_thread_t thread;
    int thread_started;
    cfw_tls_ctx_t *tls_ctx; /* borrowed; owned by caller */
} cfw_wsc_fixture_t;

static cfw_status_t cfw_wsc_on_message(cfw_ws_t *ws,
                                       const cfw_ws_message_t *msg,
                                       void *user)
{
    (void)user;
    return cfw_ws_send(ws, msg->opcode, msg->payload.data,
                       msg->payload.size);
}

static cfw_status_t cfw_wsc_handler(cfw_request_t *req,
                                    cfw_response_t *res, void *user)
{
    static char accept[CFW_WS_ACCEPT_KEY_CAPACITY];
    cfw_strview_t name;
    cfw_strview_t value;
    (void)user;

    if (cfw_ws_handshake_validate(req) != CFW_OK ||
        cfw_ws_handshake_accept(req, accept, sizeof(accept)) != CFW_OK) {
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
    cfw_strview_init(&value, accept, strlen(accept));
    cfw_response_header_add(res, &name, &value);
    return CFW_OK;
}

static cfw_status_t cfw_wsc_bad_handler(cfw_request_t *req,
                                        cfw_response_t *res, void *user)
{
    (void)req;
    (void)user;
    res->status = CFW_HTTP_STATUS_BAD_REQUEST;
    return CFW_OK;
}

static cfw_status_t cfw_wsc_upgrade(cfw_upgraded_conn_t *conn,
                                    uint32_t events, void *user)
{
    cfw_ws_t *ws;
    (void)user;

    if (events == CFW_UPGRADE_EV_CLOSE) {
        ws = cfw_upgraded_user(conn);
        if (ws != NULL) {
            cfw_ws_destroy(ws);
        }
        return CFW_OK;
    }
    if (events == CFW_UPGRADE_EV_READY) {
        cfw_ws_options_t options;
        memset(&options, 0, sizeof(options));
        options.on_message = cfw_wsc_on_message;
        ws = cfw_ws_create(conn, &options);
        if (ws == NULL) {
            return CFW_ERROR_INTERNAL;
        }
        cfw_upgraded_set_user(conn, ws);
        return CFW_OK;
    }
    ws = cfw_upgraded_user(conn);
    if (ws != NULL) {
        (void)cfw_ws_on_event(ws, events);
    }
    return CFW_OK;
}

static int cfw_wsc_server_thread_main(void *arg)
{
    cfw_wsc_fixture_t *fixture = (cfw_wsc_fixture_t *)arg;
    return (int)cfw_server_run(&fixture->server);
}

static int cfw_wsc_server_start(cfw_wsc_fixture_t *fixture,
                                cfw_tls_ctx_t *tls_ctx)
{
    cfw_server_options_t options;
    cfw_dispatch_options_t doptions;
    cfw_strview_t pattern;

    memset(fixture, 0, sizeof(*fixture));
    fixture->tls_ctx = tls_ctx;
    fixture->arena = cfw_arena_create(65536);
    CFW_TEST_ASSERT_TRUE(fixture->arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&fixture->router, fixture->arena),
                       CFW_OK);

    memset(&doptions, 0, sizeof(doptions));
    doptions.router = &fixture->router;
    doptions.handler = cfw_wsc_handler;
    doptions.max_middleware = 4;
    CFW_TEST_ASSERT_EQ(cfw_dispatch_init(&fixture->dispatch,
                                         fixture->arena, &doptions),
                       CFW_OK);

    cfw_strview_from_cstr(&pattern, "/ws");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(
                           &fixture->dispatch, CFW_HTTP_METHOD_GET,
                           &pattern, cfw_wsc_handler, NULL),
                       CFW_OK);
    cfw_strview_from_cstr(&pattern, "/bad");
    CFW_TEST_ASSERT_EQ(cfw_dispatch_add_route(
                           &fixture->dispatch, CFW_HTTP_METHOD_GET,
                           &pattern, cfw_wsc_bad_handler, NULL),
                       CFW_OK);

    memset(&options, 0, sizeof(options));
    options.host = "127.0.0.1";
    options.port = 0;
    options.dispatch = &fixture->dispatch;
    options.http.max_head_size = 8192;
    options.http.max_body_size = 65536;
    options.http.max_pending_size = 65536;
    options.max_out_size = 1u << 20;
    options.max_connections = 8;
    options.max_headers = 32;
    options.max_params = 8;
    options.upgrade = cfw_wsc_upgrade;
    options.tls_ctx = tls_ctx;
    CFW_TEST_ASSERT_EQ(cfw_server_init(&fixture->server, &options),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_server_port(&fixture->server) != 0);

    CFW_TEST_ASSERT_EQ(cfw_thread_create(
                           &fixture->thread, cfw_wsc_server_thread_main,
                           fixture),
                       CFW_OK);
    fixture->thread_started = 1;
    return 0;
}

static void cfw_wsc_server_stop(cfw_wsc_fixture_t *fixture)
{
    if (fixture->thread_started) {
        cfw_server_stop(&fixture->server);
        {
            int exit_code = 0;
            cfw_thread_join(&fixture->thread, &exit_code);
        }
    }
    cfw_server_free(&fixture->server);
    cfw_arena_destroy(fixture->arena);
}

/* ------------------------------------------------------------------ */
/* Client-side message capture                                         */
/* ------------------------------------------------------------------ */

static unsigned char cfw_wsc_got_payload[70000];
static size_t cfw_wsc_got_size;
static cfw_ws_opcode_t cfw_wsc_got_opcode;
static int cfw_wsc_got_count;

static cfw_status_t cfw_wsc_capture(cfw_ws_client_t *ws,
                                    const cfw_ws_message_t *msg,
                                    void *user)
{
    (void)ws;
    (void)user;
    if (msg->payload.size <= sizeof(cfw_wsc_got_payload)) {
        memcpy(cfw_wsc_got_payload, msg->payload.data,
               msg->payload.size);
        cfw_wsc_got_size = msg->payload.size;
        cfw_wsc_got_opcode = msg->opcode;
        cfw_wsc_got_count++;
    }
    return CFW_OK;
}

static void cfw_wsc_opts_init(cfw_ws_client_options_t *opts,
                              const char *host, uint16_t port,
                              const char *path)
{
    memset(opts, 0, sizeof(*opts));
    opts->host = host;
    opts->port = port;
    opts->path = path;
    opts->on_message = cfw_wsc_capture;
    {
        cfw_duration_t d;
        cfw_duration_from_millis(5000, &d);
        opts->timeout = d;
    }
}

/* Pumps until a message arrives or the attempt budget ends.
 * Returns 1 when cfw_wsc_got_count advanced. */
static int cfw_wsc_pump_for_message(cfw_ws_client_t *ws, int *prev_count)
{
    cfw_duration_t slice;
    int i;
    cfw_duration_from_millis(50, &slice);
    for (i = 0; i < 200; i++) {
        cfw_status_t st = cfw_ws_client_pump(ws, &slice);
        if (st != CFW_OK && st != CFW_ERROR_TIMEOUT) {
            return -1;
        }
        if (cfw_ws_client_is_closed(ws)) {
            return 0;
        }
        if (cfw_wsc_got_count != *prev_count) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static int test_ws_client_echo(void)
{
    cfw_wsc_fixture_t fixture;
    cfw_ws_client_options_t opts;
    cfw_ws_client_t *ws = NULL;
    uint16_t port;
    int prev;
    int got;

    CFW_TEST_ASSERT_EQ(cfw_wsc_server_start(&fixture, NULL), 0);
    port = cfw_server_port(&fixture.server);

    cfw_wsc_opts_init(&opts, "127.0.0.1", port, "/ws");
    CFW_TEST_ASSERT_EQ(cfw_ws_client_connect(&opts, &ws), CFW_OK);
    CFW_TEST_ASSERT_TRUE(ws != NULL);
    CFW_TEST_ASSERT_TRUE(!cfw_ws_client_is_closed(ws));

    /* Malformed close payloads are rejected before touching the
     * wire: odd length, reserved status, non-UTF-8 reason. */
    {
        uint8_t bad_status[] = {0x03, 0xe8}; /* 1000 is fine; use 999 */
        uint8_t bad_len[] = {0x00};
        uint8_t bad_reason[] = {0x03, 0xe8, 0xff};
        bad_status[0] = 0x03;
        bad_status[1] = 0xe7; /* 999: not a registered code */
        CFW_TEST_ASSERT_EQ(cfw_ws_client_send(ws, CFW_WS_OPCODE_CLOSE,
                                              bad_len, 1),
                           CFW_ERROR_INVALID_ARGUMENT);
        CFW_TEST_ASSERT_EQ(cfw_ws_client_send(ws, CFW_WS_OPCODE_CLOSE,
                                              bad_status, 2),
                           CFW_ERROR_INVALID_ARGUMENT);
        CFW_TEST_ASSERT_EQ(cfw_ws_client_send(ws, CFW_WS_OPCODE_CLOSE,
                                              bad_reason, 3),
                           CFW_ERROR_INVALID_ARGUMENT);
    }

    prev = cfw_wsc_got_count;
    CFW_TEST_ASSERT_EQ(cfw_ws_client_send_text(
                           ws, "hello-ws-client",
                           sizeof("hello-ws-client") - 1),
                       CFW_OK);
    got = cfw_wsc_pump_for_message(ws, &prev);
    CFW_TEST_ASSERT_EQ(got, 1);
    CFW_TEST_ASSERT_EQ(cfw_wsc_got_opcode, CFW_WS_OPCODE_TEXT);
    CFW_TEST_ASSERT_EQ(cfw_wsc_got_size,
                       sizeof("hello-ws-client") - 1);
    CFW_TEST_ASSERT_EQ(memcmp(cfw_wsc_got_payload, "hello-ws-client",
                              cfw_wsc_got_size),
                       0);

    /* Graceful close: client close -> server echo -> client closed.
     * Once close is sent no further data frames may be queued. */
    CFW_TEST_ASSERT_EQ(cfw_ws_client_send_close(ws, 1000, "bye", 3),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_ws_client_send_text(ws, "late", 4),
                       CFW_ERROR_INVALID_ARGUMENT);
    {
        cfw_duration_t slice;
        int i;
        int closed = 0;
        cfw_duration_from_millis(50, &slice);
        for (i = 0; i < 200 && !closed; i++) {
            (void)cfw_ws_client_pump(ws, &slice);
            closed = cfw_ws_client_is_closed(ws);
        }
        CFW_TEST_ASSERT_TRUE(closed);
    }

    cfw_ws_client_destroy(ws);
    cfw_wsc_server_stop(&fixture);
    return 0;
}

static int test_ws_client_large_message(void)
{
    /* 40000 bytes > both sides' 16 KiB frame payload: exercises
     * client-side fragmentation on send and reassembly on receive,
     * plus the server's fragmented echo back. */
    cfw_wsc_fixture_t fixture;
    cfw_ws_client_options_t opts;
    cfw_ws_client_t *ws = NULL;
    static unsigned char payload[40000];
    uint16_t port;
    int prev;
    int got;
    size_t i;

    for (i = 0; i < sizeof(payload); i++) {
        payload[i] = (unsigned char)(i * 31u + 7u);
    }

    CFW_TEST_ASSERT_EQ(cfw_wsc_server_start(&fixture, NULL), 0);
    port = cfw_server_port(&fixture.server);

    cfw_wsc_opts_init(&opts, "127.0.0.1", port, "/ws");
    CFW_TEST_ASSERT_EQ(cfw_ws_client_connect(&opts, &ws), CFW_OK);

    prev = cfw_wsc_got_count;
    CFW_TEST_ASSERT_EQ(cfw_ws_client_send_binary(
                           ws, payload, sizeof(payload)),
                       CFW_OK);
    got = cfw_wsc_pump_for_message(ws, &prev);
    CFW_TEST_ASSERT_EQ(got, 1);
    CFW_TEST_ASSERT_EQ(cfw_wsc_got_opcode, CFW_WS_OPCODE_BINARY);
    CFW_TEST_ASSERT_EQ(cfw_wsc_got_size, sizeof(payload));
    CFW_TEST_ASSERT_EQ(memcmp(cfw_wsc_got_payload, payload,
                              sizeof(payload)),
                       0);

    cfw_ws_client_destroy(ws);
    cfw_wsc_server_stop(&fixture);
    return 0;
}

static int test_ws_client_handshake_rejected(void)
{
    /* /bad returns 400: the Upgrade exchange fails and connect must
     * surface an IO-class error, not a ws connection. */
    cfw_wsc_fixture_t fixture;
    cfw_ws_client_options_t opts;
    cfw_ws_client_t *ws = NULL;
    uint16_t port;

    CFW_TEST_ASSERT_EQ(cfw_wsc_server_start(&fixture, NULL), 0);
    port = cfw_server_port(&fixture.server);

    cfw_wsc_opts_init(&opts, "127.0.0.1", port, "/bad");
    {
        cfw_status_t st = cfw_ws_client_connect(&opts, &ws);
        CFW_TEST_ASSERT_TRUE(st == CFW_ERROR_IO ||
                             st == CFW_ERROR_INVALID_ARGUMENT ||
                             st == CFW_ERROR_TIMEOUT);
        CFW_TEST_ASSERT_TRUE(ws == NULL);
    }

    cfw_wsc_server_stop(&fixture);
    return 0;
}

static int test_ws_client_bad_options(void)
{
    cfw_ws_client_options_t opts;
    cfw_ws_client_t *ws = NULL;
    cfw_http_header_t hdr;
    cfw_strview_t hn;
    cfw_strview_t hv;

    cfw_wsc_opts_init(&opts, "127.0.0.1", 80, "/ws");
    opts.host = NULL;
    CFW_TEST_ASSERT_EQ(cfw_ws_client_connect(&opts, &ws),
                       CFW_ERROR_INVALID_ARGUMENT);
    opts.host = "127.0.0.1";
    opts.port = 0;
    CFW_TEST_ASSERT_EQ(cfw_ws_client_connect(&opts, &ws),
                       CFW_ERROR_INVALID_ARGUMENT);
    opts.port = 80;
    opts.path = "ws";
    CFW_TEST_ASSERT_EQ(cfw_ws_client_connect(&opts, &ws),
                       CFW_ERROR_INVALID_ARGUMENT);
    opts.path = "/ws";

    /* Framework-managed header names are rejected. */
    cfw_strview_from_cstr(&hn, "Sec-WebSocket-Protocol");
    cfw_strview_from_cstr(&hv, "chat");
    memset(&hdr, 0, sizeof(hdr));
    hdr.name = hn;
    hdr.value = hv;
    opts.headers = &hdr;
    opts.header_count = 1;
    CFW_TEST_ASSERT_EQ(cfw_ws_client_connect(&opts, &ws),
                       CFW_ERROR_INVALID_ARGUMENT);
    opts.header_count = 0;

    /* Request targets with SP/CTL bytes are rejected (they would
     * inject into the request line). */
    opts.path = "/ws room";
    CFW_TEST_ASSERT_EQ(cfw_ws_client_connect(&opts, &ws),
                       CFW_ERROR_INVALID_ARGUMENT);
    opts.path = "/ws\r\nX-Injected: yes";
    CFW_TEST_ASSERT_EQ(cfw_ws_client_connect(&opts, &ws),
                       CFW_ERROR_INVALID_ARGUMENT);
    opts.path = "/ws?room=1&x=%20";

    /* Caller headers must satisfy the HTTP field grammar: token
     * names and CTL-free values. */
    cfw_strview_from_cstr(&hn, "Bad Header");
    cfw_strview_from_cstr(&hv, "v");
    hdr.name = hn;
    hdr.value = hv;
    opts.headers = &hdr;
    opts.header_count = 1;
    CFW_TEST_ASSERT_EQ(cfw_ws_client_connect(&opts, &ws),
                       CFW_ERROR_INVALID_ARGUMENT);
    cfw_strview_from_cstr(&hn, "X-Ok");
    hdr.name = hn;
    cfw_strview_from_cstr(&hv, "a\r\nb: c");
    hdr.value = hv;
    CFW_TEST_ASSERT_EQ(cfw_ws_client_connect(&opts, &ws),
                       CFW_ERROR_INVALID_ARGUMENT);
    opts.headers = NULL;
    opts.header_count = 0;
    return 0;
}

#if CFW_HAVE_TLS

static int test_ws_client_tls_echo(void)
{
    /* wss:// end-to-end: TLS ctx on both ends, ws echo over TLS. */
    cfw_wsc_fixture_t fixture;
    cfw_ws_client_options_t opts;
    cfw_ws_client_t *ws = NULL;
    cfw_tls_ctx_t *server_ctx = NULL;
    cfw_tls_ctx_t *client_ctx = NULL;
    cfw_tls_ctx_options_t topts;
    char path[512];
    uint16_t port;
    int prev;
    int got;
    cfw_status_t status;

    memset(&topts, 0, sizeof(topts));
    topts.role = CFW_TLS_SERVER;
    snprintf(path, sizeof(path), "%s/server.crt", CFW_TEST_TLS_DIR);
    topts.cert_file = strdup(path);
    snprintf(path, sizeof(path), "%s/server.key", CFW_TEST_TLS_DIR);
    topts.key_file = strdup(path);
    status = cfw_tls_ctx_create(&topts, &server_ctx);
    free((void *)topts.cert_file);
    free((void *)topts.key_file);
    CFW_TEST_ASSERT_EQ(status, CFW_OK);

    memset(&topts, 0, sizeof(topts));
    topts.role = CFW_TLS_CLIENT;
    snprintf(path, sizeof(path), "%s/ca.crt", CFW_TEST_TLS_DIR);
    topts.ca_file = strdup(path);
    topts.verify_peer = 1;
    status = cfw_tls_ctx_create(&topts, &client_ctx);
    free((void *)topts.ca_file);
    CFW_TEST_ASSERT_EQ(status, CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_wsc_server_start(&fixture, server_ctx), 0);
    port = cfw_server_port(&fixture.server);

    cfw_wsc_opts_init(&opts, "localhost", port, "/ws");
    opts.tls_ctx = client_ctx;
    CFW_TEST_ASSERT_EQ(cfw_ws_client_connect(&opts, &ws), CFW_OK);

    prev = cfw_wsc_got_count;
    CFW_TEST_ASSERT_EQ(cfw_ws_client_send_text(ws, "secure-ws",
                                               sizeof("secure-ws") - 1),
                       CFW_OK);
    got = cfw_wsc_pump_for_message(ws, &prev);
    CFW_TEST_ASSERT_EQ(got, 1);
    CFW_TEST_ASSERT_EQ(cfw_wsc_got_size, sizeof("secure-ws") - 1);
    CFW_TEST_ASSERT_EQ(memcmp(cfw_wsc_got_payload, "secure-ws",
                              cfw_wsc_got_size),
                       0);

    cfw_ws_client_destroy(ws);
    cfw_wsc_server_stop(&fixture);
    cfw_tls_ctx_destroy(server_ctx);
    cfw_tls_ctx_destroy(client_ctx);
    return 0;
}

#endif /* CFW_HAVE_TLS */

/* ------------------------------------------------------------------ */
/* Raw fake server: completes the handshake then sends a MASKED frame  */
/* (a protocol violation — server frames must be unmasked).            */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t port;
    int result; /* 0 ok, 1 failure */
} cfw_wsc_fake_t;

static int cfw_wsc_fake_main(void *arg)
{
    cfw_wsc_fake_t *fake = (cfw_wsc_fake_t *)arg;
    int lfd = -1;
    int cfd = -1;
    struct sockaddr_in addr;
    char buf[8192];
    ssize_t n;
    char *key_start;
    char key[64];
    char accept_key[CFW_WS_ACCEPT_KEY_CAPACITY];
    char response[1024];
    int resp_len;
    struct timeval tv;

    /* Blocking sockets with a recv timeout so the thread always
     * exits even if the client misbehaves. */
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        fake->result = 1;
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(lfd, 1) != 0) {
        close(lfd);
        fake->result = 1;
        return 1;
    }
    {
        socklen_t sl = sizeof(addr);
        if (getsockname(lfd, (struct sockaddr *)&addr, &sl) != 0) {
            close(lfd);
            fake->result = 1;
            return 1;
        }
    }
    fake->port = ntohs(addr.sin_port);

    tv.tv_sec = 10;
    tv.tv_usec = 0;
    (void)setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) {
        close(lfd);
        fake->result = 1;
        return 1;
    }
    (void)setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Read the client's Upgrade request head. */
    n = recv(cfd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        close(cfd);
        close(lfd);
        fake->result = 1;
        return 1;
    }
    buf[n] = '\0';
    key_start = strstr(buf, "Sec-WebSocket-Key: ");
    if (key_start == NULL) {
        close(cfd);
        close(lfd);
        fake->result = 1;
        return 1;
    }
    key_start += sizeof("Sec-WebSocket-Key: ") - 1;
    {
        size_t k = 0;
        while (key_start[k] != '\r' && key_start[k] != '\n' &&
               k < sizeof(key) - 1) {
            key[k] = key_start[k];
            k++;
        }
        key[k] = '\0';
    }
    if (cfw_ws_accept_key(key, strlen(key), accept_key,
                          sizeof(accept_key)) != CFW_OK) {
        close(cfd);
        close(lfd);
        fake->result = 1;
        return 1;
    }

    resp_len = snprintf(response, sizeof(response),
                        "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: %s\r\n\r\n",
                        accept_key);
    if (resp_len <= 0 ||
        send(cfd, response, (size_t)resp_len, 0) != resp_len) {
        close(cfd);
        close(lfd);
        fake->result = 1;
        return 1;
    }

    /* Delay so the masked frame cannot arrive in the same read as the
     * 101 head — the client must complete connect() first, then reject
     * the frame during pump(). */
    {
        struct timespec pause = {0, 100000000};
        nanosleep(&pause, NULL);
    }

    /* Send a MASKED text frame — illegal from a server. The client
     * must reject it with a protocol-error close. */
    {
        static const unsigned char masked[] = {
            0x81, 0x81, 0x01, 0x02, 0x03, 0x04,
            (unsigned char)('x' ^ 0x01)
        };
        (void)send(cfd, masked, sizeof(masked), 0);
    }

    /* Read whatever the client sends back (close frame or nothing)
     * until EOF/timeout, then exit. */
    (void)recv(cfd, buf, sizeof(buf), 0);
    close(cfd);
    close(lfd);
    fake->result = 0;
    return 0;
}

static int test_ws_client_masked_server_frame(void)
{
    cfw_wsc_fake_t fake;
    cfw_thread_t thread;
    cfw_ws_client_options_t opts;
    cfw_ws_client_t *ws = NULL;
    cfw_duration_t slice;
    int i;
    int closed = 0;
    int exit_code = 0;

    cfw_wsc_got_count = 0;
    cfw_wsc_got_size = 0;
    memset(&fake, 0, sizeof(fake));
    CFW_TEST_ASSERT_EQ(cfw_thread_create(&thread, cfw_wsc_fake_main,
                                         &fake),
                       CFW_OK);

    /* Wait for the listener port. */
    for (i = 0; i < 200 && fake.port == 0; i++) {
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
    CFW_TEST_ASSERT_TRUE(fake.port != 0);

    cfw_wsc_opts_init(&opts, "127.0.0.1", fake.port, "/ws");
    CFW_TEST_ASSERT_EQ(cfw_ws_client_connect(&opts, &ws), CFW_OK);

    /* The masked frame must trigger a protocol failure: pump returns
     * (OK or error) and the client reports closed. */
    cfw_duration_from_millis(50, &slice);
    for (i = 0; i < 200 && !closed; i++) {
        cfw_status_t st = cfw_ws_client_pump(ws, &slice);
        (void)st;
        closed = cfw_ws_client_is_closed(ws);
    }
    CFW_TEST_ASSERT_TRUE(closed);
    CFW_TEST_ASSERT_EQ(cfw_wsc_got_count, 0);

    cfw_ws_client_destroy(ws);
    CFW_TEST_ASSERT_EQ(cfw_thread_join(&thread, &exit_code), CFW_OK);
    return 0;
}

const cfw_test_case cfw_test_ws_client_cases[] = {
    {"ws_client_echo", test_ws_client_echo},
    {"ws_client_large_message", test_ws_client_large_message},
    {"ws_client_handshake_rejected",
     test_ws_client_handshake_rejected},
    {"ws_client_bad_options", test_ws_client_bad_options},
    {"ws_client_masked_server_frame",
     test_ws_client_masked_server_frame},
#if CFW_HAVE_TLS
    {"ws_client_tls_echo", test_ws_client_tls_echo},
#endif
    {NULL, NULL},
};

#else /* Windows */

const cfw_test_case cfw_test_ws_client_cases[] = {
    {NULL, NULL},
};

#endif /* POSIX */
