<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Roadmap

This roadmap is intentionally staged. Each phase requires planning, tests,
security review, and audit evidence before proceeding.

## Phase 0: Repository bootstrap

- [x] Establish license, C11 target, `cfw_` API prefix, and dependency policy
- [x] Add CMake build, presets, install rules, and package config
- [x] Add custom test harness and initial unit tests
- [x] Add development, testing, security, and audit documentation

## Phase 1: Core primitives

- [x] Status codes and error propagation conventions
- [x] Version and ABI policy enforcement
- [x] Allocation policy and checked arithmetic helpers
- [x] Buffer, string, span, and memory-region abstractions
- [x] Time, duration, and timeout primitives
- [x] Logging facade with rate limiting and redaction rules

Exit criteria: memory-safe core utilities with unit, sanitizer, and misuse
tests.

## Phase 2: Platform abstraction

- [x] Platform capability detection
- [x] Socket, poll/epoll/kqueue/IOCP abstraction design (epoll, kqueue,
  and poll implemented internally — ADR 0052; Windows Winsock + WSAPoll
  socket/pollset layer implemented — ADR 0072; IOCP deferred, server
  port pending)
- [x] File descriptor/handle ownership rules
- [x] Threading, mutex, and condition-variable primitives — met
  (`cfw/sync.h` mutex/condvar, ADR 0050; `cfw/thread.h` spawn/join/
  detach, ADR 0051)
- [x] Clock, random source, and filesystem boundaries

Exit criteria: documented platform matrix and portable internal APIs.

## Phase 3: Transport and HTTP

- [x] Socket lifecycle and connection management
- [x] HTTP/1.1 request parser and serializer
- [x] Header normalization and validation rules
- [x] Request smuggling resistance
- [x] Body framing, limits, and streaming strategy
- [x] Keep-alive, pipelining, and graceful shutdown behavior

Exit criteria: fuzzed parser (deterministic mutation suite under
sanitizers), protocol security tests (smuggling/framing/limit cases), and
bounded resource behavior (head/body/pending caps) — met.

## Phase 4: Routing and middleware

- Route table design and matching semantics — met (ADR 0026)
- Path parameter extraction — met
- Middleware ordering and cancellation — met (ADR 0027)
- Error handling and response pipeline — met (ADR 0027)
- Extensibility hooks for applications and plugins — met (ADR 0028)

Exit criteria met: API stability review (match.ex binding contract
documented; response-head NULL-header edge fixed) and end-to-end
integration tests over real loopback TCP
(tests/integration/test_http_pipeline.c).

## Phase 5: Server lifecycle

- Configuration model — met (ADR 0029)
- Listener management — met (ADR 0029; getaddrinfo listen hosts)
- Connection limits and timeouts — met (ADR 0029)
- Graceful start/stop/reload behavior — start/stop met; hot reload
  deferred (rebuild server instance; SIGHUP is app policy)
- Operational logging and diagnostics — met (counters, optional
  logger events, and per-request completion audit hook; ADR 0055)

Exit criteria met: bounded soak test (server_soak) and DoS
audit (audits/2026-11-26-http-server-dos-audit.md) with three fixes.

## Phase 6: Optional TLS boundary

- Decide whether TLS is in scope — decided: optional module, not core
  (ADR 0030); cleartext transport by default
- Dependency approval — met: OpenSSL 3.x approved as the optional TLS
  backend (ADR 0057)
- Boundary preserved: `cfw_http1_conn_t`/`cfw_server_t` consume an
  established byte stream; `cfw_tls` (`cfw/tls.h`) provides the
  nonblocking TLS transport below them
- `cfw_tls` transport module — met (context + nonblocking conn,
  handshake/read/write/shutdown, want-direction hints, peer
  verification; unit-tested over socketpairs)
- `cfw_server_t` TLS listener integration — met (ADR 0058): optional
  `cfw_http1_transport_t` seam in `cfw_http1_conn_t` plus
  `cfw_server_options_t.tls_ctx`, which handshakes each accepted
  connection before HTTP parsing. Unit-tested end-to-end
- TLS follow-ups — ALPN met (ADR 0060), peer-cert DER access met,
  mixed cleartext/TLS listeners met (ADR 0061), session resumption
  met (ADR 0062), authZ plumbing met (ADR 0063: `req->tls_conn`
  exposes the session to handlers/middleware)

Exit criteria: scope decision recorded (ADR 0030), dependency
approved (ADR 0057), transport module implemented and tested;
server integration met (ADR 0058) and the cryptographic/transport
audit of the integration path is met
(audits/2026-11-27-openssl-tls-audit.md,
audits/2026-11-28-transport-seam-tls-server-audit.md,
audits/2026-11-28-mixed-tls-listener-audit.md,
audits/2026-11-27-tls-error-queue-isolation-audit.md).

## Phase 7: Hardening and 1.0 readiness

- Full audit catalog pass — met (audits/2026-11-26-1.0-readiness-audit.md)
- ABI/API freeze review — met (docs/api-stability.md; API/ABI frozen at
  the `1.0.0` tag; earlier `0.x` tags remain unfrozen)
- Platform matrix verification — met (docs/platform-matrix.md updated to
  implemented state)
- Fuzz corpus stabilization — met (adversarial seeds added:
  framing conflicts, bare LF, obs-fold, chunked extensions,
  upgrade smuggling)
- Documentation and migration completeness — met (README quickstart and
  minimal-server example; no migration guide was needed for the `1.0.0`
  transition)

