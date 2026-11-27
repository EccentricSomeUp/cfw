/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Windows runtime smoke test (ADR 0072/0074): exercises the Winsock
 * socket layer, WSAPoll pollset, fd close, and a full cleartext
 * cfw_server request/response cycle over loopback — all through the
 * public API only. Runs on windows-latest CI where the POSIX unit
 * tests cannot execute.
 */

#include <cfw/cfw.h>

#include "private/platform_detect.h"

#if CFW_PLATFORM_WINDOWS
    #include <winsock2.h>
#else
    #include <sys/socket.h>
#endif

#include <stdio.h>
#include <string.h>

static int cfw_wsm_failures = 0;

#define WSM_ASSERT(cond)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "smoke: %s:%d: %s\n", __FILE__, __LINE__,         \
                    #cond);                                                  \
            cfw_wsm_failures++;                                              \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static cfw_status_t wsm_hello(cfw_request_t *req, cfw_response_t *res,
                              void *user)
{
    static const char body[] = "Hello, Windows!";
    (void)req;
    (void)user;
    res->status = CFW_HTTP_STATUS_OK;
    res->body.data = (const uint8_t *)body;
    res->body.size = sizeof(body) - 1;
    return CFW_OK;
}

/* Blocking connect helper: loopback connect completes synchronously
 * once the socket is temporarily switched to blocking mode. */
static int wsm_connect(cfw_fd_t *fd, const cfw_addr_t *addr)
{
    cfw_status_t st;

    st = cfw_net_socket(cfw_addr_is_ipv6(addr) ? AF_INET6 : AF_INET,
                        SOCK_STREAM, 0, fd);
    if (st != CFW_OK) {
        return 1;
    }
    if (cfw_net_set_nonblocking(fd, 0) != CFW_OK ||
        cfw_net_connect(fd, addr) != CFW_OK ||
        cfw_net_set_nonblocking(fd, 1) != CFW_OK) {
        cfw_fd_close(fd);
        return 1;
    }
    return 0;
}

static int wsm_addr(void)
{
    cfw_addr_t addr;
    char text[48];
    uint16_t port = 0;

    WSM_ASSERT(cfw_addr_from_str(&addr, "127.0.0.1", 8080) == CFW_OK);
    WSM_ASSERT(cfw_addr_is_ipv4(&addr));
    WSM_ASSERT(cfw_addr_port(&addr, &port) == CFW_OK && port == 8080);
    WSM_ASSERT(cfw_addr_to_str(&addr, text, sizeof(text)) == CFW_OK);
    WSM_ASSERT(strcmp(text, "127.0.0.1") == 0);

    /* getaddrinfo path (ws2_32 on Windows) for a name-resolution
     * sanity check. */
    WSM_ASSERT(cfw_addr_resolve(&addr, "localhost", 80,
                                CFW_ADDR_FAMILY_IPV4) == CFW_OK);
    return 0;
}

