/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "test_harness.h"

#include <cfw/tls.h>

#if CFW_HAVE_TLS

    #include <fcntl.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include <sys/socket.h>
    #include <unistd.h>

/* Test certificates are generated into the build tree by CMake
 * (tests/CMakeLists.txt); no key material is committed. */
#define TLS_DIR CFW_TEST_TLS_DIR

static void tls_path(char *out, size_t size, const char *name)
{
    snprintf(out, size, "%s/%s", TLS_DIR, name);
}

static int make_ctx(cfw_tls_ctx_options_t *o, const char *cert,
                    const char *key, const char *ca, int verify,
                    cfw_tls_ctx_t **ctx)
{
    char path[512];
    cfw_status_t st;

    memset(o, 0, sizeof(*o));
    if (cert != NULL) {
        tls_path(path, sizeof(path), cert);
        o->cert_file = strdup(path);
        CFW_TEST_ASSERT(o->cert_file != NULL);
    }
    if (key != NULL) {
        tls_path(path, sizeof(path), key);
        o->key_file = strdup(path);
        CFW_TEST_ASSERT(o->key_file != NULL);
    }
    if (ca != NULL) {
        tls_path(path, sizeof(path), ca);
        o->ca_file = strdup(path);
        CFW_TEST_ASSERT(o->ca_file != NULL);
    }
    o->verify_peer = verify;

    st = cfw_tls_ctx_create(o, ctx);
    free((void *)o->cert_file);
    free((void *)o->key_file);
    free((void *)o->ca_file);
    return st == CFW_OK ? 0 : 1;
}

static int tls_ctx_server(cfw_tls_ctx_t **ctx)
{
    cfw_tls_ctx_options_t o;
    return make_ctx(&o, "server.crt", "server.key", NULL, 0, ctx);
}

/* Drives both handshakes alternately until both complete, one side
 * fails, or the iteration bound is hit. Returns 0 on success. */
static int tls_drive(cfw_tls_conn_t *a, cfw_tls_conn_t *b)
{
    int a_done = 0;
    int b_done = 0;
    int i;

    for (i = 0; i < 100000 && !(a_done && b_done); i++) {
        cfw_status_t st;

        if (!a_done) {
            st = cfw_tls_handshake(a);
            if (st == CFW_OK) {
                a_done = 1;
            } else if (st != CFW_ERROR_WOULDBLOCK) {
                return 1;
            }
        }
        if (!b_done) {
            st = cfw_tls_handshake(b);
            if (st == CFW_OK) {
                b_done = 1;
            } else if (st != CFW_ERROR_WOULDBLOCK) {
                return 1;
            }
        }
    }
    return !(a_done && b_done);
}

/* Raw fds of the most recent tls_pair(); TLS conns borrow them, so
 * tls_teardown closes them after destroying the conns. */
static int tls_pair_fds[2] = {-1, -1};

/* Creates a connected nonblocking socketpair wrapped in TLS conns.
 * Returns 0 on success; server_out and client_out own the conns (the
 * borrowed fds are tracked in tls_pair_fds for tls_teardown). */
static int tls_pair(cfw_tls_ctx_t *sctx, cfw_tls_ctx_t *cctx,
                    cfw_tls_conn_t **server_out, cfw_tls_conn_t **client_out)
{
    int fds[2];
    cfw_tls_conn_t *server;
    cfw_tls_conn_t *client;
    int flags;
    cfw_status_t st;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return 1;
    }
    flags = fcntl(fds[0], F_GETFL, 0);
    (void)fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(fds[1], F_GETFL, 0);
    (void)fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);

    st = cfw_tls_conn_create(sctx, fds[0], &server);
    if (st != CFW_OK) {
        close(fds[0]);
        close(fds[1]);
        return 1;
    }
    st = cfw_tls_conn_create(cctx, fds[1], &client);
    if (st != CFW_OK) {
        cfw_tls_conn_destroy(server);
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    tls_pair_fds[0] = fds[0];
    tls_pair_fds[1] = fds[1];
    *server_out = server;
    *client_out = client;
    return 0;
}

