<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Long-term vision

This document records exploratory directions for `cfw` beyond the staged
phases in `docs/roadmap.md`. Nothing here is a commitment: every item is a
research candidate that would require its own ADR, threat-model review, and
audit pass before implementation. The purpose of this document is to keep
the ambition explicit and to filter future proposals against the same
boundaries that govern the roadmap.

The goal is a framework that competes with — and where possible surpasses —
mainstream web frameworks in other languages on functionality, usability,
security, stability, and performance, while remaining dependency-free C11 at
its core.

## Guiding principles

- **Secure core, optional breadth.** The core stays small, auditable, and
  dependency-free. Anything carrying application policy, persistence, or a
  large API surface ships only as an opt-in module or remains an
  application concern.
- **Pluggable backends.** Where a capability needs an external
  implementation (TLS, compression, QUIC, tracing, …), `cfw` defines a
  stable internal contract and ships exactly one best-fit backend first.
  Over time the contract should admit interchangeable backends so
  deployments can choose per environment. Where feasible and responsible,
  an independent in-tree implementation may be offered as an additional,
  explicitly riskier option.
- **Flexibility on the inside, simplicity on the outside.** Implementation
  complexity may grow substantially; the user-facing surface must not.
  Optionality is expressed through modules and build options, never through
  configuration the average user must understand.
- **Verification is a feature.** Assurance claims (audits, fuzzing,
  formal methods, reproducible builds) are part of the product, not
  internal hygiene.

## Assurance and security

- **Formal verification of the wire parser.** Apply bounded model checking
  (for example CBMC) to the HTTP/1.1 parser, chunked decoder, request
  smuggling invariants, and the WebSocket frame state machine. No mainstream
  web framework can credibly claim its wire parser is machine-checked; this
  would be a durable differentiator and a real security gain. The
  explicit-state-machine style already used in `src/cfw/http*` is close to
  verification-friendly.
- **Capability-based request security.** Ship unforgeable, request-scoped
  capability tokens built on `cfw_signed` envelopes so applications can
  pass authority between middleware and handlers without ambient globals.
  Distinct from an authz engine (which stays out of scope): this is a
  primitive, not a policy.
- **Memory-safety-enforced build modes.** Offer optional Clang
  `-fbounds-safety` or Checked-C-compatible build modes, ideally also as a
  supported way for applications to compile against `cfw` headers. Span and
  checked-arithmetic primitives already align with this philosophy; `cfw`
  could become the C framework that makes memory safety opt-in for
  embedders.
- **Deterministic simulation testing.** A virtual-clock + virtual-network
  harness so concurrency, timeout, and shutdown logic is testable
  deterministically. This makes "stability" claims falsifiable rather than
  aspirational.
- **Supply-chain transparency.** Reproducible builds, signed release
  artifacts, published SBOMs, and a public audit ledger linking every
  `audits/` entry to release tags — extending phase 11 from documentation
  into verifiable artifacts.

## Performance and operations

- **QUIC / HTTP/3.** The modern answer to TCP head-of-line blocking and
  0-RTT. QUIC is a full userspace transport (crypto, loss recovery,
  migration), so this likely means hosting an approved QUIC library behind
  a `cfw` transport contract rather than implementing it in-tree. An ADR
  should decide whether `cfw` hosts QUIC at all versus recommending
  termination upstream.
- **WASM component hosting.** `cfw` as the host runtime for sandboxed
  middleware/plugins (Wasmtime-style). Hot-pluggable middleware without ABI
  risk. Ambitious and genuinely novel for a C framework; the dependency and
  sandboxing implications need careful review.
- **eBPF observability hooks.** An optional Linux module exposing
  connection lifecycle events to eBPF for zero-overhead external monitoring.
- **Embedded/RTOS profile.** `CFW_PROFILE=embedded`: no threads, static
  arena only, no malloc, polling-only — a freestanding-ish build for IoT
  gateways and constrained deployments. Nobody serves this niche well.
- **Pluggable persistence contracts.** Not an ORM or job queue (out of
  scope), but minimal key-value/session-store interfaces so opt-in modules
  (sessions, rate-limit stores, cache) can share one contract.

## Ecosystem and reach

- **Agent-facing interfaces.** Structured machine-readable error catalogs
  (extending the status-code and audit schema work), OpenAPI/AsyncAPI spec
  generation from the route table, and possibly a small agent-tool protocol
  endpoint. Frameworks are increasingly competing on agent ergonomics.
- **Safe language bindings.** Generated headers plus thin Rust/Go/Python
  bindings so the security-critical core is adoptable by ecosystems that
  cannot write C. Bindings must preserve the ownership and lifetime
  contracts exactly; they are generated, never hand-maintained forks.
- **ACME automation.** An ACME client module (HTTPS + JSON — largely
  covered by the existing TLS transport and HTTP client) for
  Let's Encrypt-style certificate issuance and renewal. Large operational
  win, small protocol surface.
- **HTTP caching semantics.** A cache middleware implementing RFC 9111
  correctly the first time: ETag/Last-Modified interplay, `Vary`, `Age`,
  validation and invalidation. Incumbents bolted caching on; `cfw` can make
  it a first-class, correct module.
- **Reverse proxy as a showcase.** The phase-12 proxy module doubles as the
  canonical example of composing client, router, middleware, and TLS —
  documentation value beyond the feature itself.

## Explicit non-goals

The boundary in `docs/roadmap.md` still applies: no authentication/
authorization policy engines, ORM or persistence policy, templating,
GraphQL/RPC adapters, admin UIs, durable job queues, internationalization,
or CMS/payment/search integrations. Wild ideas that fit are the ones that
extend primitives, transports, assurance, and operability; ideas that embed
application policy do not.

## Evaluation criteria

Any proposal drawing from this document should be assessed on:

1. Does it preserve the "secure core, optional breadth" rule?
2. Does it fit behind an existing or cleanly definable internal contract?
3. Can it be added without breaking the frozen `1.x` ABI (additive only)?
4. Is the dependency story consistent with the pluggable-backend policy —
   one approved backend first, interchangeability as the growth path?
5. Does it have a credible audit and verification plan before merge?
