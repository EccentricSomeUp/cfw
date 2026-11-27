/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_TLS_H
#define CFW_TLS_H

#include <cfw/cfw_config.h>
#include <cfw/export.h>
#include <cfw/status.h>
#include <cfw/strview.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional TLS transport module (ADR 0057, ADR 0030 boundary).
 *
 * Availability: compiled only when CMake option CFW_ENABLE_TLS is ON
 * and OpenSSL 3.x was found; CFW_HAVE_TLS is 1 in that case and 0
 * otherwise. When unavailable, every function in this header is still
 * declared and returns CFW_ERROR_UNAVAILABLE (or a harmless value for
 * predicates), matching the stub convention used for unimplemented
 * platforms.
 *
 * Design notes:
 * - cfw_tls_ctx_t is an immutable, shareable TLS configuration
 *   (method, certificate, trust store, verify policy). Create one per
 *   listener configuration; it is safe to share across connections
 *   on the creating thread. It is NOT internally synchronized —
 *   create/destroy it before spawning workers.
 * - cfw_tls_conn_t wraps one nonblocking socket descriptor with a
 *   TLS session. The underlying fd MUST be nonblocking and MUST
 *   outlive the connection: cfw_tls_conn_create BORROWS the
 *   descriptor (unlike cfw_http1_conn_init it does not take
 *   ownership) and cfw_tls_conn_destroy does not close it — the
 *   caller remains responsible for closing the socket. This lets a
 *   TLS session layer under an existing fd owner such as
 *   cfw_http1_conn_t. All I/O entry points are nonblocking and
 *   report CFW_ERROR_WOULDBLOCK when the operation cannot complete
 *   immediately. Callers drive progress from a pollset using
 *   cfw_tls_conn_want_read/want_write and cfw_tls_pending.
 * - No application data is ever logged by this module; TLS errors are
 *   mapped to cfw_status_t without detail strings that could leak
 *   key material or handshake internals.
 */

typedef enum cfw_tls_role {
    /* Accept TLS connections (server side). cert_file/key_file are
     * required; ca_file + verify_peer enable client-certificate
     * authentication. */
    CFW_TLS_SERVER = 0,
    /* Initiate TLS connections (client side). cert_file/key_file are
     * optional (client certificate); ca_file + verify_peer control
     * server certificate verification. */
    CFW_TLS_CLIENT = 1
} cfw_tls_role_t;

typedef struct cfw_tls_ctx_options {
    cfw_tls_role_t role;
    /* PEM-encoded certificate chain file. Required for CFW_TLS_SERVER;
     * optional for CFW_TLS_CLIENT. NULL means "not used". */
    const char *cert_file;
    /* PEM-encoded private key file matching cert_file. Same
     * required/optional rules as cert_file. */
    const char *key_file;
    /* PEM-encoded CA bundle used to verify the peer. For servers this
     * enables client-certificate verification; for clients it is the
     * trust store for server certificates. */
    const char *ca_file;
    /* Nonzero requires and verifies the peer certificate. For
     * CFW_TLS_SERVER this means mandatory client authentication; for
     * CFW_TLS_CLIENT it means the server certificate MUST verify
     * against ca_file. */
    int verify_peer;
    /* Nonzero enables the SSL_MODE_ENABLE_PARTIAL_WRITE and
     * SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER modes, allowing cfw_tls_write
     * to report partial progress and callers to reuse/move buffers
     * between retries. Enabled by default behavior already permits
     * partial writes; this flag additionally allows the caller to
     * modify the buffer contents after a WOULDBLOCK return. Default 0
     * keeps the safer "buffer must remain unchanged until accepted"
     * contract. */
    int allow_moving_write_buffer;
    /* Optional NULL-terminated list of ALPN protocol identifiers
     * (ADR 0060), e.g. {"http/1.1", NULL}. Each identifier must be
     * 1-255 bytes. For CFW_TLS_SERVER the list is the server's
     * preference order: the first configured protocol the client also
     * offered is selected; if the client offers ALPN but nothing
     * matches, the handshake continues without ALPN (no fatal
     * alert). For CFW_TLS_CLIENT the list is sent verbatim in the
     * ClientHello. The strings are only read during
     * cfw_tls_ctx_create; NULL disables ALPN. */
    const char * const *alpn_protos;
} cfw_tls_ctx_options_t;

typedef struct cfw_tls_ctx cfw_tls_ctx_t;
typedef struct cfw_tls_conn cfw_tls_conn_t;