static void tls_teardown(cfw_tls_conn_t *server, cfw_tls_conn_t *client)
{
    cfw_tls_conn_destroy(server);
    cfw_tls_conn_destroy(client);
    if (tls_pair_fds[0] >= 0) {
        close(tls_pair_fds[0]);
        tls_pair_fds[0] = -1;
    }
    if (tls_pair_fds[1] >= 0) {
        close(tls_pair_fds[1]);
        tls_pair_fds[1] = -1;
    }
}

/* Writes all of `text` client->server (retrying partial writes and
 * WOULDBLOCK), then reads on the server until the full message
 * arrives and matches. */
static int tls_send_all(cfw_tls_conn_t *from, cfw_tls_conn_t *to,
                        const void *data, size_t len)
{
    char buf[512];
    size_t sent = 0;
    size_t got = 0;
    const char *p = data;
    int i;

    for (i = 0; i < 100000 && sent < len; i++) {
        size_t done = 0;
        cfw_status_t st = cfw_tls_write(from, p + sent, len - sent, &done);
        if (st == CFW_OK) {
            sent += done;
        } else if (st != CFW_ERROR_WOULDBLOCK) {
            return 1;
        }
    }
    if (sent != len) {
        return 1;
    }

    memset(buf, 0, sizeof(buf));
    CFW_TEST_ASSERT(len <= sizeof(buf));
    {
        int reads = 0;
        while (got < len) {
            size_t done = 0;
            cfw_status_t st = cfw_tls_read(to, buf + got,
                                           sizeof(buf) - got, &done);
            if (st == CFW_OK) {
                if (done == 0) {
                    return 1;
                }
                got += done;
            } else if (st != CFW_ERROR_WOULDBLOCK) {
                return 1;
            }
            if (++reads > 100000) {
                return 1;
            }
        }
    }
    return !(got == len && memcmp(buf, data, len) == 0);
}

/* Sends `text` client->server and echoes it back server->client,
 * asserting exact round-trip. */
static int tls_echo_check(cfw_tls_conn_t *server, cfw_tls_conn_t *client,
                          const char *text)
{
    const char *reply = "cfw-tls-echo-ok";

    if (tls_send_all(client, server, text, strlen(text))) {
        return 1;
    }
    if (tls_send_all(server, client, reply, strlen(reply))) {
        return 1;
    }
    return 0;
}

