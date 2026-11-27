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

static int tls_ctx_client_alpn(const char * const *protos,
                               cfw_tls_ctx_t **ctx);

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

    /* The server sees the client's certificate DER; the client sees
     * the server's. Both must be well-formed X.509 DER (SEQUENCE
     * tag 0x30) and sizeable (>200 bytes for these test certs). */
    {
        unsigned char der[4096];
        size_t len = 0;
        size_t need = 0;

        CFW_TEST_ASSERT(cfw_tls_conn_peer_cert_der(server, der,
                                                   sizeof(der),
                                                   &len) == CFW_OK);
        CFW_TEST_ASSERT(len > 200u && len <= sizeof(der));
        CFW_TEST_ASSERT(der[0] == 0x30u);

        /* Sizing query: too-small buffer reports the required size. */
        CFW_TEST_ASSERT(cfw_tls_conn_peer_cert_der(server, der, 1,
                                                   &need) ==
                        CFW_ERROR_OVERFLOW);
        CFW_TEST_ASSERT(need == len);

        CFW_TEST_ASSERT(cfw_tls_conn_peer_cert_der(client, der,
                                                   sizeof(der),
                                                   &len) == CFW_OK);
        CFW_TEST_ASSERT(len > 200u && len <= sizeof(der));
        CFW_TEST_ASSERT(der[0] == 0x30u);
    }
    rc = 0;
    tls_teardown(server, client);
    cfw_tls_ctx_destroy(sctx);
    cfw_tls_ctx_destroy(cctx);
    return rc;
}

/* Without a peer certificate the accessor reports an empty result
 * rather than an error. */