/* Creates a TLS context. Returns CFW_ERROR_INVALID_ARGUMENT for NULL
 * options or missing required fields, CFW_ERROR_NOMEM on allocation
 * failure, CFW_ERROR_IO when certificate/key material cannot be
 * loaded or is inconsistent, CFW_ERROR_UNAVAILABLE when TLS support
 * is not compiled in. */
CFW_API cfw_status_t cfw_tls_ctx_create(
    const cfw_tls_ctx_options_t *options, cfw_tls_ctx_t **ctx);
CFW_API void cfw_tls_ctx_destroy(cfw_tls_ctx_t *ctx);

/* Creates a TLS connection over a borrowed nonblocking socket
 * descriptor. `fd` must be a valid nonblocking socket (e.g. from
 * cfw_fd_get) that outlives the connection; the descriptor is NOT
 * owned and is not closed by cfw_tls_conn_destroy. Returns
 * CFW_ERROR_INVALID_ARGUMENT for a negative fd or NULL conn out-param,
 * CFW_ERROR_NOMEM on allocation failure, CFW_ERROR_UNAVAILABLE when
 * TLS support is not compiled in. */
CFW_API cfw_status_t cfw_tls_conn_create(cfw_tls_ctx_t *ctx, intptr_t fd,
                                         cfw_tls_conn_t **conn);
CFW_API void cfw_tls_conn_destroy(cfw_tls_conn_t *conn);

/* Returns the raw descriptor for pollset registration. The descriptor
 * remains owned by the connection; do not close it. Returns an invalid
 * handle after destroy or after the connection was closed. */
CFW_API intptr_t cfw_tls_conn_fd(const cfw_tls_conn_t *conn);

/* Drives the TLS handshake. Returns CFW_OK once the handshake has
 * completed (also true on subsequent calls), CFW_ERROR_WOULDBLOCK when
 * more socket I/O is needed — inspect cfw_tls_conn_want_read/write to
 * know which direction — CFW_ERROR_IO on handshake failure or
 * unexpected close, CFW_ERROR_UNAVAILABLE without TLS support. */
CFW_API cfw_status_t cfw_tls_handshake(cfw_tls_conn_t *conn);
CFW_API int cfw_tls_handshake_done(const cfw_tls_conn_t *conn);

/* Nonblocking TLS read. On CFW_OK, *done holds bytes written to buf
 * (0 means the peer performed an orderly close_notify shutdown).
 * CFW_ERROR_WOULDBLOCK means retry after the indicated direction is
 * ready; CFW_ERROR_IO means an unrecoverable TLS/socket error. */
CFW_API cfw_status_t cfw_tls_read(cfw_tls_conn_t *conn, void *buf,
                                  size_t size, size_t *done);

/* Nonblocking TLS write of up to size bytes. On CFW_OK, *done holds
 * bytes consumed from data (may be less than size; caller retries
 * with the remainder). Same error contract as cfw_tls_read. */
CFW_API cfw_status_t cfw_tls_write(cfw_tls_conn_t *conn, const void *data,
                                   size_t size, size_t *done);

/* Initiates/continues the TLS close_notify shutdown. Returns CFW_OK
 * once the bidirectional shutdown has completed, CFW_ERROR_WOULDBLOCK
 * while it is still in progress — inspect cfw_tls_conn_want_read /
 * want_write for the direction to wait on — CFW_ERROR_IO on failure.
 * The socket stays open until cfw_tls_conn_destroy. */
CFW_API cfw_status_t cfw_tls_shutdown(cfw_tls_conn_t *conn);

/* Direction hints from the most recent WOULDBLOCK return: nonzero
 * when the last operation needs socket readability/writability
 * before retrying. Both may be zero after a completed operation. */
CFW_API int cfw_tls_conn_want_read(const cfw_tls_conn_t *conn);
CFW_API int cfw_tls_conn_want_write(const cfw_tls_conn_t *conn);

/* Bytes already decrypted and buffered inside the TLS session that
 * can be returned by cfw_tls_read without further socket reads.
 * Pollset integrations should keep the fd readable-armed while this
 * is nonzero. */
CFW_API size_t cfw_tls_pending(const cfw_tls_conn_t *conn);

/* Returns the ALPN protocol negotiated during the handshake (ADR
 * 0060). On CFW_OK, `proto` is a view into TLS-session memory valid
 * until cfw_tls_conn_destroy (do not free it); an empty view
 * (data == NULL, size == 0) means no protocol was negotiated —
 * either ALPN was not configured or the peer declined. Returns
 * CFW_ERROR_INVALID_ARGUMENT for NULL arguments,
 * CFW_ERROR_UNAVAILABLE without TLS support. */
