/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_HTTP_CLIENT_H
#define CFW_HTTP_CLIENT_H

#include <cfw/buffer.h>
#include <cfw/http.h>
#include <cfw/status.h>
#include <cfw/time.h>
#include <cfw/tls.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * HTTP/1.1 client (ADR 0068, extended by ADR 0078). The one-shot
 * cfw_http_client_request performs a single request per call on a
 * fresh connection; cfw_http_client_session_t reuses a keep-alive
 * connection across perform() calls to the same authority.
 *
 * Both paths support redirect following (max_redirects), bounded
 * retries of idempotent requests that fail before the request is
 * written, and streaming response bodies through a sink callback.
 *
 * The call is synchronous and bounded by request->timeout (a single
 * monotonic deadline covering connect, TLS handshake, write, read,
 * redirect hops, and retries). DNS resolution itself is blocking and
 * NOT covered by the timeout — resolve-critical applications should
 * treat cfw_addr_resolve as an init-time operation.
 *
 * Thread safety: requests and sessions use only their own state plus
 * the caller's tls_ctx (which must already be safe for concurrent
 * use per cfw/tls.h rules). A session is single-threaded; concurrent
 * sessions on separate threads are supported.
 *
 * Platform support: POSIX and Windows (Winsock2, ADR 0072).
 * TLS requires a CFW_ENABLE_TLS build
 * and a client-role cfw_tls_ctx_t; without TLS support a non-NULL
 * tls_ctx yields CFW_ERROR_UNAVAILABLE.
 */

typedef struct {
    /* Server host: DNS name or numeric IP literal. Required. */
    const char *host;
    /* TCP port; required, 1-65535. */
    uint16_t port;
    /* HTTP method; must be a known cfw_http_method_t value. */
    cfw_http_method_t method;
    /* Request target in origin-form ("/path?query") or absolute-form.
     * Required; validated like cfw_http_request_head_format. */
    const char *target;
    /* Additional request headers. The framework-managed headers
     * (Host, User-Agent, Content-Length, Connection) are always
     * generated; supplying them here fails with
     * CFW_ERROR_INVALID_ARGUMENT. Names/values are validated by
     * cfw_http_request_head_format. */
    const cfw_http_header_t *headers;
    size_t header_count;
    /* Optional request body; body_size bytes are sent verbatim with a
     * generated Content-Length. May be NULL only when body_size == 0. */
    const void *body;
    size_t body_size;
    /* Optional TLS: a client-role cfw_tls_ctx_t. When non-NULL the
     * connection uses TLS and `host` is sent as SNI and (with
     * verify_peer) checked against the server certificate. NULL means
     * cleartext HTTP. */
    cfw_tls_ctx_t *tls_ctx;
    /* Total deadline for connect + handshake + write + read.
     * A zero duration selects the 30-second default. */
    cfw_duration_t timeout;
    /* Maximum accepted response-head size in bytes; 0 selects the
     * 64 KiB default. Exceeding it fails with CFW_ERROR_OVERFLOW. */
    size_t max_head_size;
    /* Maximum accepted decoded response-body size in bytes; 0 selects
     * the 16 MiB default. UINT64_MAX disables the cap. Exceeding it
     * fails with CFW_ERROR_OVERFLOW. With body_sink set, the cap
     * applies to the cumulative streamed total. */
    uint64_t max_body_size;
    /* Maximum redirect hops followed for 301/302/303/307/308 responses
     * with a resolvable Location header; 0 disables redirect
     * following (the 3xx response is returned). Hard-capped at 16.
     * Redirect semantics: 303 always becomes GET; 301/302 rewrite
     * POST to GET; 307/308 preserve method and body. Redirects never
     * cross the http/https boundary implied by tls_ctx, and
     * Authorization/Cookie headers are not forwarded to a different
     * authority. When the hop limit is exhausted the last 3xx
     * response is returned normally. */
    unsigned max_redirects;
    /* Maximum retries of an exchange that fails with CFW_ERROR_IO
     * (connect refused/reset, truncated stream, etc.); 0 disables
     * retries. Hard-capped at 4. Retries apply only to idempotent
     * methods (GET, HEAD, PUT, DELETE, OPTIONS, TRACE), never after a
     * body_sink has received bytes (a partial stream cannot be
     * unwound), and never for the caller's own timeout. They share
     * the request deadline — there is no backoff sleep. A connection
     * reused by a session additionally gets one free retry when it
     * turns out to be stale. */
    unsigned max_retries;
    /* Optional streaming body sink (ADR 0078). When non-NULL, each
     * decoded body chunk is passed to this callback instead of being
     * buffered into response->body (which then stays empty). The
     * callback runs on the caller's thread, must not block
     * indefinitely, and must return CFW_OK to continue; any other
     * status aborts the request with that status. max_body_size is
     * still enforced on the cumulative total. Intermediate redirect
     * bodies are drained but never delivered to the sink — it
     * observes only the final response. */
    cfw_status_t (*body_sink)(void *user, const void *data,
                              size_t size);
    void *body_sink_user;
} cfw_http_client_request_t;