static int test_tls_peer_cert_absent(void)
{
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_conn_t *server = NULL;
    cfw_tls_conn_t *client = NULL;
    unsigned char der[16];
    size_t len = 1;
    int rc = 1;

    CFW_TEST_ASSERT(tls_ctx_server(&sctx) == 0);
    CFW_TEST_ASSERT(tls_ctx_client_alpn(NULL, &cctx) == 0);
    CFW_TEST_ASSERT(tls_pair(sctx, cctx, &server, &client) == 0);
    CFW_TEST_ASSERT(tls_drive(server, client) == 0);

    /* Server did not request a client cert: no peer certificate. */
    CFW_TEST_ASSERT(cfw_tls_conn_peer_cert_der(server, der,
                                               sizeof(der),
                                               &len) == CFW_OK);
    CFW_TEST_ASSERT(len == 0u);
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

/* ctx helpers carrying an ALPN list; same file-path handling as
 * make_ctx. */
static int tls_ctx_server_alpn(const char * const *protos,
                               cfw_tls_ctx_t **ctx)
{
    cfw_tls_ctx_options_t o;
    cfw_status_t st;
    char cert[512];
    char key[512];

    memset(&o, 0, sizeof(o));
    o.role = CFW_TLS_SERVER;
    tls_path(cert, sizeof(cert), "server.crt");
    tls_path(key, sizeof(key), "server.key");
    o.cert_file = cert;
    o.key_file = key;
    o.alpn_protos = protos;
    st = cfw_tls_ctx_create(&o, ctx);
    return st == CFW_OK ? 0 : 1;
}

static int tls_ctx_client_alpn(const char * const *protos,
                               cfw_tls_ctx_t **ctx)
{
    cfw_tls_ctx_options_t o;
    cfw_status_t st;

    memset(&o, 0, sizeof(o));
    o.role = CFW_TLS_CLIENT;
    o.alpn_protos = protos;
    st = cfw_tls_ctx_create(&o, ctx);
    return st == CFW_OK ? 0 : 1;
}

static int tls_alpn_expect(cfw_tls_conn_t *conn, const char *want)
{
    cfw_strview_t proto;
    CFW_TEST_ASSERT(cfw_tls_conn_alpn(conn, &proto) == CFW_OK);
    if (want == NULL) {
        CFW_TEST_ASSERT(proto.data == NULL && proto.size == 0);
        return 0;
    }
    CFW_TEST_ASSERT(proto.size == strlen(want));
    CFW_TEST_ASSERT(memcmp(proto.data, want, proto.size) == 0);
    return 0;
}

/* Server preference order wins: server lists "h2" first and the
 * client offers both, so "h2" is selected on both ends. */
static int test_tls_alpn_negotiated(void)
{
    static const char * const server_protos[] = {"h2", "http/1.1", NULL};
    static const char * const client_protos[] = {"http/1.1", "h2", NULL};
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_conn_t *server = NULL;
    cfw_tls_conn_t *client = NULL;
    int rc = 1;

    CFW_TEST_ASSERT(tls_ctx_server_alpn(server_protos, &sctx) == 0);
    CFW_TEST_ASSERT(tls_ctx_client_alpn(client_protos, &cctx) == 0);
    CFW_TEST_ASSERT(tls_pair(sctx, cctx, &server, &client) == 0);
    CFW_TEST_ASSERT(tls_drive(server, client) == 0);
    tls_alpn_expect(server, "h2");
    tls_alpn_expect(client, "h2");
    rc = 0;

    tls_teardown(server, client);
    cfw_tls_ctx_destroy(sctx);
    cfw_tls_ctx_destroy(cctx);
    return rc;
}

/* Client offers ALPN but nothing matches the server list: handshake
 * still succeeds and no protocol is negotiated (NOACK policy). */
static int test_tls_alpn_no_match(void)
{
    static const char * const server_protos[] = {"h2", NULL};
    static const char * const client_protos[] = {"http/1.1", NULL};
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_conn_t *server = NULL;
    cfw_tls_conn_t *client = NULL;
    int rc = 1;

    CFW_TEST_ASSERT(tls_ctx_server_alpn(server_protos, &sctx) == 0);
    CFW_TEST_ASSERT(tls_ctx_client_alpn(client_protos, &cctx) == 0);
    CFW_TEST_ASSERT(tls_pair(sctx, cctx, &server, &client) == 0);
    CFW_TEST_ASSERT(tls_drive(server, client) == 0);
    tls_alpn_expect(server, NULL);
    tls_alpn_expect(client, NULL);
    rc = 0;

    tls_teardown(server, client);
    cfw_tls_ctx_destroy(sctx);
    cfw_tls_ctx_destroy(cctx);
    return rc;
}

/* ALPN configured only on the server: the client never sends the
 * extension, so nothing is negotiated. */
static int test_tls_alpn_server_only(void)
{
    static const char * const server_protos[] = {"http/1.1", NULL};
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_conn_t *server = NULL;
    cfw_tls_conn_t *client = NULL;
    int rc = 1;

    CFW_TEST_ASSERT(tls_ctx_server_alpn(server_protos, &sctx) == 0);
    CFW_TEST_ASSERT(tls_ctx_client_alpn(NULL, &cctx) == 0);
    CFW_TEST_ASSERT(tls_pair(sctx, cctx, &server, &client) == 0);
    CFW_TEST_ASSERT(tls_drive(server, client) == 0);
    tls_alpn_expect(server, NULL);
    tls_alpn_expect(client, NULL);
    rc = 0;

    tls_teardown(server, client);
    cfw_tls_ctx_destroy(sctx);
    cfw_tls_ctx_destroy(cctx);
    return rc;
}

static int tls_ctx_client_alpn_raw(const char * const *protos,
                                   cfw_status_t *out)
{
    cfw_tls_ctx_options_t o;
    cfw_tls_ctx_t *ctx = NULL;
    cfw_status_t st;

    memset(&o, 0, sizeof(o));
    o.role = CFW_TLS_CLIENT;
    o.alpn_protos = protos;
    st = cfw_tls_ctx_create(&o, &ctx);
    if (st == CFW_OK) {
        cfw_tls_ctx_destroy(ctx);
    }
    if (out != NULL) {
        *out = st;
    }
    return st == CFW_OK ? 0 : 1;
}

static int test_tls_alpn_invalid_args(void)
{
    static const char * const empty[] = {"", NULL};
    static const char * const null_only[] = {NULL};
    char long_proto[257];
    const char * const long_list[] = {long_proto, NULL};
    cfw_status_t st;

    memset(long_proto, 'a', sizeof(long_proto) - 1);
    long_proto[sizeof(long_proto) - 1] = '\0';

    /* Empty identifier, overlong identifier, and >64KiB total are
     * rejected; a NULL-terminated-but-empty list is just "disabled". */
    CFW_TEST_ASSERT(tls_ctx_client_alpn_raw(empty, &st) == 1);
    CFW_TEST_ASSERT(st == CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT(tls_ctx_client_alpn_raw(long_list, &st) == 1);
    CFW_TEST_ASSERT(st == CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT(tls_ctx_client_alpn_raw(null_only, &st) == 0);
    CFW_TEST_ASSERT(st == CFW_OK);
    return 0;
}

/* Full-resumption round trip: handshake once, serialize the client
 * session, install it on a fresh connection, and verify the second
 * handshake reports session_reused. Also covers the size-then-fetch
 * overflow contract and argument validation. */
static int test_tls_session_resumption(void)
{
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_conn_t *server = NULL;
    cfw_tls_conn_t *client = NULL;
    cfw_tls_conn_t *server2 = NULL;
    cfw_tls_conn_t *client2 = NULL;
    unsigned char session[4096];
    size_t session_len = 0;
    size_t need = 0;
    int rc = 1;

    CFW_TEST_ASSERT(tls_ctx_server(&sctx) == 0);
    CFW_TEST_ASSERT(tls_ctx_client_alpn(NULL, &cctx) == 0);

    /* No session exists before the handshake completes. */
    CFW_TEST_ASSERT(tls_pair(sctx, cctx, &server, &client) == 0);
    CFW_TEST_ASSERT(cfw_tls_conn_get_session(client, session,
                                             sizeof(session),
                                             &session_len) == CFW_OK);
    CFW_TEST_ASSERT(session_len == 0);
    CFW_TEST_ASSERT(cfw_tls_conn_session_reused(client) == 0);
    CFW_TEST_ASSERT(tls_drive(server, client) == 0);
    CFW_TEST_ASSERT(cfw_tls_conn_session_reused(client) == 0);
    CFW_TEST_ASSERT(cfw_tls_conn_session_reused(server) == 0);

    /* TLS 1.3 ships session tickets after the handshake; pump I/O so
     * the client processes them and its session becomes resumable. */
    {
        unsigned char tmp[16];
        size_t done = 0;
        int i;
        int progress = 1;
        for (i = 0; i < 1000 && progress; i++) {
            progress = 0;
            if (cfw_tls_read(client, tmp, sizeof(tmp), &done) ==
                CFW_OK) {
                progress = 1;
            }
            if (cfw_tls_read(server, tmp, sizeof(tmp), &done) ==
                CFW_OK) {
                progress = 1;
            }
        }
    }

    /* Argument validation on the accessors. */
    CFW_TEST_ASSERT(cfw_tls_conn_get_session(NULL, session,
                                             sizeof(session),
                                             &session_len) ==
                    CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT(cfw_tls_conn_get_session(client, NULL, 16,
                                             &session_len) ==
                    CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT(cfw_tls_conn_set_session(client, session, 1) ==
                    CFW_ERROR_INVALID_ARGUMENT);

    /* Size-then-fetch: undersized buffer reports the required size. */
    CFW_TEST_ASSERT(cfw_tls_conn_get_session(client, session, 1,
                                             &need) ==
                    CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT(need > 1 && need <= sizeof(session));
    CFW_TEST_ASSERT(cfw_tls_conn_get_session(client, session,
                                             sizeof(session),
                                             &session_len) == CFW_OK);
    CFW_TEST_ASSERT(session_len == need);

    tls_teardown(server, client);

    /* Second connection on the same ctxs: install the session before
     * the handshake and expect resumption. */
    CFW_TEST_ASSERT(tls_pair(sctx, cctx, &server2, &client2) == 0);
    CFW_TEST_ASSERT(cfw_tls_conn_set_session(client2, session,
                                             session_len) == CFW_OK);
    CFW_TEST_ASSERT(tls_drive(server2, client2) == 0);
    CFW_TEST_ASSERT(cfw_tls_conn_session_reused(client2) == 1);
    CFW_TEST_ASSERT(cfw_tls_conn_session_reused(server2) == 1);
    rc = 0;

    tls_teardown(server2, client2);
    cfw_tls_ctx_destroy(sctx);
    cfw_tls_ctx_destroy(cctx);
    return rc;
}

/* OpenSSL's error queue is per-thread: errors left by one connection
 * (e.g. an abrupt peer close) must not be inherited by the next
 * connection serviced on the same thread — a stale queue entry makes
 * SSL_get_error report SSL_ERROR_SSL for unrelated transient results
 * and kills healthy connections. Regression test for the
 * ERR_clear_error hygiene in tls.c. */
/* Variant of tls_pair that reports the borrowed fds so several
 * pairs can coexist in one test. */
static int tls_pair_tracked(cfw_tls_ctx_t *sctx, cfw_tls_ctx_t *cctx,
                            cfw_tls_conn_t **server_out,
                            cfw_tls_conn_t **client_out,
                            int fds_out[2])
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

    fds_out[0] = fds[0];
    fds_out[1] = fds[1];
    *server_out = server;
    *client_out = client;
    return 0;
}

static void tls_pair_tracked_teardown(cfw_tls_conn_t *server,
                                      cfw_tls_conn_t *client,
                                      const int fds[2])
{
    cfw_tls_conn_destroy(server);
    cfw_tls_conn_destroy(client);
    if (fds[0] >= 0) {
        close(fds[0]);
    }
    if (fds[1] >= 0) {
        close(fds[1]);
    }
}

static int test_tls_error_queue_isolation(void)
{
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_conn_t *server = NULL;
    cfw_tls_conn_t *client = NULL;
    cfw_tls_conn_t *server2 = NULL;
    cfw_tls_conn_t *client2 = NULL;
    int fds1[2] = {-1, -1};
    int fds2[2] = {-1, -1};
    unsigned char buf[8];
    size_t done = 0;
    cfw_status_t st;
    int rc = 1;

    CFW_TEST_ASSERT(tls_ctx_server(&sctx) == 0);
    CFW_TEST_ASSERT(tls_ctx_client_alpn(NULL, &cctx) == 0);

    /* Two connections on the same thread, both handshaked while the
     * error queue is clean. */
    CFW_TEST_ASSERT(tls_pair_tracked(sctx, cctx, &server, &client,
                                     fds1) == 0);
    CFW_TEST_ASSERT(tls_drive(server, client) == 0);
    CFW_TEST_ASSERT(tls_pair_tracked(sctx, cctx, &server2, &client2,
                                     fds2) == 0);
    CFW_TEST_ASSERT(tls_drive(server2, client2) == 0);

    /* Connection 1's client vanishes without close_notify; the
     * server's SSL_read records a genuine TLS error (unexpected eof)
     * in this thread's error queue. */
    cfw_tls_conn_destroy(client);
    client = NULL;
    close(fds1[1]);
    fds1[1] = -1;
    st = cfw_tls_read(server, buf, sizeof(buf), &done);
    CFW_TEST_ASSERT(st == CFW_ERROR_IO);
    cfw_tls_conn_destroy(server);
    server = NULL;
    close(fds1[0]);
    fds1[0] = -1;

    /* Connection 2's client read must not inherit the stale queued
     * error. TLS 1.3 session tickets arrive post-handshake; the
     * client's SSL_read consumes them and then reports
     * WOULDBLOCK for app data — not IO from the stale queue. */
    done = 1;
    st = cfw_tls_read(client2, buf, sizeof(buf), &done);
    CFW_TEST_ASSERT(st == CFW_ERROR_WOULDBLOCK);
    rc = 0;

    tls_pair_tracked_teardown(server2, client2, fds2);
    cfw_tls_ctx_destroy(sctx);
    cfw_tls_ctx_destroy(cctx);
    return rc;
}

/* Client ctx trusting the test CA and requiring peer verification —
 * the shape needed for hostname-check tests. */
static int tls_ctx_client_verify(cfw_tls_ctx_t **ctx)
{
    cfw_tls_ctx_options_t o;
    char ca[512];

    tls_path(ca, sizeof(ca), "ca.crt");
    memset(&o, 0, sizeof(o));
    o.role = CFW_TLS_CLIENT;
    o.ca_file = ca;
    o.verify_peer = 1;
    return cfw_tls_ctx_create(&o, ctx) == CFW_OK ? 0 : 1;
}

/* The test server certificate has CN=localhost and no SANs; OpenSSL's
 * X509_check_host falls back to CN, so "localhost" verifies and any
 * other name must fail the handshake. */
static int test_tls_server_name_accepted(void)
{
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_conn_t *server = NULL;
    cfw_tls_conn_t *client = NULL;
    int rc = 1;

    CFW_TEST_ASSERT(tls_ctx_server(&sctx) == 0);
    CFW_TEST_ASSERT(tls_ctx_client_verify(&cctx) == 0);
    CFW_TEST_ASSERT(tls_pair(sctx, cctx, &server, &client) == 0);
    CFW_TEST_ASSERT(cfw_tls_conn_set_server_name(client, "localhost")
                    == CFW_OK);
    CFW_TEST_ASSERT(tls_drive(server, client) == 0);
    rc = 0;
    tls_teardown(server, client);
    cfw_tls_ctx_destroy(sctx);
    cfw_tls_ctx_destroy(cctx);
    return rc;
}

static int test_tls_server_name_rejected(void)
{
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_conn_t *server = NULL;
    cfw_tls_conn_t *client = NULL;
    int client_failed = 0;
    int i;
    int rc = 1;

    CFW_TEST_ASSERT(tls_ctx_server(&sctx) == 0);
    CFW_TEST_ASSERT(tls_ctx_client_verify(&cctx) == 0);
    CFW_TEST_ASSERT(tls_pair(sctx, cctx, &server, &client) == 0);
    CFW_TEST_ASSERT(cfw_tls_conn_set_server_name(client,
                                                  "wrong.example.com")
                    == CFW_OK);
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
    }
    CFW_TEST_ASSERT(client_failed);
    rc = 0;
    tls_teardown(server, client);
    cfw_tls_ctx_destroy(sctx);
    cfw_tls_ctx_destroy(cctx);
    return rc;
}

static int test_tls_server_name_invalid_args(void)
{
    cfw_tls_ctx_t *sctx = NULL;
    cfw_tls_ctx_t *cctx = NULL;
    cfw_tls_conn_t *server = NULL;
    cfw_tls_conn_t *client = NULL;
    char long_name[300];
    cfw_status_t st;
    int rc = 1;

    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';

    CFW_TEST_ASSERT(tls_ctx_server(&sctx) == 0);
    CFW_TEST_ASSERT(tls_ctx_client_alpn(NULL, &cctx) == 0);

    st = cfw_tls_conn_set_server_name(NULL, "localhost");
    CFW_TEST_ASSERT(st == CFW_ERROR_INVALID_ARGUMENT);
    st = cfw_tls_conn_set_server_name(NULL, NULL);
    CFW_TEST_ASSERT(st == CFW_ERROR_INVALID_ARGUMENT);

    CFW_TEST_ASSERT(tls_pair(sctx, cctx, &server, &client) == 0);

    /* Server-side connections reject the call. */
    st = cfw_tls_conn_set_server_name(server, "localhost");
    CFW_TEST_ASSERT(st == CFW_ERROR_INVALID_ARGUMENT);
    /* Malformed names. */
    st = cfw_tls_conn_set_server_name(client, "");
    CFW_TEST_ASSERT(st == CFW_ERROR_INVALID_ARGUMENT);
    st = cfw_tls_conn_set_server_name(client, long_name);
    CFW_TEST_ASSERT(st == CFW_ERROR_INVALID_ARGUMENT);
    st = cfw_tls_conn_set_server_name(client, NULL);
    CFW_TEST_ASSERT(st == CFW_ERROR_INVALID_ARGUMENT);

    /* Once the handshake has started the name is fixed — a single
     * WOULDBLOCK handshake call is enough to start it. */
    (void)cfw_tls_handshake(client);
    st = cfw_tls_conn_set_server_name(client, "localhost");
    CFW_TEST_ASSERT(st == CFW_ERROR_INVALID_ARGUMENT);

    rc = 0;
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
    {"tls_peer_cert_absent", test_tls_peer_cert_absent},
    {"tls_shutdown_and_eof", test_tls_shutdown_and_eof},
    {"tls_alpn_negotiated", test_tls_alpn_negotiated},
    {"tls_alpn_no_match", test_tls_alpn_no_match},
    {"tls_alpn_server_only", test_tls_alpn_server_only},
    {"tls_alpn_invalid_args", test_tls_alpn_invalid_args},
    {"tls_session_resumption", test_tls_session_resumption},
    {"tls_error_queue_isolation", test_tls_error_queue_isolation},
    {"tls_server_name_accepted", test_tls_server_name_accepted},
    {"tls_server_name_rejected", test_tls_server_name_rejected},
    {"tls_server_name_invalid_args", test_tls_server_name_invalid_args},
#endif
    {NULL, NULL},
};
