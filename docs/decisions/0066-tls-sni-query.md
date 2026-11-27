<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0066: TLS SNI hostname query

- Status: Accepted
- Date: 2026-11-27

## Context

ADR 0064 added client-side SNI via
`cfw_tls_conn_set_server_name`, but the server side had no way to
read the `server_name` extension a client offered. Multi-tenant or
virtual-host deployments need that name for routing or certificate
selection decisions above the transport layer — the same role
`req->tls_conn` (ADR 0063) plays for peer-certificate authorization.

## Decision

Add `cfw_tls_conn_get_server_name(conn, name)` returning a
`cfw_strview_t` view into TLS-session memory:

- Server connections: the `server_name` (host_name) extension the
  client sent in its ClientHello, via OpenSSL's
  `SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name)`.
- Client connections: the name installed via
  `cfw_tls_conn_set_server_name`.
- Absent SNI yields an empty view (`data == NULL, size == 0`), not
  an error.

## Consequences

- The view borrows session memory valid until
  `cfw_tls_conn_destroy`; callers must not retain or free it.
- No certificate-selection callback is exposed: OpenSSL's built-in
  SNI certificate callback remains available to applications that
  need it at the `SSL_CTX` level; this API covers the common
  inspect-after-handshake case.
- Complements `cfw_tls_conn_alpn` and
  `cfw_tls_conn_peer_cert_der` as session-introspection accessors.