/* Owns the complete raw response head and the decoded body. Both
 * buffers are valid until cfw_http_client_response_free. */
typedef struct cfw_http_client_response {
    /* Raw response-head bytes (status line + headers + final CRLF).
     * Owned; do not access directly — use
     * cfw_http_client_response_head. */
    cfw_buffer_t head;
    /* Decoded response body (chunked transfer coding already
     * removed). Empty for bodyless responses. Owned. */
    cfw_buffer_t body;
} cfw_http_client_response_t;

/* Performs the request. On return `response` is always initialized
 * and safe to pass to cfw_http_client_response_free, even on error
 * (partial results are discarded). `response` may be NULL to discard
 * the result.
 *
 * Returns CFW_OK on a complete response (any status code — check it
 * via cfw_http_client_response_head). Error mapping:
 * - CFW_ERROR_INVALID_ARGUMENT: bad request fields, forbidden
 *   framework-managed header names, or a malformed response head.
 * - CFW_ERROR_IO: socket errors, TLS failures, or truncated bodies.
 * - CFW_ERROR_TIMEOUT: the deadline expired.
 * - CFW_ERROR_OVERFLOW: max_head_size or max_body_size exceeded.
 * - CFW_ERROR_UNAVAILABLE: TLS requested without TLS support
 *   (CFW_ENABLE_TLS off or OpenSSL not found).
 * - CFW_ERROR_NOMEM: allocation failure. */
CFW_API cfw_status_t cfw_http_client_request(
    const cfw_http_client_request_t *request,
    cfw_http_client_response_t *response);

/* Parses the stored response head. On CFW_OK, `head` views point into
 * response->head storage and remain valid until
 * cfw_http_client_response_free (or the next request reusing the
 * struct). Returns CFW_ERROR_INVALID_ARGUMENT when the response holds
 * no head (e.g., the request failed). */
CFW_API cfw_status_t cfw_http_client_response_head(
    const cfw_http_client_response_t *response,
    cfw_http_response_head_t *head);

/* Releases response storage. Idempotent; safe on a zeroed or
 * partially-filled struct. */
CFW_API void cfw_http_client_response_free(
    cfw_http_client_response_t *response);

/* ------------------------------------------------------------------ */
/* Keep-alive sessions (ADR 0078)                                      */
/* ------------------------------------------------------------------ */

/* Opaque keep-alive HTTP client session. A session caches at most one
 * connection, reused by perform() calls whose request authority
 * (TLS-ness, host, port) matches the cached one. Single-threaded:
 * perform/close must not run concurrently on the same session. */
typedef struct cfw_http_client_session cfw_http_client_session_t;

/* Allocates a session. The returned pointer is owned by the caller and
 * must be released with cfw_http_client_session_destroy. */
CFW_API cfw_status_t cfw_http_client_session_create(
    cfw_http_client_session_t **session);

/* Drops the cached connection (saving any TLS session for resumption)
 * and releases all session state. Idempotent; safe on NULL. */
CFW_API void cfw_http_client_session_destroy(
    cfw_http_client_session_t *session);

/* Drops the cached connection without destroying the session; the
 * next perform() connects fresh. Equivalent to destroy+create but
 * retains the session allocation. Idempotent. */
CFW_API void cfw_http_client_session_close(
    cfw_http_client_session_t *session);

/* Performs request on the session, reusing the cached connection when
 * the authority matches and the connection negotiated persistence.
 * Request/response semantics, redirect following, retries, streaming,
 * and error mapping are identical to cfw_http_client_request; the
 * session only changes connection reuse (Connection: keep-alive is
 * sent instead of close). On a stale-connection failure the exchange
 * reconnects and retries once for idempotent methods. */
CFW_API cfw_status_t cfw_http_client_session_perform(
    cfw_http_client_session_t *session,
    const cfw_http_client_request_t *request,
    cfw_http_client_response_t *response);

#ifdef __cplusplus
}
#endif

#endif /* CFW_HTTP_CLIENT_H */
