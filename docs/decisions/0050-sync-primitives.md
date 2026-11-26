<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0050: synchronization primitives (mutex and condition variable)

## Status

Accepted.

## Context

The Phase 2 roadmap item "Threading, mutex, and condition-variable
primitives if required" was deferred until the Phase 5 server
lifecycle existed. Phase 5 is now complete: `cfw_server_t` is a
single-threaded event loop by design ("one server per thread"), but
real applications need to share state between the event-loop thread
and application-owned threads — for example handoff of accepted
connections to worker threads, offloaded request processing, or
sharing caches between concurrent `cfw_server_t` instances.

Today there is no portable way to do this: every application would
reinvent mutex/condvar wrappers, and the subtle parts (timed waits on
a monotonic clock, try-lock semantics, platform differences between
pthread and Windows primitives) are exactly where bugs and
portability failures hide.

C11 `threads.h` is not viable: it is an optional C11 feature with
incomplete platform support (absent on some POSIX libcs and all
current Windows toolchains in practice), and the project constraint
requires C11 only without compiler extensions in portable code.

## Decision

New module `cfw/sync.h` + `src/cfw/sync.c` providing:

- `cfw_mutex_t` — non-recursive mutual exclusion:
  `cfw_mutex_init`, `cfw_mutex_destroy`, `cfw_mutex_lock`,
  `cfw_mutex_try_lock` (returns `CFW_ERROR_WOULDBLOCK` when
  contended), `cfw_mutex_unlock`.
- `cfw_cond_t` — condition variable:
  `cfw_cond_init`, `cfw_cond_destroy`, `cfw_cond_wait`,
  `cfw_cond_signal`, `cfw_cond_broadcast`.
- `cfw_cond_wait(cond, mutex, deadline)` takes an optional absolute
  `cfw_timepoint_t` deadline (`NULL` waits indefinitely) and returns
  `CFW_ERROR_TIMEOUT` on expiry, reusing the existing monotonic-clock
  primitives so timeout semantics match the rest of the framework.

Storage is fixed-size inline storage inside the public structs
(sized well above `pthread_mutex_t`/`pthread_cond_t` on supported
platforms, with `static_assert`s in the implementation to prove it).
This keeps the primitives allocation-free, trivially copyable-by-value
for setup, and free of hidden lifetime rules — consistent with
`cfw_fd_t`'s owning-handle style rather than heap-backed opaque
types.

Backends:

- POSIX (Linux, macOS, *BSD): `pthread_mutex_*` / `pthread_cond_*`,
  linked via CMake `Threads::Threads`. Timed waits convert the
  monotonic `cfw_timepoint_t` deadline to a `CLOCK_REALTIME`
  abstime internally, so no `pthread_condattr_setclock` dependence.
- Windows: `SRWLOCK` + `CONDITION_VARIABLE`
  (`SleepConditionVariableSRW`), which provide the same
  non-recursive, deadline-capable semantics without CRT thread
  dependencies.

Explicitly **out of scope**: thread creation/join, thread-local
storage, barriers, spinlocks, once-init, and C11 `threads.h`. The
server remains single-threaded; these primitives serve
application-owned concurrency. A `cfw_thread_t` spawn/join API is
deferred until a concrete in-tree consumer exists — adding it now
would commit to stack-size, cancellation, and detached-thread
semantics without a use case to validate them against.

## Consequences

- New public header `cfw/sync.h` (Tier 2 API addition; additive only,
  no existing symbol changes).
- `libcfw` gains a link dependency on the platform thread library
  (`Threads::Threads` on POSIX, `kernel32`/`synchronization` already
  implicit on Windows). This is an OS facility, not a third-party
  dependency, so no supply-chain ADR is required.
- Timed cond waits inherit `cfw_time_now`'s monotonic-clock
  guarantees; spurious wakeups remain possible per the underlying
  primitive contract, so callers must always re-check predicates —
  documented in the header.
- Concurrency audit (audit catalog §5) and API-misuse audit (§4)
  apply; results recorded under `audits/`.
