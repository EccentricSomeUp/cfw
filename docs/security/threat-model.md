<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Threat model

This is the initial threat model for `cfw`. It will be revised as subsystems are
designed and implemented.

## System context

`cfw` is intended to be a reusable C web server framework. Applications link it
into server processes and expose HTTP services over networks. The framework does
not inherently know the application's authentication, authorization, data, or
business-logic rules.

## Assets

- Integrity and confidentiality of HTTP requests and responses
- Availability of server processes under malicious or malformed traffic
- Memory safety of the framework and applications using it
- Correctness of routing, middleware, and handler dispatch
- Configuration and secret material supplied by the application
- Stability of the public API and ABI for downstream users

## Trust boundaries

1. Remote clients to HTTP parser and protocol layer
2. Network sockets to event loop and connection management
3. Application callbacks/middleware to core services
4. Configuration and environment to server initialization
5. Filesystem or external inputs to any future static/file-serving features
6. Optional plugins/extensions to core lifecycle and request pipeline

## Primary threats

- Malformed HTTP requests causing parser confusion or memory corruption
- Request smuggling or ambiguous message boundaries, including
  response-side framing conflicts (the server strips handler-supplied
  Transfer-Encoding/Content-Length and emits correct framing itself)
- Resource exhaustion through connections, headers, bodies, or slow clients
- Integer overflow in length, offset, allocation, or timeout calculations
- Use-after-free, double-free, or ownership confusion in callbacks
- Race conditions in event loops, timers, threads, or shutdown paths
- Information disclosure through errors, logs, timing, or uninitialized memory
- API misuse by applications leading to unsafe states
- Supply-chain or license violations in dependencies and generated artifacts

## Assumptions

- The application is responsible for authentication and authorization policy.
- Applications using upload hooks (ADR 0033) own the storage backend and any
  additional limits for streamed request bodies; the framework still enforces
  `max_body_size` on total body bytes but no longer bounds their memory.
- The application must validate business-level inputs even when the framework
  validates protocol syntax.
- Operators are responsible for secure deployment, TLS termination choices,
  firewalling, and secret management unless the framework explicitly provides
  those features later.
- Third-party dependencies, if ever approved, must be audited and pinned.

## Security objectives

- All network-facing parsers fail closed and bounded.
- No unbounded allocation is derived directly from attacker-controlled values.
- Public APIs document ownership, thread safety, error behavior, and limits.
- Security-critical state transitions are explicit and testable.
- Every accepted security risk has documented rationale and mitigation.
