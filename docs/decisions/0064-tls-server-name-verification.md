<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0064: TLS client server-name indication and hostname verification

- Status: Accepted
- Date: 2026-11-27

## Context

`cfw_tls_ctx_options_t.verify_peer` on a `CFW_TLS_CLIENT` context
checks only the certificate chain against `ca_file`: OpenSSL's
`SSL_VERIFY_PEER` does not compare the peer certificate's identity to
the name the client intended to reach. A client connecting to
`api.example.com` would accept any certificate signed by the trusted
CA, including one issued for `evil.example.com`. The TLS client role
also sent no SNI, so servers hosting multiple names could not select
the right certificate or configuration.

## Decision

Add `cfw_tls_conn_set_server_name(conn, name)` for client
connections, to be called after `cfw_tls_conn_create` and before
`cfw_tls_handshake`. It performs two OpenSSL calls:

- `SSL_set_tlsext_host_name` — sends the name in the ClientHello SNI
  extension so the server can select the appropriate certificate or
  virtual host.
- `SSL_set1_host` — pins hostname verification into the certificate
  check performed when `verify_peer` is enabled (RFC 6125 dNSName SAN
  matching with CN fallback via OpenSSL's `X509_check_host`).

The call is rejected (`CFW_ERROR_INVALID_ARGUMENT`) for NULL/empty
names, names over 255 bytes, server-side connections, and calls after
the handshake has started (tracked by a new `handshake_started` flag
set on the first `cfw_tls_handshake` call).

## Consequences

- Callers that want hostname verification must call
  `cfw_tls_conn_set_server_name` explicitly; `verify_peer` alone
  retains its chain-only semantics. This is documented on both the
  function and `verify_peer`.
- Server-side connections are unaffected; SNI remains a client-side
  concern (the server sees the offered name through OpenSSL but the
  framework does not surface it — no current consumer needs it).
- IDN/punycode handling is delegated to the caller: the name is passed
  to OpenSSL verbatim, matching `SSL_set1_host` semantics.

## Alternatives considered

- **Hostname as a ctx option:** rejected — SNI/verification is
  per-connection (one ctx may serve many names), not per-context.
- **Automatic hostname from the connect address:** rejected — the
  transport layer does not know the intended hostname (the fd may be
  connected by other means), and implicit verification is a footgun.
- **Separate verify and SNI functions:** rejected — the two almost
  always want the same name; splitting invites mismatched pairs.
