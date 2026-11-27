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
 * Minimal HTTP/1.1 client (ADR 0068). Performs one request per call:
 * resolve (blocking getaddrinfo for names), connect, optional TLS,
 * send request, read the full response. One connection per request —
 * keep-alive reuse, redirects, retries, and streaming bodies are
 * intentionally out of scope for this first version.
 *
 * The call is synchronous and bounded by request->timeout (a single
 * monotonic deadline covering connect, TLS handshake, write, and
 * read). DNS resolution itself is blocking and NOT covered by the
 * timeout — resolve-critical applications should treat
 * cfw_addr_resolve as an init-time operation.
 *
 * Thread safety: cfw_http_client_request uses only stack state plus
 * the caller's tls_ctx (which must already be safe for concurrent
 * use per cfw/tls.h rules). Concurrent requests on separate threads
 * are supported.
 *
 * Platform support: POSIX only; on Windows cfw_http_client_request
 * returns CFW_ERROR_UNAVAILABLE. TLS requires a CFW_ENABLE_TLS build
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
     * fails with CFW_ERROR_OVERFLOW. */
    uint64_t max_body_size;
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
 * - CFW_ERROR_UNAVAILABLE: TLS requested without TLS support, or the
 *   platform has no socket backend (Windows).
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

#ifdef __cplusplus
}
#endif

#endif /* CFW_HTTP_CLIENT_H */