static int wsm_net_pollset(void)
{
    cfw_fd_t listener, client, server_side;
    cfw_addr_t addr;
    cfw_pollset_t *pollset = NULL;
    cfw_poll_event_t events[4];
    size_t count = 0;
    cfw_timepoint_t deadline;
    cfw_duration_t slice;
    static const char ping[] = "ping";
    char buf[16];
    size_t got = 0;

    cfw_fd_init(&listener);
    cfw_fd_init(&client);
    cfw_fd_init(&server_side);

    WSM_ASSERT(cfw_addr_from_str(&addr, "127.0.0.1", 0) == CFW_OK);
    WSM_ASSERT(cfw_net_socket(AF_INET, SOCK_STREAM, 0, &listener) ==
               CFW_OK);
    WSM_ASSERT(cfw_net_bind(&listener, &addr) == CFW_OK);
    WSM_ASSERT(cfw_net_listen(&listener, 4) == CFW_OK);
    WSM_ASSERT(cfw_net_local_addr(&listener, &addr) == CFW_OK);

    WSM_ASSERT(wsm_connect(&client, &addr) == 0);
    WSM_ASSERT(cfw_net_accept(&listener, NULL, &server_side) == CFW_OK);

    WSM_ASSERT(cfw_pollset_create(&pollset) == CFW_OK);
    WSM_ASSERT(cfw_pollset_add(pollset, cfw_fd_get(&server_side),
                               CFW_POLL_IN, NULL) == CFW_OK);

    /* Nothing sent yet: a short deadline must expire with no events. */
    WSM_ASSERT(cfw_duration_from_millis(50, &slice) == CFW_OK);
    WSM_ASSERT(cfw_deadline_from_now(&deadline, slice) == CFW_OK);
    WSM_ASSERT(cfw_pollset_wait(pollset, &deadline, events,
                                sizeof(events) / sizeof(events[0]),
                                &count) == CFW_ERROR_TIMEOUT);
    WSM_ASSERT(count == 0);

    {
        size_t sent = 0;
        WSM_ASSERT(cfw_net_send(&client, ping, sizeof(ping) - 1,
                                &sent) == CFW_OK);
        WSM_ASSERT(sent == sizeof(ping) - 1);
    }

    WSM_ASSERT(cfw_duration_from_millis(2000, &slice) == CFW_OK);
    WSM_ASSERT(cfw_deadline_from_now(&deadline, slice) == CFW_OK);
    WSM_ASSERT(cfw_pollset_wait(pollset, &deadline, events,
                                sizeof(events) / sizeof(events[0]),
                                &count) == CFW_OK);
    WSM_ASSERT(count >= 1);
    WSM_ASSERT(events[0].handle == cfw_fd_get(&server_side));
    WSM_ASSERT((events[0].events & CFW_POLL_IN) != 0);

    WSM_ASSERT(cfw_net_recv(&server_side, buf, sizeof(buf) - 1,
                            &got) == CFW_OK);
    WSM_ASSERT(got == sizeof(ping) - 1);
    buf[got] = '\0';
    WSM_ASSERT(memcmp(buf, ping, got) == 0);

    /* Nonblocking recv with no data must report WOULDBLOCK, not block. */
    WSM_ASSERT(cfw_net_recv(&server_side, buf, sizeof(buf), &got) ==
               CFW_ERROR_WOULDBLOCK);

    /* Peer close is observed as readability followed by EOF
     * (CFW_OK with zero bytes). */
    cfw_fd_close(&client);
    WSM_ASSERT(cfw_duration_from_millis(2000, &slice) == CFW_OK);
    WSM_ASSERT(cfw_deadline_from_now(&deadline, slice) == CFW_OK);
    WSM_ASSERT(cfw_pollset_wait(pollset, &deadline, events,
                                sizeof(events) / sizeof(events[0]),
                                &count) == CFW_OK);
    WSM_ASSERT(count >= 1);
    got = 1;
    WSM_ASSERT(cfw_net_recv(&server_side, buf, sizeof(buf), &got) ==
               CFW_OK);
    WSM_ASSERT(got == 0);

    cfw_fd_close(&server_side);
    cfw_fd_close(&listener);
    cfw_pollset_destroy(pollset);
    return 0;
}

#if CFW_HAVE_TLS

/* TLS smoke (ADR 0075): a cleartext cfw_server upgraded to a TLS
 * listener via tls_ctx, exercised end-to-end by a cfw_tls_conn client
 * over loopback. Certificate paths come from CFW_TEST_TLS_DIR, which
 * CMake defines only for TLS-enabled builds. */
