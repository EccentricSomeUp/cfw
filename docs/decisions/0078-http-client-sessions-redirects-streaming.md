<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0078: HTTP client sessions, redirects, retries, and streaming bodies

- Status: Accepted
- Date: 2026-11-27

## Context

ADR 0068 shipped `cfw/http_client.h` as a deliberately minimal
one-shot client: one connection per call, `Connection: close`, the
full response body buffered into `response->body`, and no redirect,
retry, or reuse support. The 1.0 readiness review flagged this as the
largest gap between the shipped surface and what a first major
version should offer, and the API freeze means the final shape must be
settled before `1.0.0` — post-freeze only additive changes are
permitted.

The pieces needed already exist in-tree: `cfw_http_response_framing`
knows when a connection may persist, `cfw_tls_conn_get_session` /
`cfw_tls_conn_set_session` (ADR 0062) provide TLS session resumption,
and the client already drives everything through the public
`cfw_net_*` / `cfw_pollset_*` / `cfw_tls_*` surface.

## Decision

Extend `cfw/http_client.h` along four axes, all implemented inside the
existing single-exchange engine:

### Keep-alive sessions

New opaque type `cfw_http_client_session_t` (heap-allocated via
`cfw_http_client_session_create` / `_destroy`, per the "prefer opaque
types" rule in `docs/api-stability.md`) with
`cfw_http_client_session_perform` and `cfw_http_client_session_close`.
A session caches at most one connection keyed by authority
(scheme/TLS-ness, host, port). `perform` reuses the cached connection
when the authority matches and the previous response negotiated
persistence; otherwise it connects fresh. On reuse, a stale connection
discovered mid-exchange triggers one transparent reconnect-and-retry —
but only for idempotent methods (GET/HEAD/PUT/DELETE/OPTIONS), since a
non-idempotent request may already have been processed.

Keep-alive negotiation follows RFC 9112: the client sends
`Connection: keep-alive` on session requests (`close` for one-shot
`cfw_http_client_request`, unchanged), and a connection is retained
only when the response framing is self-terminating (Content-Length or
chunked — never UNTIL_CLOSE), the response carries no
`Connection: close`, and either the response is HTTP/1.1+ or it
explicitly sends `Connection: keep-alive`. Any leftover bytes past the
response body, or a `Connection: close` response, drop the connection.
TLS sessions are serialized with `cfw_tls_conn_get_session` when a TLS
connection is retired and replayed via `cfw_tls_conn_set_session` on
the next connect, giving resumption for free.

### Redirects

`cfw_http_client_request_t.max_redirects` (0 = off, hard cap 16)
follows 301/302/303/307/308 `Location` responses inside `perform`.
303 always rewrites to GET; 301/302 rewrite POST to GET (the
interoperability convention); 307/308 preserve method and body. The
`Location` target is resolved against the request target supporting
absolute URIs, scheme-relative `//host`, absolute paths, relative
paths, and query-only references. Redirects never cross the
http/https scheme boundary implied by `tls_ctx` presence, never carry
`Authorization` or `Cookie` headers to a different authority, and an
unresolvable or userinfo-bearing `Location` ends the chain by
returning the 3xx response itself. Exhausting `max_redirects` also
returns the last 3xx response rather than an error.

### Streaming bodies

`cfw_http_client_request_t.body_sink` / `body_sink_user` deliver each
decoded body chunk to a caller callback instead of buffering into
`response->body`. `max_body_size` is still enforced on the cumulative
streamed total; a sink error aborts the exchange with that status.
Intermediate redirect bodies are drained (bounded) but never passed to
the sink — the sink observes only the final response.

### Retries

`cfw_http_client_request_t.max_retries` (hard cap 4) retries an
exchange that fails with `CFW_ERROR_IO`, and only for idempotent
methods (GET/HEAD/PUT/DELETE/OPTIONS/TRACE) — a retried request is
therefore safe to re-execute. Retries never fire after a `body_sink`
callback has received bytes (a partial stream cannot be unwound), and
never for `CFW_ERROR_TIMEOUT`, which the shared deadline already
bounds. In addition, a session connection that turns out to be stale
earns one free reconnect-and-retry on top of the budget. All hops and
retries share the request's single monotonic deadline; there is no
backoff sleep.

## Consequences

- `cfw_http_client_request_t` gains appended fields only — existing
  callers compile and behave identically (all new fields default to
  off).
- One-shot `cfw_http_client_request` keeps its exact current
  semantics; it is implemented over the same exchange engine with a
  private, non-reused connection.
- Sessions are single-threaded like the rest of the client contract;
  concurrent sessions on separate threads are supported.
- DNS resolution remains blocking and outside the deadline, unchanged.
- Compression, cookies/jar policy, proxy support, and HTTP/2 remain
  application concerns.