CFW_API cfw_status_t cfw_tls_conn_alpn(const cfw_tls_conn_t *conn,
                                       cfw_strview_t *proto);

/* Writes the DER encoding of the peer's certificate (RFC 5280) into
 * `out` — the identity a verified client or server presented. Useful
 * for mutual-TLS authorization decisions above the transport layer.
 *
 * `written` always receives the DER length: on CFW_OK it is the
 * number of bytes written; on CFW_ERROR_OVERFLOW it is the required
 * buffer size so callers can size-then-fetch. When the peer sent no
 * certificate (e.g. server side without verify_peer), returns CFW_OK
 * with *written == 0. The bytes are owned by the caller; parse or
 * hash them as needed — the framework does not interpret them.
 * Returns CFW_ERROR_INVALID_ARGUMENT for bad arguments,
 * CFW_ERROR_UNAVAILABLE without TLS support. */
CFW_API cfw_status_t cfw_tls_conn_peer_cert_der(
    const cfw_tls_conn_t *conn, unsigned char *out, size_t cap,
    size_t *written);

/* Session resumption (ADR 0062). The server side resumption cache is
 * OpenSSL's built-in per-SSL_CTX session cache — enabled by default
 * with an automatic timeout; no extra configuration is needed. The
 * client side is explicit: serialize the negotiated session with
 * cfw_tls_conn_get_session after the handshake, keep the bytes in
 * your own cache keyed however you like, and offer them to the next
 * connection via cfw_tls_conn_set_session BEFORE calling
 * cfw_tls_handshake.
 *
 * get_session writes the DER-encoded SSL_SESSION into `out`;
 * `written` always receives the length — on CFW_OK the bytes written,
 * on CFW_ERROR_OVERFLOW the required buffer size (so callers can
 * size-then-fetch). When no session exists yet (handshake not
 * complete) it returns CFW_OK with *written == 0. Treat the bytes as
 * opaque and do not hand them to peers other than the issuing
 * server.
 *
 * set_session decodes and installs a session previously produced by
 * get_session; it returns CFW_ERROR_INVALID_ARGUMENT for malformed
 * bytes or when called after the handshake has started. Returns
 * CFW_ERROR_UNAVAILABLE without TLS support. */
CFW_API cfw_status_t cfw_tls_conn_get_session(const cfw_tls_conn_t *conn,
                                              unsigned char *out, size_t cap,
                                              size_t *written);
CFW_API cfw_status_t cfw_tls_conn_set_session(cfw_tls_conn_t *conn,
                                              const unsigned char *data,
                                              size_t size);

/* Nonzero when the most recently completed handshake resumed a
 * previously negotiated session instead of running a full
 * handshake. Returns 0 before the handshake completes or without
 * TLS support. */
CFW_API int cfw_tls_conn_session_reused(const cfw_tls_conn_t *conn);

/* Sets the expected peer hostname on a CLIENT connection: the name
 * is sent as the TLS SNI extension in the ClientHello and, when the
 * ctx has verify_peer enabled, is checked against the server
 * certificate during verification (RFC 6125 dNSName SAN with CN
 * fallback via OpenSSL's X509_check_host). Must be called before
 * cfw_tls_handshake; returns CFW_ERROR_INVALID_ARGUMENT for NULL or
 * empty names, names over 255 bytes, server-side connections, or
 * calls after the handshake has started, and CFW_ERROR_IO if the
 * underlying SSL object rejects the name. Without this call,
 * verify_peer alone checks only the certificate chain — hostname
 * verification does not happen. Returns CFW_ERROR_UNAVAILABLE
 * without TLS support. */
CFW_API cfw_status_t cfw_tls_conn_set_server_name(cfw_tls_conn_t *conn,
                                                  const char *name);

/* Returns the TLS SNI hostname associated with the session, when
 * present: on a server connection this is the server_name extension
 * the client offered in its ClientHello (usable for virtual-host
 * routing or certificate selection); on a client connection it is
 * the name previously installed via cfw_tls_conn_set_server_name.
 * The returned view borrows TLS-session memory and is valid until
 * cfw_tls_conn_destroy — do not free it. An empty view
 * (data == NULL, size == 0) means no SNI name is available. Returns
 * CFW_ERROR_INVALID_ARGUMENT for NULL arguments and
 * CFW_ERROR_UNAVAILABLE without TLS support. */
CFW_API cfw_status_t cfw_tls_conn_get_server_name(const cfw_tls_conn_t *conn,
                                                  cfw_strview_t *name);

#ifdef __cplusplus
}
#endif

#endif /* CFW_TLS_H */
