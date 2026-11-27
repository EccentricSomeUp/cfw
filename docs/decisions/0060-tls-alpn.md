<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0060: ALPN protocol negotiation for TLS connections

- Status: Accepted
- Date: 2026-11-28

## Context

ADR 0057/0058 deferred ALPN as TLS follow-up work. Today the only
application protocol is HTTP/1.1 (plus WebSocket upgrades), but ALPN
is the standard mechanism by which a TLS endpoint declares which
protocols it speaks — needed before any future h2 or protocol
multiplexing, and useful now for clients that require it.

## Decision

Add optional ALPN support to `cfw_tls`:

- `cfw_tls_ctx_options_t.alpn_protos`: a NULL-terminated list of
  protocol identifiers (1-255 bytes each). Encoded once at
  `cfw_tls_ctx_create` into the RFC 7301 wire form and owned by the
  ctx, so the option strings are only read during creation.
- **Server**: an OpenSSL `SSL_CTX_set_alpn_select_cb` callback
  receives the client's wire-format offer and selects the first
  *server-preference-ordered* configured protocol the client also
  offered. When the client offers ALPN but nothing matches, the
  callback returns `SSL_TLSEXT_ERR_NOACK`: the handshake continues
  without ALPN rather than aborting — the cleartext-only default
  posture is preserved for clients that cannot negotiate.
- **Client**: the encoded list is installed via
  `SSL_set_alpn_protos` on each connection.
- `cfw_tls_conn_alpn(conn, &view)` exposes the negotiated protocol
  as a `cfw_strview_t` into SSL-owned memory (valid until conn
  destroy); an empty view means nothing was negotiated.

## Alternatives considered

- **Fatal `no_application_protocol` alert on mismatch**: rejected as
  the default — it would break ordinary HTTPS clients that offer
  protocols we do not list. Operators needing strictness can inspect
  `cfw_tls_conn_alpn` after the handshake and close the connection.
- **Single protocol string instead of a list**: rejected — ALPN is
  inherently a list negotiation; a list costs nothing more.
- **Exposing raw OpenSSL types**: rejected per ADR 0057 — the public
  API stays `cfw_`-typed and backend-agnostic.

## Consequences

- Purely additive; contexts without `alpn_protos` behave exactly as
  before.
- The select callback parses untrusted client wire data; bounds are
  checked against `inlen` and malformed lists are treated as
  non-matching (NOACK), never as memory-unsafe reads.
- Follow-up: an HTTP server policy hook could reject connections
  whose negotiated ALPN is absent/unexpected; h2 support remains out
  of scope.
