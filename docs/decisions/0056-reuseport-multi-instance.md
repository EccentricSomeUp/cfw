<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0056: SO_REUSEPORT multi-instance serving

## Status

Accepted.

## Context

`cfw_server_t` is deliberately single-threaded (ADR 0029): one event
loop owns one listener, one pollset, and all connection state.
Horizontal scaling was deferred as "one server per thread with
SO_REUSEPORT". Since then `cfw/thread.h` and `cfw/sync.h` (ADRs
0050/0051) landed, giving applications the primitives to run one
server per thread — but nothing in the framework lets those servers
share a listen address.

Constraints:

- The dispatch pipeline is read-only during requests, so N servers
  can safely share one `cfw_dispatch_t`/`cfw_router_t`; only
  server-owned state (listener, pollset, connections, counters)
  differs per instance.
- SO_REUSEPORT semantics differ subtly across platforms: on Linux
  every socket must set the option *before* `bind`, and a socket
  without it cannot join an existing reuse-port group (and vice
  versa); on BSD/macOS the option exists with the same pre-bind
  requirement. Windows has no equivalent and its server is a stub
  anyway (ADR 0011).
- The option must fail loudly when unsupported: silently degrading
  to a plain bind would either fail confusingly at bind time or, worse,
  appear to work while scaling nothing.

## Decision

Add `int reuse_port` to `cfw_server_options_t` (appended field;
additive, pre-1.0 ABI is unfrozen). When nonzero,
`cfw_server_init` sets `SO_REUSEPORT` on the listener socket after
`cfw_net_socket` and before address resolution/`bind`.

- Compile-time support is probed via `check_symbol_exists(SO_REUSEPORT
  "sys.socket.h")` → `CFW_HAVE_SO_REUSEPORT` in the generated internal
  platform header, consistent with ADR 0053's centralized detection.
- Without compile-time support, init fails `CFW_ERROR_UNAVAILABLE`.
- With support, a `setsockopt` failure (e.g. old kernel, sandboxed
  seccomp) fails init `CFW_ERROR_IO`.
- Usage: create one `cfw_server_t` per worker thread, each with
  `reuse_port = 1`, identical host/port, and a shared read-only
  dispatch. The kernel distributes accepted connections across the
  group; each instance keeps its own counters, timeouts, and audit
  hook stream.

Explicitly out of scope: in-process accept handoff to worker pollsets
(a single-listener thread pool), SO_REUSEPORT load-balancing hints,
and any change to connection-to-thread affinity guarantees (the
kernel chooses; apps must not assume sticky sessions).

## Alternatives considered

- **Dedicated acceptor thread + fd handoff to worker pollsets**:
  rejected for now. It gives precise load control and works without
  SO_REUSEPORT, but requires cross-thread fd ownership transfer,
  per-worker wake channels, and a shutdown protocol — a much larger
  correctness surface for marginal gain over kernel distribution.
  Revisit if connection-affinity or accept-rate shaping becomes a
  requirement.
- **Public `cfw_net_set_reuse_port` socket option**: rejected — the
  only consumer is the server listener; exposing raw setsockopt
  surface invites misuse (setting it after bind, on client sockets).
- **Implicit reuseport when bind fails with EADDRINUSE**: rejected —
  retry-with-option hides configuration errors and changes semantics
  silently; explicit opt-in is auditable.
- **SO_REUSEADDR instead**: rejected — it permits rebinding
  TIME_WAIT/listening conflicts but does not load-balance accepts
  across sockets; it solves a different problem.

## Consequences

- Positive: near-linear scaling across cores with ~15 lines of
  framework code; each instance retains full isolation (own
  timeouts, limits, audit stream); no new threads or shared mutable
  state inside the library.
- Negative: kernel hashing can distribute unevenly; connection
  affinity is not guaranteed (documented); platforms without
  SO_REUSEPORT (OpenBSD, Windows) cannot use it — init reports
  `CFW_ERROR_UNAVAILABLE` rather than degrading.
- Security: no new parsing or allocation; the option marginally widens
  the bind surface (multiple sockets per port) which is the intended
  semantics. Audit note: `audits/2026-11-26-reuseport-audit.md`.
- Follow-up: if demand emerges, an in-process worker pool could layer
  on top without changing this API.
