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
- [x] Socket, poll/epoll/kqueue/IOCP abstraction design (epoll + poll
  implemented internally; kqueue/IOCP designed, pending backends)
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
- Operational logging and diagnostics — met (counters + optional
  logger events; structured audit logging deferred)

Exit criteria met: bounded soak test (server_soak) and DoS
audit (audits/2026-11-26-http-server-dos-audit.md) with three fixes.

## Phase 6: Optional TLS boundary

- Decide whether TLS is in scope — decided: out of scope for the core
  library (ADR 0030); cleartext transport only, TLS deferred to a
  future optional module pending a dependency ADR
- Boundary preserved: `cfw_http1_conn_t`/`cfw_server_t` consume an
  established byte stream; a future `cfw_tls` module slots below them

Exit criteria: scope decision recorded (ADR 0030); cryptographic and
transport audit deferred with the module itself.

## Phase 7: Hardening and 1.0 readiness

- Full audit catalog pass — met (audits/2026-11-26-1.0-readiness-audit.md)
- ABI/API freeze review — met (docs/api-stability.md; freeze applies at
  the 1.0.0 tag; earlier 0.x tags remain unfrozen)
- Platform matrix verification — met (docs/platform-matrix.md updated to
  implemented state)
- Fuzz corpus stabilization — met (adversarial seeds added:
  framing conflicts, bare LF, obs-fold, chunked extensions,
  upgrade smuggling)
- Documentation and migration completeness — met (README quickstart and
  minimal-server example; migration guide N/A pre-1.0)