static int wsm_tls(void)
{
    char cert[512];
    char key[512];
    char ca[512];
    cfw_tls_ctx_options_t sopt;
    cfw_tls_ctx_options_t copt;
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_conn_t *tconn = NULL;
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t dopt;
    cfw_server_options_t srv;
    cfw_server_t server;
    cfw_strview_t pattern;
    cfw_fd_t client;
    cfw_addr_t addr;
    static const char req[] =
        "GET /secure HTTP/1.1\r\nHost: localhost\r\n"
        "Connection: close\r\n\r\n";
    char response[4096];
    size_t used = 0;
    size_t off = 0;
    int done = 0;
    int spins;

    snprintf(cert, sizeof(cert), "%s/server.crt", CFW_TEST_TLS_DIR);
    snprintf(key, sizeof(key), "%s/server.key", CFW_TEST_TLS_DIR);
    snprintf(ca, sizeof(ca), "%s/ca.crt", CFW_TEST_TLS_DIR);

    memset(&sopt, 0, sizeof(sopt));
    sopt.role = CFW_TLS_SERVER;
    sopt.cert_file = cert;
    sopt.key_file = key;
    WSM_ASSERT(cfw_tls_ctx_create(&sopt, &sctx) == CFW_OK);

    memset(&copt, 0, sizeof(copt));
    copt.role = CFW_TLS_CLIENT;
    copt.ca_file = ca;
    copt.verify_peer = 1;
    WSM_ASSERT(cfw_tls_ctx_create(&copt, &cctx) == CFW_OK);

    arena = cfw_arena_create(1u << 20);
    WSM_ASSERT(arena != NULL);
    WSM_ASSERT(cfw_router_init(&router, arena) == CFW_OK);

    memset(&dopt, 0, sizeof(dopt));
    dopt.router = &router;
    dopt.handler = wsm_hello;
    dopt.max_middleware = 4;
    WSM_ASSERT(cfw_dispatch_init(&dispatch, arena, &dopt) == CFW_OK);
    WSM_ASSERT(cfw_strview_from_cstr(&pattern, "/secure") == CFW_OK);
    WSM_ASSERT(cfw_dispatch_add_route(&dispatch, CFW_HTTP_METHOD_GET,
                                    &pattern, wsm_hello, NULL) == CFW_OK);

    memset(&srv, 0, sizeof(srv));
    srv.host = "127.0.0.1";
    srv.port = 0;
    srv.dispatch = &dispatch;
    srv.tls_ctx = sctx;
    srv.http.max_head_size = 8192;
    srv.http.max_pending_size = 65536;
    srv.max_out_size = 8192;
    srv.max_connections = 16;
    srv.max_headers = 32;
    srv.max_params = 16;
    WSM_ASSERT(cfw_server_init(&server, &srv) == CFW_OK);
    WSM_ASSERT(cfw_server_port(&server) != 0);

    WSM_ASSERT(cfw_addr_from_str(&addr, "127.0.0.1",
                                 cfw_server_port(&server)) == CFW_OK);
    cfw_fd_init(&client);
    WSM_ASSERT(wsm_connect(&client, &addr) == 0);
    WSM_ASSERT(cfw_tls_conn_create(cctx, cfw_fd_get(&client),
                                   &tconn) == CFW_OK);

    /* Each WOULDBLOCK retry is paced by cfw_server_step with a short
     * deadline: it processes whatever the peer has sent and waits
     * briefly when idle, which gives the client socket time to become
     * ready without a second pollset. */
    for (spins = 0; spins < 2000; spins++) {
        cfw_timepoint_t deadline;
        cfw_duration_t slice;
        cfw_status_t st = cfw_tls_handshake(tconn);
        if (st == CFW_OK) {
            break;
        }
        WSM_ASSERT(st == CFW_ERROR_WOULDBLOCK);
        WSM_ASSERT(cfw_duration_from_millis(10, &slice) == CFW_OK);
        WSM_ASSERT(cfw_deadline_from_now(&deadline, slice) == CFW_OK);
        WSM_ASSERT(cfw_server_step(&server, &deadline) == CFW_OK);
    }
    WSM_ASSERT(spins < 2000);
    WSM_ASSERT(cfw_tls_handshake_done(tconn));

    /* Write the request (may partial-write; retry on WOULDBLOCK). */
    while (off < sizeof(req) - 1) {
        cfw_timepoint_t deadline;
        cfw_duration_t slice;
        size_t sent = 0;
        cfw_status_t st =
            cfw_tls_write(tconn, req + off, sizeof(req) - 1 - off, &sent);
        if (st == CFW_OK) {
            off += sent;
            continue;
        }
        WSM_ASSERT(st == CFW_ERROR_WOULDBLOCK);
        WSM_ASSERT(cfw_duration_from_millis(10, &slice) == CFW_OK);
        WSM_ASSERT(cfw_deadline_from_now(&deadline, slice) == CFW_OK);
        WSM_ASSERT(cfw_server_step(&server, &deadline) == CFW_OK);
    }

    /* Read the response while stepping the server. */
    for (spins = 0; spins < 2000 && !done; spins++) {
        cfw_timepoint_t deadline;
        cfw_duration_t slice;
        size_t got = 0;
        cfw_status_t st =
            cfw_tls_read(tconn, response + used,
                         sizeof(response) - 1 - used, &got);
        if (st == CFW_OK) {
            if (got == 0) {
                break;
            }
            used += got;
            response[used] = '\0';
            if (strstr(response, "200") != NULL &&
                strstr(response, "Hello, Windows!") != NULL) {
                done = 1;
            }
        } else {
            WSM_ASSERT(st == CFW_ERROR_WOULDBLOCK);
        }
        WSM_ASSERT(cfw_duration_from_millis(10, &slice) == CFW_OK);
        WSM_ASSERT(cfw_deadline_from_now(&deadline, slice) == CFW_OK);
        WSM_ASSERT(cfw_server_step(&server, &deadline) == CFW_OK);
    }
    WSM_ASSERT(done);

    cfw_tls_conn_destroy(tconn);
    cfw_server_stop(&server);
    cfw_fd_close(&client);
    cfw_server_free(&server);
    cfw_tls_ctx_destroy(cctx);
    cfw_tls_ctx_destroy(sctx);
    cfw_arena_destroy(arena);
    return 0;
}