## Post-1.0 direction

The phases below are candidates, not commitments. Each requires a planning
decision record recording scope before implementation, consistent with
ADR 0030 (TLS shipped as an optional module rather than core). The guiding
rule is "secure core, optional breadth": the core stays small and
dependency-free; anything carrying application policy, persistence, or a
large API surface ships only as an opt-in module or remains an application
concern.

### Phase 8: Request pipeline and validation helpers

- Request-ID/correlation hooks in the dispatch pipeline so applications
  can propagate context across handlers and logs
- Declarative, bounds-checked request validation helpers (parameters,
  headers, body fields) as an opt-in module; validation stays
  syntax-level — business rules remain application code
- Standard error-response helpers so applications emit consistent,
  non-leaking failures instead of ad-hoc error bodies
- Security-headers middleware module (CSP, frame, nosniff, referrer)
  with per-route opt-out and clear guidance that browser headers do not
  apply indiscriminately to non-browser API responses

### Phase 9: Application-facing security primitives

- Session/token issuance helpers built on `cfw_signed` + `cfw_cookie`
  (rotation, expiry, fixation-resistant issuance) as an opt-in module
- CSRF helper pairing the signed-token primitive with documented cookie
  policies for cookie-authenticated state changes
- Rate-limiting primitives with documented per-instance vs. shared-store
  semantics; the framework must not label per-instance limits as global

### Phase 10: Observability and operations

- Structured request/trace context hooks for correlating logs, metrics,
  and audit events across a request lifecycle
- Metrics interface with stable naming guidance and defaults that avoid
  high-cardinality labels (no raw IDs, tokens, or arbitrary paths)
- Liveness/readiness guidance in deployment docs: readiness may depend on
  subsystems; liveness must not restart-loop on dependency outages
- Audit-event schema extension building on the ADR 0055 completion hook

### Phase 11: Supply chain and verification

- SBOM generation and build-provenance documentation for releases
- Published vulnerability-reporting channel and supported-version policy
- Optional mapping of `docs/security/audit-catalog.md` entries to a named
  external standard (for example a pinned OWASP ASVS version) with an
  explicit selected requirement subset, so verification claims cite a
  standard rather than an informal checklist

### Phase 12: Protocol breadth

Candidates, each gated on its own ADR and audit pass:

- HTTP/2 server support (h2c upgrade and TLS ALPN) as an opt-in module:
  HPACK encode/decode, connection state machine, flow control, stream
  prioritization, and CONTINUATION-flooding defenses. The existing
  `cfw_http1_transport_t` seam is the likely integration point; whether
  h2 shares or parallels the dispatch pipeline is an open design
  question for the ADR.
- Response compression: start with precompressed static assets
  (`.br`/`.gz` variants negotiated via `Accept-Encoding`) as the cheap,
  safe path; a dynamic-compression backend (zlib or equivalent) is a
  separate optional dependency decision.
- HTTP/1.1 polish: HEAD/ETag consistency audit, resumable/ranged
  uploads, and connection-coalescing groundwork shared with HTTP/2.
- Graceful reload: a documented drain + socket-handoff pattern (the
  nginx-style `cfw_server_drain` + listener transfer across worker
  processes) on top of ADR 0056's multi-instance model.
- Reverse proxy / upstream module built on `cfw_http_client`: load
  balancing, health checks, and least-connection scheduling as opt-in
  middleware rather than core.
- Declarative per-route body-size and content-type policy extending the
  phase-8 validation helpers.

### Phase 13: Performance and operational depth

- `io_uring` pollset backend (Linux-only, opt-in build) behind the
  existing platform seam, following the epoll/kqueue/WSAPoll precedent.
- kTLS offload behind the `cfw_tls` boundary where the kernel supports
  it, reducing per-byte TLS cost for high-throughput deployments.
- Zero-copy static-file paths (sendfile/splice on Linux, equivalent
  platform calls elsewhere) inside the existing stream-slot model.
- Multi-worker hardening: per-worker memory arenas, connection
  migration between workers, and SCM_RIGHTS/PIDfd listener handoff for
  zero-downtime reloads.
- Structured tracing export compatible with OTLP/HTTP semantics —
  cfw emits span data; an exporter remains an application concern.
- Predeclared metrics registry with fixed label sets, extending the
  phase-10 interface so exposition helpers cannot introduce
  cardinality bombs.
- Bounds-checked configuration parsing helper so applications do not
  hand-roll argv/env parsing per deployment.
- Reproducible in-repo benchmark suite (`wrk`-style) with published
  numbers against comparable servers on identical hardware.

### Long-horizon directions

Beyond the staged phases above, `docs/long-term-vision.md` records
exploratory directions — formal verification of the wire parser,
capability-based request security, opt-in memory-safety build modes,
WASM middleware hosting, embedded profiles, and agent-facing interfaces.
These are research candidates, not commitments; each would require the
same ADR + audit discipline before any code lands.

### Explicitly out of framework scope

The following are application-layer concerns. `cfw` provides primitives
(signed tokens, crypto, sockets, parsers, static-file confinement) but
will not grow built-in: authentication/authorization policy engines,
multi-tenancy enforcement, database/ORM tooling, templating, GraphQL/RPC
adapters, admin UIs, durable job queues, internationalization, and
payment/CMS/search integrations. If a demonstrated need arises, an ADR
may propose an opt-in module; it must remain dependency-free C11 or
justify a newly approved dependency.
