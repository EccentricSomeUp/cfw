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
    /* Wire-encoded ALPN protocol list ([len][bytes]...); NULL when
     * ALPN is disabled. Owned by the ctx. */
    unsigned char *alpn_wire;
    size_t alpn_wire_len;
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

/* Encodes a NULL-terminated ALPN identifier list into the wire
 * format ([len][bytes] per entry, total <= 65535). Returns NULL with
 * *out_len 0 when the list is NULL/empty; CFW_ERROR_INVALID_ARGUMENT
 * for malformed entries; CFW_ERROR_OVERFLOW when the wire form does
 * not fit. */
static cfw_status_t cfw_tls_encode_alpn(const char * const *protos,
                                        unsigned char **out,
                                        size_t *out_len)
{
    size_t count = 0;
    size_t total = 0;
    size_t i;
    unsigned char *wire;
    unsigned char *w;

    *out = NULL;
    *out_len = 0;
    if (protos == NULL) {
        return CFW_OK;
    }
    for (i = 0; protos[i] != NULL; i++) {
        size_t len = strlen(protos[i]);
        if (len == 0u || len > 255u) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        count++;
        total += 1u + len;
        if (total > 65535u) {
            return CFW_ERROR_OVERFLOW;
        }
    }
    if (count == 0u) {
        return CFW_OK;
    }
    wire = (unsigned char *)malloc(total);
    if (wire == NULL) {
        return CFW_ERROR_NOMEM;
    }
    w = wire;
    for (i = 0; protos[i] != NULL; i++) {
        size_t len = strlen(protos[i]);
        *w++ = (unsigned char)len;
        memcpy(w, protos[i], len);
        w += len;
    }
    *out = wire;
    *out_len = total;
    return CFW_OK;
}

/* Server-side ALPN selection callback (ADR 0060): server preference
 * order — the first configured protocol the client also offered
 * wins. Returns SSL_TLSEXT_ERR_NOACK when nothing matches so the
 * handshake continues without ALPN rather than failing. */
static int cfw_tls_alpn_select(SSL *ssl, const unsigned char **out,
                               unsigned char *outlen,
                               const unsigned char *in,
                               unsigned int inlen, void *arg)
{
    cfw_tls_ctx_t *ctx = (cfw_tls_ctx_t *)arg;
    const unsigned char *sp = ctx->alpn_wire;
    const unsigned char *send = ctx->alpn_wire + ctx->alpn_wire_len;

    (void)ssl;
    while (sp < send) {
        unsigned int slen = *sp++;
        const unsigned char *sproto = sp;
        const unsigned char *cp = in;
        const unsigned char *cin_end = in + inlen;

        sp += slen;
        while (cp < cin_end) {
            unsigned int clen = *cp++;
            if ((size_t)(cin_end - cp) < clen) {
                break; /* malformed client list: stop scanning */
            }
            if (clen == slen && memcmp(cp, sproto, slen) == 0) {
                *out = sproto;
                *outlen = (unsigned char)slen;
                return SSL_TLSEXT_ERR_OK;
            }
            cp += clen;
        }
    }
    return SSL_TLSEXT_ERR_NOACK;
}

cfw_status_t cfw_tls_ctx_create(const cfw_tls_ctx_options_t *options,
                                cfw_tls_ctx_t **ctx)
{
    cfw_tls_ctx_t *created;
    SSL_CTX *ssl_ctx;
    const SSL_METHOD *method;
    int have_cert;
    cfw_status_t st;

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

    /* Allocate the wrapper early: the ALPN select callback needs it
     * as its argument, so it must exist before any handshake. */
    created = (cfw_tls_ctx_t *)calloc(1, sizeof(*created));
    if (created == NULL) {
        SSL_CTX_free(ssl_ctx);
        return CFW_ERROR_NOMEM;
    }
    created->ssl_ctx = ssl_ctx;
    created->role = options->role;
    created->allow_moving_write_buffer =
        options->allow_moving_write_buffer != 0;

    st = cfw_tls_encode_alpn(options->alpn_protos, &created->alpn_wire,
                             &created->alpn_wire_len);
    if (st != CFW_OK) {
        goto fail;
    }
    if (options->role == CFW_TLS_SERVER && created->alpn_wire != NULL) {
        SSL_CTX_set_alpn_select_cb(ssl_ctx, cfw_tls_alpn_select, created);
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
            st = CFW_ERROR_IO;
            goto fail;
        }
    }

    if (options->ca_file != NULL &&
        SSL_CTX_load_verify_locations(ssl_ctx, options->ca_file,
                                      NULL) != 1) {
        st = CFW_ERROR_IO;
        goto fail;
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

    *ctx = created;
    return CFW_OK;

fail:
    free(created->alpn_wire);
    SSL_CTX_free(ssl_ctx);
    free(created);
    return st;
}

void cfw_tls_ctx_destroy(cfw_tls_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->ssl_ctx != NULL) {
        SSL_CTX_free(ctx->ssl_ctx);
    }
    free(ctx->alpn_wire);
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
        if (ctx->alpn_wire != NULL) {
            SSL_set_alpn_protos(ssl, ctx->alpn_wire,
                                (unsigned int)ctx->alpn_wire_len);
        }
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
    /* OpenSSL's error queue is per-thread, not per-SSL: stale errors
     * left by earlier operations (including other connections on this
     * thread) make SSL_get_error report SSL_ERROR_SSL for unrelated
     * transient results. Drain it so each call classifies only its
     * own outcome. */
    ERR_clear_error();
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

    ERR_clear_error();
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

    ERR_clear_error();
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
    ERR_clear_error();
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

