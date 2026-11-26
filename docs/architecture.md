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
  POSIX implementation with `poll` fallback where `epoll` is unavailable
  (implemented; ADR 0014)
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
  graceful stop, streaming request-body upload hooks (ADR 0033), and
  peer-address exposure (ADR 0032) (implemented; see `cfw/server.h`)
- `static`: static file serving middleware with lexical path validation,
  directory index resolution, MIME mapping, bounded stream slots,
  confined no-follow file opens on POSIX, and conditional/Range request
  support with entity-tag validators (implemented; see `cfw/static.h`,
  ADRs 0036/0037/0039/0040)
- `tls`: out of scope for the core library (ADR 0030); a future optional
  module would sit below `http1_conn`/`server` behind its own dependency ADR
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
