/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/tls.h>

#if CFW_HAVE_TLS

    #include <openssl/err.h>
    #include <openssl/ssl.h>

    #include <stdint.h>
    #include <stdlib.h>
    #include <string.h>

struct cfw_tls_ctx {
    SSL_CTX *ssl_ctx;
    cfw_tls_role_t role;
    int allow_moving_write_buffer;
};

struct cfw_tls_conn {
    SSL *ssl;
    intptr_t fd;
    int want_read;
    int want_write;
};

static void cfw_tls_clear_want(cfw_tls_conn_t *conn)
{
    conn->want_read = 0;
    conn->want_write = 0;
}

/* Maps an SSL failure to a cfw_status_t and records direction hints.
 * `ret` is the value returned by the SSL operation. */
static cfw_status_t cfw_tls_map_error(cfw_tls_conn_t *conn, int ret)
{
    int ssl_err;

    cfw_tls_clear_want(conn);
    if (ret > 0) {
        return CFW_OK;
    }

    ssl_err = SSL_get_error(conn->ssl, ret);
    switch (ssl_err) {
    case SSL_ERROR_WANT_READ:
        conn->want_read = 1;
        return CFW_ERROR_WOULDBLOCK;
    case SSL_ERROR_WANT_WRITE:
        conn->want_write = 1;
        return CFW_ERROR_WOULDBLOCK;
    case SSL_ERROR_ZERO_RETURN:
        /* Orderly close_notify from the peer: not an error for read
         * paths (handled by callers); treat as IO here. */
        return CFW_ERROR_IO;
    case SSL_ERROR_WANT_CONNECT:
    case SSL_ERROR_WANT_ACCEPT:
        conn->want_read = 1;
        conn->want_write = 1;
        return CFW_ERROR_WOULDBLOCK;
    default:
        return CFW_ERROR_IO;
    }
}

