<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0008: Logging facade

- Status: Accepted
- Date: 2026-11-25

## Context

Phase 1 of the roadmap requires a logging facade with rate limiting and
redaction rules before server subsystems exist. Logging must be bounded
(attacker-influenced messages must not cause unbounded allocation or I/O),
usable from multiple threads, and must never be a channel for secret
material.

## Decision

Add `include/cfw/log.h` and `src/cfw/log.c` with an explicit logger object:

- `cfw_log_level_t`: TRACE through FATAL ordering.
- `cfw_log_sink_fn`: a typed callback receiving `(level, message view,
  user_data)`. The message view is borrowed and valid only for the call.
- `cfw_logger_t`: a public value type initialized by `cfw_logger_init`,
  configured via `cfw_logger_set_sink`, `cfw_logger_set_min_level`, and
  `cfw_logger_set_rate_limit`.
- `cfw_log`: printf-style formatted logging into a fixed 1024-byte stack
  buffer; overlong output is truncated and marked with a trailing `...`.
- `cfw_log_message`: emits a caller-provided `cfw_strview_t` unchanged.
- `cfw_logger_dropped_count`: returns messages suppressed by the rate
  limiter.

Filtering order: invalid arguments return `CFW_ERROR_INVALID_ARGUMENT`;
messages below `min_level` or logged while no sink is installed return
`CFW_OK` without side effects; the rate limiter then applies before any
formatting work.

The rate limiter is an approximate fixed one-second window shared by all
threads, implemented with C11 atomics on the logger. It bounds emission rate
globally per logger, not per call site. If the monotonic clock is
unavailable the limiter fails open so logging never disappears silently due
to a clock fault; this is documented behavior.

Redaction policy: the facade performs no redaction. Callers must never
format secrets, credentials, tokens, or key material into messages; sinks
must not retain borrowed message views. Structured logging with
enforced key-based redaction is deferred to a future ADR.

## Alternatives considered

- **Global logger singleton:** rejected because hidden global state conflicts
  with the architecture's extensibility rules and makes tests and embedded
  use fragile.
- **Heap-allocated opaque logger:** rejected for now; the public value type
  avoids an allocation pair and matches the span/buffer conventions.
- **Unbounded formatted messages:** rejected because attacker-influenced
  format arguments could force large allocations or I/O.
- **Per-call-site or per-level rate limits:** deferred; a single global
  window is sufficient for v1 and can be refined without breaking the API.
- **Built-in redaction now:** rejected because meaningful redaction requires
  structured fields, which is a larger API decision deferred to its own ADR.

## Consequences

- All subsystems gain bounded, thread-tolerant diagnostics without depending
  on stdio directly.
- Sinks run synchronously on the caller's thread; slow sinks stall callers,
  which is documented in the sink contract.
- Rate limiting is approximate under concurrency; exact per-window counts
  are not guaranteed and must not be relied upon.
- Concurrency, cancellation, and DoS audits must cover the atomic window
  logic, sink reentrancy rules, truncation path, and dropped-count behavior.
