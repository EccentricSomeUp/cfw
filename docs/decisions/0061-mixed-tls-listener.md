<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0061: Mixed cleartext/TLS listener via first-byte sniffing

- Status: Accepted
- Date: 2026-11-28

## Context

ADR 0058 made `cfw_server_options_t.tls_ctx` apply TLS to *every*
accepted connection. Operators sometimes need one port to serve both
cleartext HTTP and HTTPS (development convenience, gradual
migration), which previously required two listeners or an external
multiplexer.

## Decision

Add `cfw_server_options_t.tls_detect` (requires `tls_ctx`; init
fails with `CFW_ERROR_INVALID_ARGUMENT` without it):

- Accepted connections initialize as ordinary cleartext
  `cfw_http1_conn_t`s and are marked `tls_detect`.
- On the first readable event the server peeks exactly one byte with
  `recv(MSG_PEEK | MSG_DONTWAIT)` — consuming nothing:
  - `0x16` (TLS handshake record, content type 22) → attach a
    `cfw_tls_conn_t` via the ADR 0058 transport seam and proceed to
    the normal handshake gate.
  - Anything else → serve cleartext; the byte remains in the socket
    buffer for the HTTP reader.
  - EOF → close; `EAGAIN` → keep waiting.
- Detection is reliable because an HTTP/1.1 request line always
  begins with an uppercase method token — never byte `0x16`.
  SSLv2-format ClientHellos (`0x80`) are intentionally not detected:
  the TLS floor is TLS 1.2 anyway.

## Alternatives considered

- **Two listeners on two ports**: already possible (two server
  instances) but doesn't satisfy the single-port requirement.
- **SNI/ALPN-based dispatch after handshake**: ALPN (ADR 0060) can
  confirm the protocol post-handshake, but cannot un-encrypt a
  cleartext peer — sniffing must happen before choosing the
  transport, so first-byte detection is the only correct layer.
- **Peeking more bytes / parsing ClientHello**: unnecessary — the
  record content type is sufficient and keeps the hot path to one
  syscall per connection.

## Consequences

- One extra `recv(MSG_PEEK)` syscall per connection, only on
  detect-mode listeners and only until the first byte arrives.
- Cleartext and TLS connections coexist with identical per-conn
  state machines; no new conn type or pollset changes.
- Security note: detection does not weaken TLS — a cleartext peer
  simply gets cleartext service, exactly as if `tls_ctx` were unset.
  Operators must not treat the port as "always encrypted".