cfw_status_t cfw_tls_ctx_create(const cfw_tls_ctx_options_t *options,
                                cfw_tls_ctx_t **ctx)
{
    cfw_tls_ctx_t *created;
    SSL_CTX *ssl_ctx;
    const SSL_METHOD *method;
    int have_cert;

    if (options == NULL || ctx == NULL ||
        (options->role != CFW_TLS_SERVER &&
         options->role != CFW_TLS_CLIENT)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (options->role == CFW_TLS_SERVER &&
        (options->cert_file == NULL || options->key_file == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (options->verify_peer && options->ca_file == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    method = options->role == CFW_TLS_SERVER ? TLS_server_method()
                                             : TLS_client_method();
    if (method == NULL) {
        return CFW_ERROR_UNAVAILABLE;
    }
    ssl_ctx = SSL_CTX_new(method);
    if (ssl_ctx == NULL) {
        return CFW_ERROR_NOMEM;
    }

    /* TLS 1.2 floor: the framework's security posture does not accept
     * protocol versions with known plaintext-recovery weaknesses. */
    SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION);

    have_cert = options->cert_file != NULL;
    if (have_cert) {
        if (SSL_CTX_use_certificate_chain_file(ssl_ctx,
                                               options->cert_file) != 1 ||
            SSL_CTX_use_PrivateKey_file(ssl_ctx, options->key_file,
                                        SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(ssl_ctx) != 1) {
            SSL_CTX_free(ssl_ctx);
            return CFW_ERROR_IO;
        }
    }

    if (options->ca_file != NULL &&
        SSL_CTX_load_verify_locations(ssl_ctx, options->ca_file,
                                      NULL) != 1) {
        SSL_CTX_free(ssl_ctx);
        return CFW_ERROR_IO;
    }

    if (options->role == CFW_TLS_SERVER) {
        /* Session ids are required before server session reuse; a
         * fixed application context is sufficient for a single
         * listener configuration. */
        static const unsigned char session_ctx[] = "cfw";
        SSL_CTX_set_session_id_context(
            ssl_ctx, session_ctx, (unsigned int)sizeof(session_ctx));
        if (options->verify_peer) {
            SSL_CTX_set_verify(ssl_ctx,
                               SSL_VERIFY_PEER |
                                   SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                               NULL);
        }
    } else if (options->verify_peer) {
        SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, NULL);
    }

    created = (cfw_tls_ctx_t *)calloc(1, sizeof(*created));
    if (created == NULL) {
        SSL_CTX_free(ssl_ctx);
        return CFW_ERROR_NOMEM;
    }
    created->ssl_ctx = ssl_ctx;
    created->role = options->role;
    created->allow_moving_write_buffer =
        options->allow_moving_write_buffer != 0;
    *ctx = created;
    return CFW_OK;
}

void cfw_tls_ctx_destroy(cfw_tls_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->ssl_ctx != NULL) {
        SSL_CTX_free(ctx->ssl_ctx);
    }
    free(ctx);
}

cfw_status_t cfw_tls_conn_create(cfw_tls_ctx_t *ctx, intptr_t fd,
                                 cfw_tls_conn_t **conn)
{
    cfw_tls_conn_t *created;
    SSL *ssl;
    long modes;

    if (ctx == NULL || conn == NULL || fd < 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    ssl = SSL_new(ctx->ssl_ctx);
    if (ssl == NULL) {
        return CFW_ERROR_NOMEM;
    }

    /* Nonblocking partial-write semantics: SSL_write may report fewer
     * bytes than requested. ACCEPT_MOVING_WRITE_BUFFER additionally
     * lets the caller modify/reuse the buffer between WOULDBLOCK
     * retries; it is opt-in because the default contract requires the
     * buffer to remain unchanged until the bytes are accepted. */
    modes = SSL_MODE_ENABLE_PARTIAL_WRITE;
    if (ctx->allow_moving_write_buffer) {
        modes |= SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER;
    }
    SSL_set_mode(ssl, modes);

    created = (cfw_tls_conn_t *)calloc(1, sizeof(*created));
    if (created == NULL) {
        SSL_free(ssl);
        return CFW_ERROR_NOMEM;
    }
    created->fd = fd;
    created->ssl = ssl;
    SSL_set_fd(ssl, (int)created->fd);
    if (ctx->role == CFW_TLS_SERVER) {
        SSL_set_accept_state(ssl);
    } else {
        SSL_set_connect_state(ssl);
    }

    *conn = created;
    return CFW_OK;
}

void cfw_tls_conn_destroy(cfw_tls_conn_t *conn)
{
    if (conn == NULL) {
        return;
    }
    if (conn->ssl != NULL) {
        SSL_free(conn->ssl);
    }
    /* The descriptor is borrowed: the caller owns and closes it. */
    free(conn);
}

intptr_t cfw_tls_conn_fd(const cfw_tls_conn_t *conn)
{
    if (conn == NULL) {
        return -1;
    }
    return conn->fd;
}

cfw_status_t cfw_tls_handshake(cfw_tls_conn_t *conn)
{
    int ret;

    if (conn == NULL || conn->ssl == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (SSL_is_init_finished(conn->ssl)) {
        cfw_tls_clear_want(conn);
        return CFW_OK;
    }
    ret = SSL_do_handshake(conn->ssl);
    if (ret == 1) {
        cfw_tls_clear_want(conn);
        return CFW_OK;
    }
    return cfw_tls_map_error(conn, ret);
}

int cfw_tls_handshake_done(const cfw_tls_conn_t *conn)
{
    return conn != NULL && conn->ssl != NULL &&
           SSL_is_init_finished(conn->ssl);
}

cfw_status_t cfw_tls_read(cfw_tls_conn_t *conn, void *buf, size_t size,
                          size_t *done)
{
    int ret;

    if (conn == NULL || buf == NULL || done == NULL || size == 0 ||
        size > INT32_MAX || conn->fd < 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    ret = SSL_read(conn->ssl, buf, (int)size);
    if (ret > 0) {
        *done = (size_t)ret;
        cfw_tls_clear_want(conn);
        return CFW_OK;
    }

    {
        int ssl_err = SSL_get_error(conn->ssl, ret);
        if (ssl_err == SSL_ERROR_ZERO_RETURN) {
            /* Orderly close_notify: EOF, not an error. */
            *done = 0;
            cfw_tls_clear_want(conn);
            return CFW_OK;
        }
    }
    {
        cfw_status_t st = cfw_tls_map_error(conn, ret);
        *done = 0;
        return st;
    }
}

cfw_status_t cfw_tls_write(cfw_tls_conn_t *conn, const void *data,
                           size_t size, size_t *done)
{
    int ret;

    if (conn == NULL || data == NULL || done == NULL || size == 0 ||
        size > INT32_MAX || conn->fd < 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    ret = SSL_write(conn->ssl, data, (int)size);
    if (ret > 0) {
        *done = (size_t)ret;
        cfw_tls_clear_want(conn);
        return CFW_OK;
    }
    {
        cfw_status_t st = cfw_tls_map_error(conn, ret);
        *done = 0;
        return st;
    }
}

cfw_status_t cfw_tls_shutdown(cfw_tls_conn_t *conn)
{
    int ret;

    if (conn == NULL || conn->ssl == NULL ||
        conn->fd < 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    ret = SSL_shutdown(conn->ssl);
    if (ret == 1) {
        cfw_tls_clear_want(conn);
        return CFW_OK;
    }
    if (ret == 0) {
        /* Our close_notify was sent but the peer's has not arrived
         * yet; SSL_shutdown must be called again once the connection
         * is readable. */
        cfw_tls_clear_want(conn);
        conn->want_read = 1;
        return CFW_ERROR_WOULDBLOCK;
    }
    return cfw_tls_map_error(conn, ret);
}

int cfw_tls_conn_want_read(const cfw_tls_conn_t *conn)
{
    return conn != NULL && conn->want_read;
}

int cfw_tls_conn_want_write(const cfw_tls_conn_t *conn)
{
    return conn != NULL && conn->want_write;
}

size_t cfw_tls_pending(const cfw_tls_conn_t *conn)
{
    if (conn == NULL || conn->ssl == NULL) {
        return 0;
    }
    return (size_t)SSL_pending(conn->ssl);
}

#else /* !CFW_HAVE_TLS — stubs (ADR 0057) */

cfw_status_t cfw_tls_ctx_create(const cfw_tls_ctx_options_t *options,
                                cfw_tls_ctx_t **ctx)
{
    (void)options;
    if (ctx != NULL) {
        *ctx = NULL;
    }
    return CFW_ERROR_UNAVAILABLE;
}

void cfw_tls_ctx_destroy(cfw_tls_ctx_t *ctx)
{
    (void)ctx;
}

cfw_status_t cfw_tls_conn_create(cfw_tls_ctx_t *ctx, intptr_t fd,
                                 cfw_tls_conn_t **conn)
{
    (void)ctx;
    (void)fd;
    if (conn != NULL) {
        *conn = NULL;
    }
    return CFW_ERROR_UNAVAILABLE;
}

void cfw_tls_conn_destroy(cfw_tls_conn_t *conn)
{
    (void)conn;
}

intptr_t cfw_tls_conn_fd(const cfw_tls_conn_t *conn)
{
    (void)conn;
    return -1;
}

cfw_status_t cfw_tls_handshake(cfw_tls_conn_t *conn)
{
    (void)conn;
    return CFW_ERROR_UNAVAILABLE;
}

int cfw_tls_handshake_done(const cfw_tls_conn_t *conn)
{
    (void)conn;
    return 0;
}

cfw_status_t cfw_tls_read(cfw_tls_conn_t *conn, void *buf, size_t size,
                          size_t *done)
{
    (void)conn;
    (void)buf;
    (void)size;
    if (done != NULL) {
        *done = 0;
    }
    return CFW_ERROR_UNAVAILABLE;
}

cfw_status_t cfw_tls_write(cfw_tls_conn_t *conn, const void *data,
                           size_t size, size_t *done)
{
    (void)conn;
    (void)data;
    (void)size;
    if (done != NULL) {
        *done = 0;
    }
    return CFW_ERROR_UNAVAILABLE;
}

cfw_status_t cfw_tls_shutdown(cfw_tls_conn_t *conn)
{
    (void)conn;
    return CFW_ERROR_UNAVAILABLE;
}

int cfw_tls_conn_want_read(const cfw_tls_conn_t *conn)
{
    (void)conn;
    return 0;
}

int cfw_tls_conn_want_write(const cfw_tls_conn_t *conn)
{
    (void)conn;
    return 0;
}

size_t cfw_tls_pending(const cfw_tls_conn_t *conn)
{
    (void)conn;
    return 0;
}

#endif /* CFW_HAVE_TLS */
