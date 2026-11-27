<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0062: TLS session resumption API

- Status: Accepted
- Date: 2026-11-28

## Context

The TLS module (ADR 0057) negotiated full handshakes only. Session
resumption was the last deferred TLS roadmap item: servers already
get OpenSSL's built-in session cache for free, but clients had no
way to serialize, store, and offer a session for a subsequent
connection, and neither side could observe whether resumption
occurred.

## Decision

Expose three additive functions on `cfw_tls_conn_t`:

- `cfw_tls_conn_get_session` — DER-encodes the negotiated
  `SSL_SESSION` (`i2d_SSL_SESSION`) into a caller buffer with the
  same size-then-fetch contract as `cfw_tls_conn_peer_cert_der`
  (`CFW_ERROR_OVERFLOW` reports the required size). Returns
  `CFW_OK` with `*written == 0` when no session exists yet.
- `cfw_tls_conn_set_session` — decodes (`d2i_SSL_SESSION`) and
  installs a session via `SSL_set_session`. Must be called before
  `cfw_tls_handshake`; malformed bytes or a completed handshake
  return `CFW_ERROR_INVALID_ARGUMENT`. The decoded session is freed
  immediately — `SSL_set_session` increments its own reference.
- `cfw_tls_conn_session_reused` — `SSL_session_reused` passthrough
  for observability/logging.

The server side needs no API: OpenSSL's per-`SSL_CTX` session cache
is enabled by default and works for both TLS 1.2 session IDs and
TLS 1.3 tickets.

## Consequences

- Clients own session caching policy (key, TTL, storage) — the
  framework stays storage-agnostic and dependency-free.
- Serialized sessions are sensitive (they authenticate the client
  to the server): documented as opaque bytes that must not be
  shared with other peers.
- TLS 1.3 sessions are only resumable after post-handshake
  NewSessionTicket messages are processed; applications that cache
  eagerly after `cfw_tls_handshake` reports done may capture a
  non-resumable session — the session simply won't resume, which is
  a performance issue, not a correctness one.
