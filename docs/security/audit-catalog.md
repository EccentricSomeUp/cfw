<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit catalog

This catalog defines recurring audit goals. Audits may be manual, automated, or
both. Each audit should record scope, method, findings, severity, disposition,
and verification evidence.

## 1. Memory safety audit

Goal: detect buffer overflows, underflows, use-after-free, double-free,
uninitialized reads, invalid frees, and ownership/lifetime mistakes.

Focus areas:

- allocation and deallocation paths
- parser buffers and cursor arithmetic
- callback ownership transfers
- error cleanup paths
- platform abstraction resource handles

Methods: code review, ASan, UBSan, Valgrind where available, targeted stress
tests, and fuzzing for parsers.

## 2. Integer and bounds audit

Goal: detect signedness errors, overflow, truncation, off-by-one errors,
negative-to-unsigned conversion, and unsafe length/offset arithmetic.

Focus areas:

- HTTP header and body lengths
- buffer capacity and remaining-space calculations
- timeout and size-limit conversions
- index and slice operations
- allocation-size multiplication

Methods: boundary-value tests, static review, UBSan, fuzzing, and targeted
property tests.

## 3. Parser and protocol audit

Goal: verify parsers are deterministic, bounded, fail closed, and resistant to
request smuggling, desynchronization, ambiguous encodings, and state confusion.

Focus areas:

- HTTP request line and headers
- chunked transfer coding
- content-length handling
- connection reuse and pipelining
- websocket or upgrade paths if added later

Methods: grammar review, differential tests, malformed-input corpus, fuzzing,
and protocol conformance tests.

## 4. API misuse audit

Goal: ensure public APIs are hard to misuse and document all preconditions,
postconditions, ownership, thread safety, and error behavior.

Focus areas:

- NULL and invalid enum/flag handling
- double initialization and cleanup
- callback reentrancy
- object lifetime across asynchronous operations
- ABI-visible structures and opaque types

Methods: API review, misuse tests, documentation review, and ABI checks.

## 5. Concurrency and cancellation audit

Goal: detect data races, deadlocks, cancellation leaks, thread-unsafe global
state, and shutdown races.

Focus areas:

- event loop ownership
- timers and deferred callbacks
- worker/thread pools if added
- graceful shutdown and connection draining
- signal handling boundaries

Methods: code review, stress tests, TSan where supported, and deterministic
interleaving tests where practical.

## 6. Denial-of-service audit

Goal: verify attacker-controlled inputs cannot cause unbounded CPU, memory,
disk, file-descriptor, or connection consumption.

Focus areas:

- slowloris-style behavior
- header/body size limits
- compression or encoding amplification if added
- logging and error-path amplification
- connection and request rate limits

Methods: resource-limit tests, load tests, timeout tests, and code review of
allocation and loop bounds.

## 7. Platform portability audit

Goal: verify behavior across supported platforms and isolate OS-specific code.

Focus areas:

- socket and polling APIs
- file descriptor vs socket handle types
- endianness and alignment
- path handling and filesystem APIs
- compiler-specific warnings and undefined behavior

Methods: platform matrix builds, static analysis, code review, and CI/manual
verification on supported platforms.

## 8. Cryptography and transport audit

Goal: ensure any future TLS, cookie signing, token, or cryptographic feature uses
reviewed primitives and safe protocols.

Focus areas:

- certificate validation
- key and secret handling
- protocol downgrade resistance
- random number generation
- constant-time comparisons where required

Methods: cryptographic design review, dependency audit, known-answer tests, and
external review for high-risk choices.

## 9. Supply-chain and license audit

Goal: verify the project remains dependency-free unless a dependency is
explicitly approved, pinned, licensed compatibly, and audited.

Focus areas:

- source and binary dependencies
- generated files and vendored code
- compiler and CMake requirements
- license headers and SPDX identifiers
- release artifact contents

Methods: dependency inventory, license scan, build log review, and repository
search for unexpected files.

## 10. Documentation and ABI audit

Goal: ensure docs match implementation and public API/ABI changes are
deliberate.

Focus areas:

- public headers and exported symbols
- version and ABI macros
- ownership and thread-safety documentation
- migration notes for breaking changes
- security assumptions in docs

Methods: API diff review, symbol inspection, documentation review, and package
consumption tests.
