<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0006: Time, duration, and deadline primitives

- Status: Accepted
- Date: 2026-11-25

## Context

Server timeouts, keep-alive deadlines, and elapsed-time accounting require a
monotonic time source and checked duration arithmetic. Wall-clock time is
unsuitable because system clock adjustments can move deadlines unpredictably.
Phase 1 of the roadmap calls for time, duration, and timeout primitives before
any platform or transport code depends on them.

## Decision

Add `include/cfw/time.h` and `src/cfw/time.c` with two value types:

- `cfw_duration_t`: a signed count of nanoseconds (`int64_t`). Negative
  durations are permitted so callers can represent past deadlines and
  backwards intervals.
- `cfw_timepoint_t`: a non-negative nanosecond count on a monotonic clock.
  Timepoints are opaque, process-local values and must not be persisted or
  compared across restarts.

The API provides checked constructors (`cfw_duration_from_nanos`,
`_micros`, `_millis`, `_seconds`), arithmetic (`cfw_duration_add`,
`cfw_duration_sub`, `cfw_timepoint_add`, `cfw_timepoint_sub`,
`cfw_timepoint_diff`), comparison, truncating conversions
(`cfw_duration_to_nanos`, `_micros`, `_millis`), a monotonic clock read
(`cfw_time_now`), and deadline helpers (`cfw_deadline_from_now`,
`cfw_deadline_expired`).

`cfw_checked_add_i64`, `cfw_checked_sub_i64`, and `cfw_checked_mul_i64` are
added to the checked-arithmetic API to support nanosecond arithmetic without
relying on wider integer types, which are not portable to MSVC.

The monotonic clock is `CLOCK_MONOTONIC` via `clock_gettime` on POSIX-like
systems and `QueryPerformanceCounter` on Windows. If neither is available the
clock read returns `CFW_ERROR_UNAVAILABLE`. Conversion to nanoseconds is
checked: results exceeding `INT64_MAX` return `CFW_ERROR_OVERFLOW`, and any
arithmetic producing a negative timepoint fails closed with
`CFW_ERROR_INVALID_ARGUMENT`.

`cfw_duration_to_micros` and `cfw_duration_to_millis` truncate toward zero,
matching C integer division, including for negative durations.

## Alternatives considered

- **Wall-clock (`CLOCK_REALTIME` / `gettimeofday`) timepoints:** rejected
  because NTP adjustments and manual clock changes can expire or extend
  deadlines unpredictably.
- **`struct timespec` as the public representation:** rejected because it is
  POSIX-shaped, complicates Windows support, and invites callers to perform
  unchecked arithmetic on its fields.
- **Opaque deadline objects:** rejected because deadlines are just timepoints;
  a separate type would add API surface without adding safety.
- **Unsigned duration representation:** rejected because negative durations
  are needed for deadlines in the past and interval subtraction.

## Consequences

- Timeout, keep-alive, and elapsed-time code has a portable, checked,
  monotonic foundation.
- Platform clock access is isolated in one translation unit; the Phase 2
  platform-abstraction layer can replace `cfw_time_monotonic_ns` without
  changing the public API.
- `int64_t` nanoseconds overflow after roughly 292 years of uptime or
  duration; representability failures return `CFW_ERROR_OVERFLOW`.
- Integer-overflow, bounds, and API-misuse audits must cover checked `i64`
  arithmetic, clock conversion, negative-duration handling, and deadline
  expiry paths.