cfw_status_t cfw_tls_conn_alpn(const cfw_tls_conn_t *conn,
                               cfw_strview_t *proto)
{
    const unsigned char *data = NULL;
    unsigned int len = 0;

    if (conn == NULL || conn->ssl == NULL || proto == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    proto->data = NULL;
    proto->size = 0;
    SSL_get0_alpn_selected(conn->ssl, &data, &len);
    if (data == NULL || len == 0u) {
        return CFW_OK;
    }
    proto->data = (const char *)data;
    proto->size = (size_t)len;
    return CFW_OK;
}

cfw_status_t cfw_tls_conn_peer_cert_der(const cfw_tls_conn_t *conn,
                                        unsigned char *out, size_t cap,
                                        size_t *written)
{
    X509 *cert;
    int len;
    unsigned char *p;

    if (conn == NULL || conn->ssl == NULL || written == NULL ||
        (cap != 0 && out == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    *written = 0;
    cert = SSL_get1_peer_certificate(conn->ssl);
    if (cert == NULL) {
        return CFW_OK;
    }
    len = i2d_X509(cert, NULL);
    if (len <= 0) {
        X509_free(cert);
        return CFW_ERROR_IO;
    }
    if ((size_t)len > cap) {
        *written = (size_t)len;
        X509_free(cert);
        return CFW_ERROR_OVERFLOW;
    }
    p = out;
    if (i2d_X509(cert, &p) != len) {
        X509_free(cert);
        return CFW_ERROR_IO;
    }
    X509_free(cert);
    *written = (size_t)len;
    return CFW_OK;
}

cfw_status_t cfw_tls_conn_get_session(const cfw_tls_conn_t *conn,
                                      unsigned char *out, size_t cap,
                                      size_t *written)
{
    SSL_SESSION *sess;
    unsigned char *p;
    int len;

    if (conn == NULL || conn->ssl == NULL || written == NULL ||
        (cap != 0 && out == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    *written = 0;
    sess = SSL_get_session(conn->ssl);
    if (sess == NULL) {
        return CFW_OK;
    }
    len = i2d_SSL_SESSION(sess, NULL);
    if (len <= 0) {
        return CFW_ERROR_IO;
    }
    if ((size_t)len > cap) {
        *written = (size_t)len;
        return CFW_ERROR_OVERFLOW;
    }
    p = out;
    if (i2d_SSL_SESSION(sess, &p) != len) {
        return CFW_ERROR_IO;
    }
    *written = (size_t)len;
    return CFW_OK;
}

cfw_status_t cfw_tls_conn_set_session(cfw_tls_conn_t *conn,
                                      const unsigned char *data,
                                      size_t size)
{
    const unsigned char *p;
    SSL_SESSION *sess;

    if (conn == NULL || conn->ssl == NULL || data == NULL || size == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (SSL_is_init_finished(conn->ssl)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    p = data;
    sess = d2i_SSL_SESSION(NULL, &p, (long)size);
    if (sess == NULL || (size_t)(p - data) != size) {
        if (sess != NULL) {
            SSL_SESSION_free(sess);
        }
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (SSL_set_session(conn->ssl, sess) != 1) {
        SSL_SESSION_free(sess);
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    SSL_SESSION_free(sess);
    return CFW_OK;
}

int cfw_tls_conn_session_reused(const cfw_tls_conn_t *conn)
{
    if (conn == NULL || conn->ssl == NULL) {
        return 0;
    }
    return SSL_session_reused(conn->ssl);
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

cfw_status_t cfw_tls_conn_alpn(const cfw_tls_conn_t *conn,
                               cfw_strview_t *proto)
{
    (void)conn;
    if (proto != NULL) {
        proto->data = NULL;
        proto->size = 0;
    }
    return CFW_ERROR_UNAVAILABLE;
}

cfw_status_t cfw_tls_conn_peer_cert_der(const cfw_tls_conn_t *conn,
                                        unsigned char *out, size_t cap,
                                        size_t *written)
{
    (void)conn;
    (void)out;
    (void)cap;
    if (written != NULL) {
        *written = 0;
    }
    return CFW_ERROR_UNAVAILABLE;
}

cfw_status_t cfw_tls_conn_get_session(const cfw_tls_conn_t *conn,
                                      unsigned char *out, size_t cap,
                                      size_t *written)
{
    (void)conn;
    (void)out;
    (void)cap;
    if (written != NULL) {
        *written = 0;
    }
    return CFW_ERROR_UNAVAILABLE;
}

cfw_status_t cfw_tls_conn_set_session(cfw_tls_conn_t *conn,
                                      const unsigned char *data,
                                      size_t size)
{
    (void)conn;
    (void)data;
    (void)size;
    return CFW_ERROR_UNAVAILABLE;
}

int cfw_tls_conn_session_reused(const cfw_tls_conn_t *conn)
{
    (void)conn;
    return 0;
}

#endif /* CFW_HAVE_TLS */
