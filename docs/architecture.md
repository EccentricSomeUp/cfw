<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Architecture

## Current state

`cfw` is a working, pre-1.0 POSIX HTTP/1.1 server framework. The core
primitives (status, version, checked arithmetic, span/buffer, arena, time,
random, logging, filesystem, fd ownership), the platform abstraction layer
(page size, CSPRNG, file metadata, close), the internal POSIX socket/pollset/
net layer, the HTTP/1.1 parser and connection binding, the router, the
dispatch/middleware chain with streaming bodies, and the single-threaded
server lifecycle are all implemented and covered by unit, integration,
security, and fuzz tests. Windows and non-POSIX platforms remain stubs
returning `CFW_STATUS_UNAVAILABLE`; see `docs/platform-matrix.md`.

## Architectural goals

- Dependency-free C11 implementation
- Predictable ownership and lifetime rules
- Explicit error handling with stable status codes
- Bounded resource usage and denial-of-service resistance
- Platform abstraction for POSIX-like and Windows systems
- Stable public API with controlled ABI evolution
- Extensibility through well-defined middleware, handler, configuration, and
  lifecycle hooks

## Intended layering

```text
Application
    |
Public API: server, router, request, response, middleware, config
    |
Core services: memory, buffers, strings, time, logging, events
    |
Platform abstraction: sockets, polling, threads, files, TLS boundary
    |
Operating system
```

Layers must not bypass lower-level contracts casually. For example, HTTP code
must not depend directly on OS socket APIs when an internal transport
abstraction exists.

## Subsystems

- `core`: status codes, versioning, allocation policy, buffers, strings, time
  (implemented; see `cfw/status.h`, `cfw/version.h`, `cfw/buffer.h`,
  `cfw/span.h`, `cfw/strview.h`, `cfw/arena.h`, `cfw/time.h`,
  `cfw/checked_arithmetic.h`, `cfw/random.h`, `cfw/log.h`)
- `platform`: OS capability detection and abstraction — page size, CSPRNG,
  file metadata (implemented; see `cfw/platform.h`, `cfw/fs.h`, `cfw/fd.h`)
- `net`: sockets, address handling, polling/event loop primitives — internal
  POSIX implementation: `epoll` on Linux/Android, `kqueue` on macOS/BSD,
  `poll` fallback elsewhere (ADRs 0014/0052)
- `http`: request/response parsing and serialization
- `http1`: HTTP/1.1 server message reader and connection binding
  (implemented; see `cfw/http.h`, `cfw/http1.h`, `cfw/http1_conn.h`)
- `router`: route matching and parameters (implemented; see `cfw/route.h`)
- `dispatch`: middleware chain, per-route handlers, error mapping,
  response-head serialization, and pull-style streaming bodies with
  optional known-length Content-Length framing
  (implemented; see `cfw/dispatch.h`, ADRs 0031/0041)
- `multipart`: buffered multipart/form-data parsing — boundary
  extraction, part headers, Content-Disposition name/filename
  unescaping, zero-copy part bodies (implemented; see
  `cfw/multipart.h`, ADR 0043)
- `server`: listener, event loop, connection limits/timeouts,
  graceful stop, streaming request-body upload hooks (ADR 0033),
  peer-address exposure (ADR 0032), a per-request completion audit
  hook (ADR 0055), and optional SO_REUSEPORT multi-instance serving
  for application-owned worker threads (ADR 0056)
  (implemented; see `cfw/server.h`)
- `static`: static file serving middleware with lexical path validation,
  directory index resolution, MIME mapping, bounded stream slots,
  confined no-follow file opens on POSIX, and conditional/Range request
  support with entity-tag validators (implemented; see `cfw/static.h`,
  ADRs 0036/0037/0039/0040)
- `tls`: optional OpenSSL 3.x-backed transport module below
  `http1_conn`/`server` (implemented as `cfw/tls.h`/`src/cfw/tls.c`;
  ADRs 0030/0057/0058/0060). Opt-in via `CFW_ENABLE_TLS`; default
  builds are cleartext-only and the entry points stub to
  `CFW_ERROR_UNAVAILABLE`. `cfw_http1_conn_t` accepts an optional
  `cfw_http1_transport_t` byte-stream override, and
  `cfw_server_options_t.tls_ctx` turns a listener into an HTTPS
  server (handshake before HTTP parsing, ALPN negotiation, mTLS peer
  certificate access). Upgraded protocols (WebSocket) ride the same
  transport, so `wss://` works end-to-end.
- `uri`: shared percent-decoding primitive with per-use flags
  (implemented; see `cfw/uri.h`, ADR 0044); also backs query parsing,
  static-file path decoding, and route-parameter decoding
- `cookie`: Cookie header parsing and Set-Cookie serialization with
  RFC 6265 character validation (implemented; see `cfw/cookie.h`,
  ADR 0045)
- `crypto`: SHA-256, HMAC-SHA256 (one-shot and streaming), and
  constant-time comparison for MACs and integrity tags (implemented;
  see `cfw/crypto.h`, ADR 0046); not for password hashing
- `codec`: canonical base64, URL-safe base64url (ADR 0059), and hex
  encode/decode for tokens and signatures (implemented; see
  `cfw/codec.h`, ADRs 0047/0059)
- `signed`: HMAC-authenticated token envelopes for signed cookies,
  CSRF, and reset tokens (implemented; see `cfw/signed.h`, ADR 0048)
- `sync`: portable mutex and condition-variable primitives with
  monotonic deadlines for application-owned concurrency (implemented;
  see `cfw/sync.h`, ADR 0050)
- `thread`: portable thread spawn/join/detach primitive for
  application-owned workers; each `cfw_server_t` instance remains
  single-threaded, and multi-worker deployments bind separate
  instances via `SO_REUSEPORT` (implemented; see `cfw/thread.h`,
  ADRs 0051/0056)
- `log`: structured, rate-limited, non-blocking-friendly logging

## Security invariants

- All externally supplied lengths and offsets are validated before use.
- All allocations have checked multiplication/addition and failure paths.
- Parser state is explicit and bounded; ambiguous input fails closed.
- Public APIs document ownership, thread safety, lifetime, and error behavior.
- No secret material is logged or embedded in error strings.
- Timeouts and resource limits are part of normal operation, not optional
  hardening.

## Extensibility model

Extensibility occurs through typed callbacks and lifecycle hooks rather
than macro injection or global mutable state. Middleware ordering, ownership,
and cancellation behavior are specified in `cfw/dispatch.h` and ADR 0031.

## Compatibility policy

Until a stability decision record says otherwise:

- Minor versions may add APIs and fix behavior.
- Patch versions may fix bugs without changing documented behavior.
- Breaking API or ABI changes require a major version increment.
- Internal headers and symbols are not stable and may change without notice.
