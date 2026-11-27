<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0058: Connection transport seam and TLS server listeners

- Status: Accepted
- Date: 2026-11-28

## Context

ADR 0057 delivered `cfw_tls` as a standalone transport module but
deferred `cfw_server_t` integration: there was no way for the
server's accept path to hand an encrypted byte stream to
`cfw_http1_conn_t`, which previously called `recv`/`send` directly
on its socket.

Two properties had to be preserved:

- The default build must remain byte-for-byte identical in behavior
  and dependency-free; the seam must cost nothing when unused.
- `cfw_http1_conn_t` keeps sole ownership of the socket fd — TLS is
  a byte-stream transform, not a socket owner, so lifecycle stays in
  one place.

## Decision

Add an optional **transport vtable** to `cfw_http1_conn_t`:

- `cfw_http1_conn_options_t` gains a `const cfw_http1_transport_t
  *transport` field (`user`, `read`, `write`). When NULL — the
  default — `http1_conn` performs raw `recv`/`send`/`close` on the
  fd exactly as before.
- The transport borrows the fd; it must not close or dup it.
  `http1_conn` retains fd ownership and closes it on
  `cfw_http1_conn_free`/`close` regardless of transport.
- Options structs are now documented as requiring zero
  initialization (`memset`) so optional fields default off. All
  in-tree callers were updated; this is an additive, source-compatible
  change for conforming callers.

Server integration:

- `cfw_server_options_t` gains `tls_ctx` (`cfw_tls_ctx_t *`, not
  owned — must outlive the server).
- On accept, when `options.tls` is set the server creates a
  `cfw_tls_conn_t` over the accepted fd and installs a server-side
  transport that delegates to `cfw_tls_conn_read`/`write`/`shutdown`.
- During the handshake the connection is not yet an HTTP parser
  consumer: `service_conn` drives `cfw_tls_conn_handshake`-equivalent
  I/O through the same transport callbacks and arms poll interest
  from `cfw_tls_conn_want_read`/`want_write` instead of the HTTP
  reader's needs. Once the handshake completes, normal HTTP
  servicing resumes unchanged — the decrypted stream is
  indistinguishable from cleartext to the parser.
- TLS errors map to connection teardown; `CFW_ERROR_WOULDBLOCK`
  yields to the pollset like any other backpressure.

## Alternatives considered

- **Server-owned TLS inside `http1_conn`**: rejected — would couple
  the connection layer to OpenSSL and break the dependency-free
  default build.
- **TLS termination in a front proxy only**: still supported, but
  first-party HTTPS is now possible without extra infrastructure.
- **Per-listener TLS (mixed cleartext+TLS ports)**: deferred. A
  single `options.tls` applies to all accepted connections on the
  listener; mixed-mode listeners can be added later without ABI
  breakage by extending options.

## Consequences

- `CFW_ENABLE_TLS=ON` builds can serve HTTPS directly via
  `cfw_server_options_t.tls_ctx`; cleartext builds are unaffected
  (`cfw_server_init` returns `CFW_ERROR_UNAVAILABLE` if `tls_ctx`
  is set without `CFW_HAVE_TLS`).
- Handshake happens inline on the accepting thread — acceptable for
  the single-threaded event loop model; a future worker-pool design
  can offload it without API changes.
- New audit surface: handshake state machine inside the server loop,
  fd/SSL lifetime pairing on teardown paths, and poll-interest
  correctness during handshake. Covered by the change's audit notes.
- Follow-up work: ALPN, session resumption tuning, TLS example,
  mixed cleartext/TLS listeners, client-certificate auth plumbing.
