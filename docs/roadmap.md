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

- Platform capability detection
- Socket, poll/epoll/kqueue/IOCP abstraction design
- File descriptor/handle ownership rules
- Threading, mutex, and condition-variable primitives if required
- Clock, random source, and filesystem boundaries

Exit criteria: documented platform matrix and portable internal APIs.

## Phase 3: Transport and HTTP

- Socket lifecycle and connection management
- HTTP/1.1 request parser and serializer
- Header normalization and validation rules
- Request smuggling resistance
- Body framing, limits, and streaming strategy
- Keep-alive, pipelining, and graceful shutdown behavior

Exit criteria: fuzzed parser, protocol security tests, and bounded resource
behavior.

## Phase 4: Routing and middleware

- Route table design and matching semantics
- Path parameter extraction
- Middleware ordering and cancellation
- Error handling and response pipeline
- Extensibility hooks for applications and plugins

Exit criteria: API stability review and integration tests.

## Phase 5: Server lifecycle

- Configuration model
- Listener management
- Connection limits and timeouts
- Graceful start/stop/reload behavior
- Operational logging and diagnostics

Exit criteria: stability soak tests and DoS audit.

## Phase 6: Optional TLS boundary

- Decide whether TLS is in scope
- If approved, choose a minimal audited dependency or design a separate TLS
  module with explicit ADR and security review

Exit criteria: cryptographic and transport audit completed.

## Phase 7: Hardening and 1.0 readiness

- Full audit catalog pass
- ABI/API freeze review
- Platform matrix verification
- Fuzz corpus stabilization
- Documentation and migration completeness
