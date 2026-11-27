/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_TLS_H
#define CFW_TLS_H

#include <cfw/cfw_config.h>
#include <cfw/export.h>
#include <cfw/status.h>

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

#ifdef __cplusplus
}
#endif

#endif /* CFW_TLS_H */