static int test_tls_ctx_invalid_args(void)
{
    cfw_tls_ctx_t *ctx = NULL;
    cfw_tls_ctx_options_t o;

    CFW_TEST_ASSERT_EQ(cfw_tls_ctx_create(NULL, &ctx),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT(ctx == NULL);

    memset(&o, 0, sizeof(o));
    o.role = CFW_TLS_SERVER;
    /* Server requires cert_file/key_file. */
    CFW_TEST_ASSERT_EQ(cfw_tls_ctx_create(&o, &ctx),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* verify_peer requires ca_file. */
    {
        char cert[512];
        char key[512];
        tls_path(cert, sizeof(cert), "server.crt");
        tls_path(key, sizeof(key), "server.key");
        o.cert_file = cert;
        o.key_file = key;
        o.verify_peer = 1;
        o.ca_file = NULL;
        CFW_TEST_ASSERT_EQ(cfw_tls_ctx_create(&o, &ctx),
                           CFW_ERROR_INVALID_ARGUMENT);
    }
    return 0;
}

static int test_tls_handshake_and_echo(void)
{
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_ctx_options_t copt;
    cfw_tls_conn_t *server = NULL;
    cfw_tls_conn_t *client = NULL;
    int rc = 1;

    CFW_TEST_ASSERT(tls_ctx_server(&sctx) == 0);
    /* Client ctx without verification (test CA is private to the
     * build tree; verification is covered by dedicated cases). */
    memset(&copt, 0, sizeof(copt));
    copt.role = CFW_TLS_CLIENT;
    CFW_TEST_ASSERT(cfw_tls_ctx_create(&copt, &cctx) == CFW_OK);

    CFW_TEST_ASSERT(tls_pair(sctx, cctx, &server, &client) == 0);
    CFW_TEST_ASSERT(cfw_tls_handshake_done(server) == 0);
    CFW_TEST_ASSERT(tls_drive(server, client) == 0);
    CFW_TEST_ASSERT(cfw_tls_handshake_done(server) == 1);
    CFW_TEST_ASSERT(cfw_tls_handshake_done(client) == 1);
    CFW_TEST_ASSERT(cfw_tls_pending(client) == 0);
    CFW_TEST_ASSERT(cfw_tls_conn_fd(server) >= 0);

    if (tls_echo_check(server, client, "hello tls")) {
        goto done;
    }
    rc = 0;
done:
    tls_teardown(server, client);
    cfw_tls_ctx_destroy(sctx);
    cfw_tls_ctx_destroy(cctx);
    return rc;
}

static int test_tls_wouldblock_progression(void)
{
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_ctx_options_t copt;
    cfw_tls_conn_t *server = NULL;
    cfw_tls_conn_t *client = NULL;
    int saw_wouldblock = 0;
    int saw_want = 0;
    int i;
    int rc = 1;

    CFW_TEST_ASSERT(tls_ctx_server(&sctx) == 0);
    memset(&copt, 0, sizeof(copt));
    copt.role = CFW_TLS_CLIENT;
    CFW_TEST_ASSERT(cfw_tls_ctx_create(&copt, &cctx) == CFW_OK);
    CFW_TEST_ASSERT(tls_pair(sctx, cctx, &server, &client) == 0);

    for (i = 0; i < 100000; i++) {
        cfw_status_t st = cfw_tls_handshake(client);
        if (st == CFW_OK) {
            break;
        }
        if (st == CFW_ERROR_WOULDBLOCK) {
            saw_wouldblock = 1;
            if (cfw_tls_conn_want_read(client) ||
                cfw_tls_conn_want_write(client)) {
                saw_want = 1;
            }
            (void)cfw_tls_handshake(server);
        } else {
            goto done;
        }
    }
    CFW_TEST_ASSERT(i < 100000);
    CFW_TEST_ASSERT(saw_wouldblock);
    CFW_TEST_ASSERT(saw_want);
    CFW_TEST_ASSERT(cfw_tls_handshake_done(client));
    /* Server completes once it sees the client's Finished flight. */
    for (i = 0; i < 100000 && !cfw_tls_handshake_done(server); i++) {
        (void)cfw_tls_handshake(server);
    }
    CFW_TEST_ASSERT(cfw_tls_handshake_done(server));
    rc = 0;
done:
    tls_teardown(server, client);
    cfw_tls_ctx_destroy(sctx);
    cfw_tls_ctx_destroy(cctx);
    return rc;
}

static int test_tls_client_verifies_server(void)
{
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_conn_t *server = NULL;
    cfw_tls_conn_t *client = NULL;
    int rc = 1;

    CFW_TEST_ASSERT(tls_ctx_server(&sctx) == 0);
    {
        cfw_tls_ctx_options_t o;
        char ca[512];
        tls_path(ca, sizeof(ca), "ca.crt");
        memset(&o, 0, sizeof(o));
        o.role = CFW_TLS_CLIENT;
        o.ca_file = ca;
        o.verify_peer = 1;
        CFW_TEST_ASSERT(cfw_tls_ctx_create(&o, &cctx) == CFW_OK);
    }
    CFW_TEST_ASSERT(tls_pair(sctx, cctx, &server, &client) == 0);
    CFW_TEST_ASSERT(tls_drive(server, client) == 0);
    rc = 0;
    tls_teardown(server, client);
    cfw_tls_ctx_destroy(sctx);
    cfw_tls_ctx_destroy(cctx);
    return rc;
}

static int test_tls_client_rejects_bad_ca(void)
{
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_conn_t *server = NULL;
    cfw_tls_conn_t *client = NULL;
    int client_failed = 0;
    int i;

    CFW_TEST_ASSERT(tls_ctx_server(&sctx) == 0);
    {
        /* Trust anchor that did not sign the server cert: the server
         * certificate itself is not a CA, so verification must fail. */
        cfw_tls_ctx_options_t o;
        char ca[512];
        tls_path(ca, sizeof(ca), "server.crt");
        memset(&o, 0, sizeof(o));
        o.role = CFW_TLS_CLIENT;
        o.ca_file = ca;
        o.verify_peer = 1;
        CFW_TEST_ASSERT(cfw_tls_ctx_create(&o, &cctx) == CFW_OK);
    }
    CFW_TEST_ASSERT(tls_pair(sctx, cctx, &server, &client) == 0);

    for (i = 0; i < 100000; i++) {
        cfw_status_t st = cfw_tls_handshake(client);
        if (st == CFW_ERROR_IO) {
            client_failed = 1;
            break;
        }
        if (st != CFW_OK && st != CFW_ERROR_WOULDBLOCK) {
            break;
        }
        (void)cfw_tls_handshake(server);
        if (cfw_tls_handshake_done(client) &&
            cfw_tls_handshake_done(server)) {
            break;
        }
    }
    CFW_TEST_ASSERT(client_failed || i >= 100000);
    CFW_TEST_ASSERT(!cfw_tls_handshake_done(client));
    tls_teardown(server, client);
    cfw_tls_ctx_destroy(sctx);
    cfw_tls_ctx_destroy(cctx);
    return 0;
}

static int test_tls_server_requires_client_cert(void)
{
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_conn_t *server = NULL;
    cfw_tls_conn_t *client = NULL;
    int server_failed = 0;
    int i;

    {
        cfw_tls_ctx_options_t o;
        char cert[512];
        char key[512];
        char ca[512];
        tls_path(cert, sizeof(cert), "server.crt");
        tls_path(key, sizeof(key), "server.key");
        tls_path(ca, sizeof(ca), "ca.crt");
        memset(&o, 0, sizeof(o));
        o.role = CFW_TLS_SERVER;
        o.cert_file = cert;
        o.key_file = key;
        o.ca_file = ca;
        o.verify_peer = 1;
        CFW_TEST_ASSERT(cfw_tls_ctx_create(&o, &sctx) == CFW_OK);
    }
    {
        /* Client presents no certificate. */
        cfw_tls_ctx_options_t o;
        memset(&o, 0, sizeof(o));
        o.role = CFW_TLS_CLIENT;
        CFW_TEST_ASSERT(cfw_tls_ctx_create(&o, &cctx) == CFW_OK);
    }
    CFW_TEST_ASSERT(tls_pair(sctx, cctx, &server, &client) == 0);

    for (i = 0; i < 100000; i++) {
        cfw_status_t st = cfw_tls_handshake(server);
        if (st == CFW_ERROR_IO) {
            server_failed = 1;
            break;
        }
        if (st != CFW_OK && st != CFW_ERROR_WOULDBLOCK) {
            break;
        }
        (void)cfw_tls_handshake(client);
    }
    CFW_TEST_ASSERT(server_failed || i >= 100000);
    CFW_TEST_ASSERT(!cfw_tls_handshake_done(server));
    tls_teardown(server, client);
    cfw_tls_ctx_destroy(sctx);
    cfw_tls_ctx_destroy(cctx);
    return 0;
}

static int test_tls_client_cert_accepted(void)
{
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_conn_t *server = NULL;
    cfw_tls_conn_t *client = NULL;
    int rc = 1;

    {
        cfw_tls_ctx_options_t o;
        char cert[512];
        char key[512];
        char ca[512];
        tls_path(cert, sizeof(cert), "server.crt");
        tls_path(key, sizeof(key), "server.key");
        tls_path(ca, sizeof(ca), "ca.crt");
        memset(&o, 0, sizeof(o));
        o.role = CFW_TLS_SERVER;
        o.cert_file = cert;
        o.key_file = key;
        o.ca_file = ca;
        o.verify_peer = 1;
        CFW_TEST_ASSERT(cfw_tls_ctx_create(&o, &sctx) == CFW_OK);
    }
    {
        cfw_tls_ctx_options_t o;
        char cert[512];
        char key[512];
        char ca[512];
        tls_path(cert, sizeof(cert), "client.crt");
        tls_path(key, sizeof(key), "client.key");
        tls_path(ca, sizeof(ca), "ca.crt");
        memset(&o, 0, sizeof(o));
        o.role = CFW_TLS_CLIENT;
        o.cert_file = cert;
        o.key_file = key;
        o.ca_file = ca;
        o.verify_peer = 1;
        CFW_TEST_ASSERT(cfw_tls_ctx_create(&o, &cctx) == CFW_OK);
    }
    CFW_TEST_ASSERT(tls_pair(sctx, cctx, &server, &client) == 0);
    CFW_TEST_ASSERT(tls_drive(server, client) == 0);
    rc = 0;
    tls_teardown(server, client);
    cfw_tls_ctx_destroy(sctx);
    cfw_tls_ctx_destroy(cctx);
    return rc;
}

static int test_tls_shutdown_and_eof(void)
{
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_ctx_options_t copt;
    cfw_tls_conn_t *server = NULL;
    cfw_tls_conn_t *client = NULL;
    size_t rdone;
    char buf[8];
    cfw_status_t st;
    int i;
    int rc = 1;

    CFW_TEST_ASSERT(tls_ctx_server(&sctx) == 0);
    memset(&copt, 0, sizeof(copt));
    copt.role = CFW_TLS_CLIENT;
    CFW_TEST_ASSERT(cfw_tls_ctx_create(&copt, &cctx) == CFW_OK);
    CFW_TEST_ASSERT(tls_pair(sctx, cctx, &server, &client) == 0);
    CFW_TEST_ASSERT(tls_drive(server, client) == 0);

    /* Mutual close_notify: client shuts down (its close_notify is
     * sent while awaiting the peer's), server reads EOF then closes
     * back, and the client's shutdown completes. */
    for (i = 0; i < 100000; i++) {
        st = cfw_tls_shutdown(client);
        if (st == CFW_ERROR_WOULDBLOCK) {
            break;
        }
        if (st != CFW_OK) {
            goto done;
        }
    }
    if (i >= 100000 || st != CFW_ERROR_WOULDBLOCK) {
        goto done;
    }
    for (i = 0; i < 100000; i++) {
        st = cfw_tls_read(server, buf, sizeof(buf), &rdone);
        if (st == CFW_OK) {
            CFW_TEST_ASSERT_EQ(rdone, 0);
            break;
        }
        if (st != CFW_ERROR_WOULDBLOCK) {
            goto done;
        }
    }
    if (i >= 100000 || st != CFW_OK) {
        goto done;
    }
    for (i = 0; i < 100000; i++) {
        st = cfw_tls_shutdown(server);
        if (st == CFW_ERROR_WOULDBLOCK) {
            break;
        }
        if (st != CFW_OK) {
            goto done;
        }
    }
    for (i = 0; i < 100000; i++) {
        st = cfw_tls_shutdown(client);
        if (st == CFW_OK) {
            rc = 0;
            break;
        }
        if (st != CFW_ERROR_WOULDBLOCK) {
            goto done;
        }
    }
done:
    tls_teardown(server, client);
    cfw_tls_ctx_destroy(sctx);
    cfw_tls_ctx_destroy(cctx);
    return rc;
}

#endif /* CFW_HAVE_TLS */

const cfw_test_case cfw_test_tls_cases[] = {
#if CFW_HAVE_TLS
    {"tls_ctx_invalid_args", test_tls_ctx_invalid_args},
    {"tls_handshake_and_echo", test_tls_handshake_and_echo},
    {"tls_wouldblock_progression", test_tls_wouldblock_progression},
    {"tls_client_verifies_server", test_tls_client_verifies_server},
    {"tls_client_rejects_bad_ca", test_tls_client_rejects_bad_ca},
    {"tls_server_requires_client_cert",
     test_tls_server_requires_client_cert},
    {"tls_client_cert_accepted", test_tls_client_cert_accepted},
    {"tls_shutdown_and_eof", test_tls_shutdown_and_eof},
#endif
    {NULL, NULL},
};