#endif /* CFW_HAVE_TLS */

static int wsm_server(void)
{
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_dispatch_options_t dopt;
    cfw_server_options_t sopt;
    cfw_server_t server;
    cfw_strview_t pattern;
    cfw_fd_t client;
    cfw_addr_t addr;
    char response[4096];
    size_t used = 0;
    int done = 0;
    int spins = 0;

    arena = cfw_arena_create(1u << 20);
    WSM_ASSERT(arena != NULL);
    WSM_ASSERT(cfw_router_init(&router, arena) == CFW_OK);

    memset(&dopt, 0, sizeof(dopt));
    dopt.router = &router;
    dopt.handler = wsm_hello;
    dopt.max_middleware = 4;
    WSM_ASSERT(cfw_dispatch_init(&dispatch, arena, &dopt) == CFW_OK);
    WSM_ASSERT(cfw_strview_from_cstr(&pattern, "/hello") == CFW_OK);
    WSM_ASSERT(cfw_dispatch_add_route(&dispatch, CFW_HTTP_METHOD_GET,
                                    &pattern, wsm_hello, NULL) == CFW_OK);

    memset(&sopt, 0, sizeof(sopt));
    sopt.host = "127.0.0.1";
    sopt.port = 0;
    sopt.dispatch = &dispatch;
    sopt.http.max_head_size = 8192;
    sopt.http.max_pending_size = 65536;
    sopt.max_out_size = 8192;
    sopt.max_connections = 16;
    sopt.max_headers = 32;
    sopt.max_params = 16;
    WSM_ASSERT(cfw_server_init(&server, &sopt) == CFW_OK);
    WSM_ASSERT(cfw_server_port(&server) != 0);

    WSM_ASSERT(cfw_addr_from_str(&addr, "127.0.0.1",
                                 cfw_server_port(&server)) == CFW_OK);
    cfw_fd_init(&client);
    WSM_ASSERT(wsm_connect(&client, &addr) == 0);

    {
        static const char req[] =
            "GET /hello HTTP/1.1\r\nHost: localhost\r\n"
            "Connection: close\r\n\r\n";
        size_t sent = 0;
        WSM_ASSERT(cfw_net_send(&client, req, sizeof(req) - 1,
                                &sent) == CFW_OK);
        WSM_ASSERT(sent == sizeof(req) - 1);
    }

    /* Drive the server one step at a time while polling the client for
     * the response; no threads required. */
    while (!done && spins < 2000) {
        cfw_timepoint_t deadline;
        cfw_duration_t slice;
        size_t got = 0;
        cfw_status_t st;

        WSM_ASSERT(cfw_duration_from_millis(50, &slice) == CFW_OK);
        WSM_ASSERT(cfw_deadline_from_now(&deadline, slice) == CFW_OK);
        /* step() reports CFW_OK even when the wait deadline expires. */
        WSM_ASSERT(cfw_server_step(&server, &deadline) == CFW_OK);

        st = cfw_net_recv(&client, response + used,
                          sizeof(response) - 1 - used, &got);
        if (st == CFW_OK && got > 0) {
            used += got;
            response[used] = '\0';
            if (strstr(response, "200") != NULL &&
                strstr(response, "Hello, Windows!") != NULL) {
                done = 1;
            }
        } else if (st == CFW_OK && got == 0) {
            break; /* peer closed */
        } else {
            WSM_ASSERT(st == CFW_ERROR_WOULDBLOCK);
        }
        spins++;
    }
    WSM_ASSERT(done);
    WSM_ASSERT(used > 0 && used < sizeof(response));

    cfw_server_stop(&server);
    cfw_fd_close(&client);
    cfw_server_free(&server);
    cfw_arena_destroy(arena);
    return 0;
}

int main(void)
{
    if (wsm_addr() != 0) {
        return 1;
    }
    if (wsm_net_pollset() != 0) {
        return 1;
    }
    if (wsm_server() != 0) {
        return 1;
    }
#if CFW_HAVE_TLS
    if (wsm_tls() != 0) {
        return 1;
    }
#endif
    if (cfw_wsm_failures != 0) {
        return 1;
    }
    printf("windows smoke: ok\n");
    return 0;
}
