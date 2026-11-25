<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Architecture

## Current state

`cfw` is pre-implementation. The repository currently contains only build,
packaging, API conventions, version/status primitives, checked arithmetic
helpers, byte-span primitives, and test infrastructure. The architecture below
describes the intended direction, not implemented behavior.

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

## Planned subsystems

- `core`: status codes, versioning, allocation policy, buffers, strings, time
- `platform`: OS capability detection and abstraction
- `net`: sockets, address handling, polling/event loop primitives
- `http`: request/response parsing and serialization
- `router`: route matching, parameters, middleware pipeline
- `server`: lifecycle, configuration, connection management
- `tls`: optional boundary for a future dependency decision; no TLS dependency
  is approved yet
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

Extensibility should occur through typed callbacks and lifecycle hooks rather
than macro injection or global mutable state. Middleware ordering, ownership,
and cancellation behavior must be specified before implementation.

## Compatibility policy

Until a stability decision record says otherwise:

- Minor versions may add APIs and fix behavior.
- Patch versions may fix bugs without changing documented behavior.
- Breaking API or ABI changes require a major version increment.
- Internal headers and symbols are not stable and may change without notice.
